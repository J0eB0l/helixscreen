// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "storage_report.h"

#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;
}

namespace helix::storage {

/// Fetches the storage report from the plugin or, without it, lists the
/// known directories over the file API and sums them; runs a consumer's
/// cleanup either way. Callbacks run on the main thread, and never after the
/// service is destroyed. One request at a time: a new fetch or cleanup
/// abandons the one in flight.
class StorageReportService {
  public:
    using ReportCallback =
        std::function<void(ReportSource source, std::optional<StorageReport> report)>;
    /// Bytes freed, or the error message of a cleanup that failed.
    using CleanupCallback =
        std::function<void(std::optional<uint64_t> freed, const std::string& error)>;

    explicit StorageReportService(IMoonrakerClient* client) : client_(client) {}

    /// @p link_dir is the plugin's link directory (empty for its default).
    void fetch(ReportSource source, const std::string& link_dir, ReportCallback done);

    /// Remove consumer @p index's files. The fallback removes only files the
    /// last FileApi fetch listed, and never @p printing_file (gcodes-relative,
    /// empty while nothing prints).
    void cleanup(ReportSource source, std::size_t index, const std::string& printing_file,
                 CleanupCallback done);

  private:
    struct Pending {
        Root root;
        std::string rel; ///< relative to the root, empty for the root itself
    };
    struct Known {
        std::string path; ///< root-prefixed, as server.files.delete_file takes it
        ListedFile file;
    };

    void list_next();
    void on_listing(const Pending& dir, const std::optional<DirectoryListing>& listing);
    void finish_scan();
    void delete_next();

    IMoonrakerClient* client_;
    AsyncLifetimeGuard lifetime_;

    // Fallback scan in flight
    ReportCallback report_done_;
    std::string link_dir_;
    std::deque<Pending> pending_;
    std::size_t listed_dirs_ = 0;
    StorageReport scan_;

    /// Files the last fallback scan found, per consumer
    std::array<std::vector<Known>, kConsumerCount> known_{};

    // Fallback cleanup in flight
    std::vector<Known> to_delete_;
    uint64_t freed_ = 0;
    CleanupCallback cleanup_done_;
};

/// Read the gcodes volume's free space and answer whether an upload of
/// @p size bytes may go ahead (upload_space). @p done runs on the main thread
/// while @p token is live. Unknown free space answers Unknown, which callers
/// treat as Ok.
void check_upload_space(IMoonrakerClient* client, uint64_t size, LifetimeToken token,
                        std::function<void(UploadSpace)> done);

} // namespace helix::storage
