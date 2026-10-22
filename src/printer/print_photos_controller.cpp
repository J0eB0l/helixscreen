// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_photos_controller.h"

#include "app_globals.h"
#include "fallback_sender.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "webcam_selection.h"

#include <spdlog/spdlog.h>

#include <ctime>
#include <memory>
#include <utility>

namespace helix {

using ui::observe;

namespace {
constexpr const char* kHandlerName = "PrintPhotos";

PrintPhotosController::Job job_from(const nlohmann::json& job) {
    PrintPhotosController::Job out;
    out.job_id = json_util::safe_string(job, "job_id");
    out.filename = json_util::safe_string(job, "filename");
    out.start_time = json_util::safe_double(job, "start_time");
    return out;
}
} // namespace

PrintPhotosController::PrintPhotosController(SnapshotFetch fetch) : fetch_(std::move(fetch)) {
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PrintPhotosController* self, int seq) {
            if (seq > 0) {
                self->on_plugin_probed();
            }
        },
        ps.get_subjects_lifetime());
    // print_stats.info's layer is stored after the subject fires, so it is
    // read on the next tick.
    layer_observer_ = observe<int>(
        ps.print_state().get_print_layer_current_subject(), this,
        [](PrintPhotosController* self, int) {
            self->lifetime_.defer("PrintPhotos::layer", [self] {
                const auto& print = get_printer_state().print_state();
                if (print.slicer_reports_layers()) {
                    self->on_slicer_layer(print.slicer_current_layer());
                }
            });
        },
        ps.get_subjects_lifetime());
}

void PrintPhotosController::on_plugin_probed() {
    subscribe_history();
}

void PrintPhotosController::subscribe_history() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    IMoonrakerClient& client = api->get_client();
    if (subscribed_client_ == &client) {
        return;
    }
    subscribed_client_ = &client;
    client.register_method_callback(
        "notify_history_changed", kHandlerName,
        [this, token = lifetime_.token()](const nlohmann::json& data) {
            // bg thread: parse only.
            const nlohmann::json* payload = json_util::notification_payload(data);
            if (payload == nullptr) {
                return;
            }
            const std::string action = json_util::safe_string(*payload, "action");
            const nlohmann::json* job = json_util::find_member(*payload, "job");
            if (job == nullptr || !job->is_object() ||
                (action != "added" && action != "finished")) {
                return;
            }
            Job parsed = job_from(*job);
            if (parsed.job_id.empty()) {
                return;
            }
            std::string status = json_util::safe_string(*job, "status");
            token.defer("PrintPhotos::history",
                        [this, action, parsed = std::move(parsed), status = std::move(status)] {
                            if (action == "added") {
                                on_job_added(parsed);
                            } else {
                                on_job_finished(parsed, status);
                            }
                        });
        });
}

void PrintPhotosController::on_job_added(const Job& job) {
    job_ = job;
    layer_.reset();
    layer_photo_due_ = true;
}

void PrintPhotosController::on_job_finished(const Job& job, const std::string& status) {
    if (job_ && job_->job_id == job.job_id) {
        job_.reset();
    }
    layer_photo_due_ = false;
    layer_.reset();
    if (ends_with_photo(status)) {
        capture(job, kPhotoKindEnd);
    }
}

void PrintPhotosController::on_slicer_layer(int layer) {
    const std::optional<int> previous = layer_;
    layer_ = layer;
    if (!job_ || !layer_photo_due_ || !layer_two_done(previous, layer)) {
        return;
    }
    layer_photo_due_ = false;
    capture(*job_, kPhotoKindLayer);
}

void PrintPhotosController::capture(const Job& job, const std::string& kind) {
    auto& ps = get_printer_state();
    auto& status = ps.plugin_status_state();
    const PluginFeatureState photos = status.plugin_feature(kPhotosFeature, kPhotosMinVersion);
    const bool opted_in =
        plugin_feature_opted_in(status.stored_opt_ins(), kPhotosFeature, /*acts=*/false);
    std::optional<WebcamInfo> cam = webcam::photo_pick(ps.capabilities_state().get_webcams());
    const bool can_capture = fetch_ != nullptr && cam.has_value();
    // Only a photo this instance would take needs to know who else is here.
    if (photo_taker(photos, opted_in, can_capture, FallbackSenderRole::Sender) != PhotoTaker::App) {
        spdlog::debug("[PrintPhotos] Job {} {} photo: {}", job.job_id, kind,
                      photos == PluginFeatureState::Ready ? "the plugin takes it"
                      : !can_capture                      ? "no camera to take it with"
                                                          : "switched off");
        return;
    }
    std::string url = cam->snapshot_url;
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        api->resolve_webcam_url(url);
    }
    resolve_fallback_sender_role(
        lifetime_.token(), [this, job, kind, url, photos, opted_in](FallbackSenderRole role) {
            if (photo_taker(photos, opted_in, true, role) != PhotoTaker::App) {
                spdlog::debug("[PrintPhotos] Job {} {} photo: a local screen takes it", job.job_id,
                              kind);
                return;
            }
            fetch_and_store(job, kind, url);
        });
}

