// SPDX-License-Identifier: GPL-3.0-or-later

#include "storage_report_service.h"

#include "i_moonraker_client.h"
#include "json_utils.h"
#include "moonraker_error.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>

namespace helix::storage {

namespace {

const char* root_name(Root root) {
    switch (root) {
    case Root::Gcodes:
        return "gcodes";
    case Root::Timelapse:
        return "timelapse";
    case Root::Logs:
        return "logs";
    case Root::Data:
        break;
    }
    return "";
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) {
        return a.empty() ? b : a;
    }
    return a + "/" + b;
}

const nlohmann::json& result_of(const nlohmann::json& response) {
    const nlohmann::json* result = json_util::find_member(response, "result");
    return result != nullptr ? *result : response;
}

} // namespace

void StorageReportService::fetch(ReportSource source, const std::string& link_dir,
                                 ReportCallback done) {
    lifetime_.invalidate();
    if (client_ == nullptr) {
        done(source, std::nullopt);
        return;
    }
    if (source == ReportSource::Plugin) {
        client_->send_jsonrpc("server.helix.storage", nlohmann::json::object(),
                              lifetime_.bg_cb("StorageReportService::report",
                                              [done](const nlohmann::json& response) {
                                                  done(ReportSource::Plugin,
                                                       parse_storage_result(result_of(response)));
                                              }),
                              lifetime_.bg_cb("StorageReportService::report_error",
                                              [done](const MoonrakerError& err) {
                                                  spdlog::warn("[Storage] Plugin report failed: {}",
                                                               err.message);
                                                  done(ReportSource::Plugin, std::nullopt);
                                              }),
                              0, /*silent=*/true);
        return;
    }

    report_done_ = std::move(done);
    link_dir_ = link_dir;
    scan_ = StorageReport{};
    listed_dirs_ = 0;
    for (auto& known : known_) {
        known.clear();
    }
    pending_ = {{Root::Gcodes, ""}, {Root::Timelapse, ""}, {Root::Logs, ""}};
    list_next();
}

void StorageReportService::list_next() {
    if (pending_.empty()) {
        finish_scan();
        return;
    }
    if (listed_dirs_ >= kMaxFallbackDirectories) {
        scan_.partial = true;
        pending_.clear();
        finish_scan();
        return;
    }
    const Pending dir = pending_.front();
    pending_.pop_front();
    ++listed_dirs_;
    const std::string path = join_path(root_name(dir.root), dir.rel);
    client_->send_jsonrpc(
        "server.files.get_directory", nlohmann::json{{"path", path}},
        lifetime_.bg_cb("StorageReportService::listing",
                        [this, dir](const nlohmann::json& response) {
                            on_listing(dir, parse_directory_result(result_of(response)));
                        }),
        lifetime_.bg_cb("StorageReportService::listing_error",
                        [this, dir, path](const MoonrakerError& err) {
                            spdlog::debug("[Storage] Listing {} failed: {}", path, err.message);
                            on_listing(dir, std::nullopt);
                        }),
        0, /*silent=*/true);
}

void StorageReportService::on_listing(const Pending& dir,
                                      const std::optional<DirectoryListing>& listing) {
    if (listing) {
        const bool at_root = dir.rel.empty();
        if (at_root) {
            for (std::size_t i = 0; i < kConsumerCount; ++i) {
                const ConsumerInfo& info = kConsumers[i];
                // Without the plugin no file is a .3mf output, and no file root
                // reaches power-loss recovery's directory.
                const bool measured =
                    info.root == dir.root && std::string_view(info.key) != "3mf_outputs";
                if (measured) {
                    scan_.consumers[i].available = true;
                    scan_.consumers[i].cleanable = info.cleanable;
                }
            }
            if (dir.root == Root::Gcodes) {
                scan_.total = listing->total;
                scan_.free = listing->free;
            }
        }
        for (const ListedFile& f : listing->files) {
            const std::string rel = join_path(dir.rel, f.name);
            std::size_t index = 0;
            if (dir.root == Root::Gcodes) {
                index = classify_gcodes_path(rel, link_dir_);
            } else {
                index =
                    consumer_index(dir.root == Root::Timelapse ? "timelapse" : "logs").value_or(0);
            }
            ConsumerUsage& usage = scan_.consumers[index];
            usage.bytes += f.size;
            usage.files += 1;
            if (kConsumers[index].cleanable) {
                ListedFile file = f;
                file.name = rel;
                known_[index].push_back({join_path(root_name(dir.root), rel), std::move(file)});
            }
        }
        for (const std::string& sub : listing->dirs) {
            pending_.push_back({dir.root, join_path(dir.rel, sub)});
        }
    }
    list_next();
}

