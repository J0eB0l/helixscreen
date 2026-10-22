// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_plugin_features.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

/// What fills the printer's disks, by consumer: the HelixPrint plugin's
/// `storage` report when it is ready, else the same breakdown summed from the
/// file API. Pure rules only, exception-free; the fetching lives in
/// storage_report_service.h.
namespace helix::storage {

inline constexpr const char* kStorageFeature = "storage";
inline constexpr int kStorageMinVersion = 1;
inline constexpr const char* kAutoCleanupFeature = "storage_auto_cleanup";
inline constexpr int kAutoCleanupMinVersion = 1;

/// The free space the gcodes volume keeps after a print's copy is written:
/// the plugin's FREE_SPACE_FLOOR_BYTES, which the app's own uploads keep too.
inline constexpr uint64_t kFreeSpaceFloorBytes = 100ULL * 1024 * 1024;

/// A plugin scratch file younger than this may still be on its way to a
/// print; no cleanup removes it.
inline constexpr double kScratchMinAgeS = 600.0;

/// Where a consumer's files live.
enum class Root {
    Gcodes,
    Timelapse,
    Logs,
    Data, ///< Moonraker's data directory, which no file root exposes
};

/// One consumer, in the plugin's report order. Its row on the Storage page
/// (label, icon) lives in settings_storage_overlay.xml.
struct ConsumerInfo {
    const char* key;
    Root root;
    /// Cleanup may remove its files; the rest hold the user's files or state
    /// another feature needs.
    bool cleanable;
    /// The body of the cleanup confirmation, English; the UI translates it
    const char* confirm;
};

inline constexpr std::array<ConsumerInfo, 9> kConsumers = {{
    {"helix_temp", Root::Gcodes, true,
     TR_NOOP("Delete every modified print copy that is not in use? Files you uploaded are not "
             "touched.")},
    {"3mf_outputs", Root::Gcodes, false, ""},
    {"analysis_sidecars", Root::Gcodes, true,
     TR_NOOP("Delete the layer indexes? Previews read each file again until it is analyzed "
             "again.")},
    {"photos", Root::Gcodes, true,
     TR_NOOP("Delete every print photo? Print history keeps its "
             "entries.")},
    {"timelapse", Root::Timelapse, true, TR_NOOP("Delete every timelapse video?")},
    {"logs", Root::Logs, true,
     TR_NOOP("Delete the rotated logs? The logs in use are kept, and debug bundles still work.")},
    {"thumbnails", Root::Gcodes, false, ""},
    {"user_gcode", Root::Gcodes, false, ""},
    {"plr", Root::Data, false, ""},
}};

inline constexpr std::size_t kConsumerCount = kConsumers.size();

/// The index of consumer @p key in kConsumers.
std::optional<std::size_t> consumer_index(std::string_view key);

struct ConsumerUsage {
    uint64_t bytes = 0;
    uint64_t files = 0;
    /// Measured: its root exists here
    bool available = false;
    /// Has a cleanup action now
    bool cleanable = false;
};

struct StorageReport {
    std::optional<uint64_t> total;
    std::optional<uint64_t> free;
    std::array<ConsumerUsage, kConsumerCount> consumers{};
    /// The listing stopped at kMaxFallbackDirectories; sums are a lower bound.
    bool partial = false;
};

enum class ReportSource {
    Plugin,  ///< server.helix.storage
    FileApi, ///< the known directories listed and summed by the app
};

/// Where the Storage page reads from: the plugin's report only when its
/// `storage` feature is Ready.
ReportSource report_source(PluginFeatureState storage);

/// The report in a server.helix.storage result; nullopt when it is not one.
std::optional<StorageReport> parse_storage_result(const nlohmann::json& result);

// ---------------------------------------------------------------------------
// File-API fallback
// ---------------------------------------------------------------------------

/// The fallback lists at most this many directories per report.
inline constexpr std::size_t kMaxFallbackDirectories = 256;

/// The consumer a file under the gcodes root belongs to (the plugin's
/// classify_gcodes_path). @p link_dir is the plugin's link directory, empty
/// for the default `.helix_print`. Without the plugin no file is a .3mf
/// output.
std::size_t classify_gcodes_path(std::string_view rel, std::string_view link_dir);

/// A file of a server.files.get_directory listing.
struct ListedFile {
    std::string name;
    uint64_t size = 0;
    double modified = 0.0;
};

/// A server.files.get_directory result.
struct DirectoryListing {
    std::vector<std::string> dirs;
    std::vector<ListedFile> files;
    std::optional<uint64_t> total;
    std::optional<uint64_t> free;
};

std::optional<DirectoryListing> parse_directory_result(const nlohmann::json& result);

/// Whether the fallback cleanup of consumer @p index removes a listed file
/// (its name relative to the consumer's root): a cleanable consumer, the file
/// at least @p older_than seconds old (and kScratchMinAgeS for print copies),
/// not a live log, not @p printing_file. While anything prints no print copy
/// is removed: which copy a print's link names only the plugin can tell.
/// Mirrors the plugin's cleanup_candidates.
bool fallback_removes(std::size_t index, const ListedFile& file, double now, double older_than,
                      std::string_view printing_file);

/// Whether an upload of @p size bytes may go to the gcodes volume.
enum class UploadSpace {
    Ok,
    TooFull, ///< it would leave less than kFreeSpaceFloorBytes
    Unknown, ///< free space could not be read: the upload goes ahead
};

UploadSpace upload_space(std::optional<uint64_t> free_bytes, uint64_t size);

/// What the app says when it refuses one of its own uploads for space.
inline constexpr const char* kNoUploadSpaceMessage =
    TR_NOOP("Not enough free space on the printer for this file. Free some in Settings > "
            "System > Storage.");

} // namespace helix::storage