void PrintPhotosController::fetch_and_store(const Job& job, const std::string& kind,
                                            const std::string& url) {
    spdlog::info("[PrintPhotos] Taking the {} photo of job {} from {}", kind, job.job_id, url);
    fetch_(url, [this, token = lifetime_.token(), job, kind](std::string body) {
        // Worker thread: check the bytes, hand them to the main thread.
        if (!is_jpeg(body) || body.size() > kMaxPhotoBytes) {
            spdlog::info("[PrintPhotos] No {} photo for job {}: the snapshot is {}", kind,
                         job.job_id, body.empty() ? "unavailable" : "not a usable JPEG");
            return;
        }
        auto bytes = std::make_shared<std::string>(std::move(body));
        token.defer("PrintPhotos::fetched",
                    [this, job, kind, bytes] { upload(job, kind, std::move(*bytes)); });
    });
}

void PrintPhotosController::upload(const Job& job, const std::string& kind, std::string bytes) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || !api->is_connected()) {
        return;
    }
    const std::string path = photo_path(job.job_id, job.start_time, kind);
    const size_t size = bytes.size();
    api->transfers().upload_file_with_name(
        "gcodes", path, photo_file_name(job.job_id, job.start_time, kind), bytes,
        [this, token = lifetime_.token(), job, kind, path, size] {
            token.defer("PrintPhotos::uploaded",
                        [this, job, kind, path, size] { write_record(job, kind, path, size); });
        },
        [path](const MoonrakerError& err) {
            spdlog::warn("[PrintPhotos] Uploading {} failed: {}", path, err.message);
        });
}

void PrintPhotosController::write_record(const Job& job, const std::string& kind,
                                         const std::string& path, size_t size) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    // Read, add this photo, write back: the record keeps the job's other one.
    auto store = [this, token = lifetime_.token(), job, kind, path,
                  size](const nlohmann::json& existing) {
        nlohmann::json record =
            photo_record_with(existing, job.job_id, job.filename, job.start_time, kind, path, size,
                              static_cast<int64_t>(std::time(nullptr)));
        token.defer("PrintPhotos::store", [this, job, record = std::move(record)] {
            IMoonrakerAPI* api = get_moonraker_api();
            if (api == nullptr) {
                return;
            }
            api->database_post_item(
                kPhotosNamespace, job.job_id, record,
                [this, token = lifetime_.token(), job] {
                    spdlog::info("[PrintPhotos] Photo record of job {} written", job.job_id);
                    token.defer("PrintPhotos::prune", [this] { prune(); });
                },
                [job](const MoonrakerError& err) {
                    spdlog::warn("[PrintPhotos] Writing the record of job {} failed: {}",
                                 job.job_id, err.message);
                });
        });
    };
    api->database_get_item(kPhotosNamespace, job.job_id, store,
                           [store, job](const MoonrakerError& err) {
                               if (!err.is_not_found()) {
                                   spdlog::warn("[PrintPhotos] Reading the record of job {} "
                                                "failed: {}",
                                                job.job_id, err.message);
                                   return;
                               }
                               store(nlohmann::json());
                           });
}

void PrintPhotosController::prune() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    api->database_get_namespace(
        kPhotosNamespace,
        [token = lifetime_.token()](const nlohmann::json& records) {
            std::vector<std::string> drop = photo_records_to_prune(records, kMaxPhotoJobs);
            if (drop.empty()) {
                return;
            }
            std::vector<std::pair<std::string, std::vector<std::string>>> doomed;
            for (const auto& key : drop) {
                const nlohmann::json* record = json_util::find_member(records, key.c_str());
                doomed.emplace_back(key, record != nullptr ? photo_record_paths(*record)
                                                           : std::vector<std::string>{});
            }
            token.defer("PrintPhotos::prune_apply", [doomed = std::move(doomed)] {
                IMoonrakerAPI* api = get_moonraker_api();
                if (api == nullptr) {
                    return;
                }
                spdlog::info("[PrintPhotos] Dropping the photos of {} old jobs", doomed.size());
                for (const auto& [key, paths] : doomed) {
                    for (const auto& path : paths) {
                        api->files().delete_file(
                            "gcodes/" + path, [] {},
                            [path](const MoonrakerError& err) {
                                spdlog::debug("[PrintPhotos] Deleting {} failed: {}", path,
                                              err.message);
                            });
                    }
                    api->database_delete_item(kPhotosNamespace, key);
                }
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[PrintPhotos] Reading the photo records failed: {}", err.message);
        });
}

} // namespace helix