void StorageReportService::finish_scan() {
    ReportCallback done = std::move(report_done_);
    report_done_ = nullptr;
    const bool any = std::any_of(scan_.consumers.begin(), scan_.consumers.end(),
                                 [](const ConsumerUsage& u) { return u.available; });
    if (done) {
        done(ReportSource::FileApi, any ? std::optional<StorageReport>(scan_) : std::nullopt);
    }
}

void StorageReportService::cleanup(ReportSource source, std::size_t index,
                                   const std::string& printing_file, CleanupCallback done) {
    lifetime_.invalidate();
    if (client_ == nullptr || index >= kConsumerCount || !kConsumers[index].cleanable) {
        done(std::nullopt, "not cleanable");
        return;
    }
    if (source == ReportSource::Plugin) {
        client_->send_jsonrpc(
            "server.helix.storage_cleanup",
            nlohmann::json{{"consumer", kConsumers[index].key}, {"older_than", 0}},
            lifetime_.bg_cb("StorageReportService::cleanup",
                            [done](const nlohmann::json& response) {
                                done(json_util::safe_uint64(result_of(response), "freed"), "");
                            }),
            lifetime_.bg_cb("StorageReportService::cleanup_error",
                            [done](const MoonrakerError& err) { done(std::nullopt, err.message); }),
            0, /*silent=*/true);
        return;
    }

    const double now = static_cast<double>(std::time(nullptr));
    to_delete_.clear();
    const std::string_view held =
        kConsumers[index].root == Root::Gcodes ? std::string_view(printing_file) : "";
    for (const Known& k : known_[index]) {
        if (fallback_removes(index, k.file, now, 0.0, held)) {
            to_delete_.push_back(k);
        }
    }
    known_[index].clear();
    freed_ = 0;
    cleanup_done_ = std::move(done);
    delete_next();
}

void StorageReportService::delete_next() {
    if (to_delete_.empty()) {
        CleanupCallback done = std::move(cleanup_done_);
        cleanup_done_ = nullptr;
        if (done) {
            done(freed_, "");
        }
        return;
    }
    const Known k = to_delete_.back();
    to_delete_.pop_back();
    client_->send_jsonrpc("server.files.delete_file", nlohmann::json{{"path", k.path}},
                          lifetime_.bg_cb("StorageReportService::deleted",
                                          [this, size = k.file.size](const nlohmann::json&) {
                                              freed_ += size;
                                              delete_next();
                                          }),
                          lifetime_.bg_cb("StorageReportService::delete_error",
                                          [this, path = k.path](const MoonrakerError& err) {
                                              spdlog::warn("[Storage] Could not delete {}: {}",
                                                           path, err.message);
                                              delete_next();
                                          }),
                          0, /*silent=*/true);
}

void check_upload_space(IMoonrakerClient* client, uint64_t size, LifetimeToken token,
                        std::function<void(UploadSpace)> done) {
    if (client == nullptr) {
        done(UploadSpace::Unknown);
        return;
    }
    client->send_jsonrpc(
        "server.files.get_directory", nlohmann::json{{"path", "gcodes"}},
        [token, size, done](const nlohmann::json& response) {
            const auto listing = parse_directory_result(result_of(response));
            const UploadSpace verdict = upload_space(listing ? listing->free : std::nullopt, size);
            token.defer("storage::check_upload_space", [done, verdict]() { done(verdict); });
        },
        [token, done](const MoonrakerError& err) {
            spdlog::debug("[Storage] Free space unreadable before upload: {}", err.message);
            token.defer("storage::check_upload_space_error",
                        [done]() { done(UploadSpace::Unknown); });
        },
        0, /*silent=*/true);
}

} // namespace helix::storage
