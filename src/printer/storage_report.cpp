// SPDX-License-Identifier: GPL-3.0-or-later

#include "storage_report.h"

#include "json_utils.h"

namespace helix::storage {

namespace {

constexpr std::string_view kDefaultLinkDir = ".helix_print";
constexpr std::string_view kIndexDir = ".helix_index";
constexpr std::string_view kPhotosDir = ".helix_photos";
constexpr std::string_view kThumbsDir = ".thumbs";
constexpr std::string_view kIndexSuffix = ".layers.jsonl";

std::size_t index_of(std::string_view key) {
    return consumer_index(key).value_or(0);
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::optional<uint64_t> byte_count(const nlohmann::json& obj, const char* key) {
    const nlohmann::json* v = json_util::find_member(obj, key);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    return json_util::as_uint64(*v);
}

} // namespace

std::optional<std::size_t> consumer_index(std::string_view key) {
    for (std::size_t i = 0; i < kConsumers.size(); ++i) {
        if (key == kConsumers[i].key) {
            return i;
        }
    }
    return std::nullopt;
}

ReportSource report_source(PluginFeatureState storage) {
    return storage == PluginFeatureState::Ready ? ReportSource::Plugin : ReportSource::FileApi;
}

std::optional<StorageReport> parse_storage_result(const nlohmann::json& result) {
    const nlohmann::json* consumers = json_util::find_member(result, "consumers");
    if (consumers == nullptr || !consumers->is_array()) {
        return std::nullopt;
    }
    StorageReport report;
    if (const nlohmann::json* volume = json_util::find_member(result, "volume");
        volume != nullptr && volume->is_object()) {
        report.total = byte_count(*volume, "total");
        report.free = byte_count(*volume, "free");
    }
    for (const nlohmann::json& entry : *consumers) {
        const auto index = consumer_index(json_util::safe_string(entry, "name"));
        if (!index) {
            continue; // a consumer a newer plugin reports and this app cannot name
        }
        ConsumerUsage& usage = report.consumers[*index];
        usage.bytes = json_util::safe_uint64(entry, "bytes");
        usage.files = json_util::safe_uint64(entry, "files");
        usage.available = json_util::safe_bool(entry, "available");
        usage.cleanable = usage.available && kConsumers[*index].cleanable &&
                          json_util::safe_bool(entry, "cleanable");
    }
    return report;
}

std::size_t classify_gcodes_path(std::string_view rel, std::string_view link_dir) {
    const std::size_t slash = rel.find('/');
    if (slash != std::string_view::npos) {
        const std::string_view top = rel.substr(0, slash);
        const std::string_view link = link_dir.empty() ? kDefaultLinkDir : link_dir;
        if (top == ".helix_temp" || top == "helix_staging" || top == link) {
            return index_of("helix_temp");
        }
        if (top == kIndexDir) {
            return index_of("analysis_sidecars");
        }
        if (top == kPhotosDir) {
            return index_of("photos");
        }
        // Any directory segment named .thumbs, at any depth
        std::size_t start = 0;
        for (std::size_t end = slash; end != std::string_view::npos; end = rel.find('/', start)) {
            if (rel.substr(start, end - start) == kThumbsDir) {
                return index_of("thumbnails");
            }
            start = end + 1;
        }
    }
    return index_of("user_gcode");
}

std::optional<DirectoryListing> parse_directory_result(const nlohmann::json& result) {
    // Moonraker's get_directory answers {dirs, files, disk_usage}; a flat
    // array of {path, size, modified} is server.files.list's shape, which some
    // servers (and the mock, for non-gcodes roots) answer instead.
    const bool flat = result.is_array();
    const nlohmann::json* files = flat ? &result : json_util::find_member(result, "files");
    if (files == nullptr || !files->is_array()) {
        return std::nullopt;
    }
    DirectoryListing listing;
    for (const nlohmann::json& f : *files) {
        ListedFile file;
        file.name = json_util::safe_string(f, flat ? "path" : "filename");
        if (file.name.empty()) {
            continue;
        }
        file.size = json_util::safe_uint64(f, "size");
        file.modified = json_util::safe_double(f, "modified");
        listing.files.push_back(std::move(file));
    }
    if (flat) {
        return listing;
    }
    if (const nlohmann::json* dirs = json_util::find_member(result, "dirs");
        dirs != nullptr && dirs->is_array()) {
        for (const nlohmann::json& d : *dirs) {
            std::string name = json_util::safe_string(d, "dirname");
            if (!name.empty() && name != "." && name != "..") {
                listing.dirs.push_back(std::move(name));
            }
        }
    }
    if (const nlohmann::json* du = json_util::find_member(result, "disk_usage");
        du != nullptr && du->is_object()) {
        listing.total = byte_count(*du, "total");
        listing.free = byte_count(*du, "free");
    }
    return listing;
}

bool fallback_removes(std::size_t index, const ListedFile& file, double now, double older_than,
                      std::string_view printing_file) {
    if (index >= kConsumers.size() || !kConsumers[index].cleanable) {
        return false;
    }
    const std::string_view key = kConsumers[index].key;
    if (!printing_file.empty() && (key == "helix_temp" || file.name == printing_file)) {
        return false;
    }
    double min_age = older_than;
    if (key == "helix_temp" && min_age < kScratchMinAgeS) {
        min_age = kScratchMinAgeS;
    }
    if (key == "logs" && ends_with(file.name, ".log")) {
        return false; // a log a service is still writing
    }
    if (key == "analysis_sidecars" && !ends_with(file.name, kIndexSuffix)) {
        return false; // a worker's partial index
    }
    return now - file.modified >= min_age;
}

UploadSpace upload_space(std::optional<uint64_t> free_bytes, uint64_t size) {
    if (!free_bytes) {
        return UploadSpace::Unknown;
    }
    if (*free_bytes < size || *free_bytes - size < kFreeSpaceFloorBytes) {
        return UploadSpace::TooFull;
    }
    return UploadSpace::Ok;
}

} // namespace helix::storage
