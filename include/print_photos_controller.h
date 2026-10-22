// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Takes a print's camera photos (print_photos.h) when the HelixPrint plugin's
// photos feature cannot: fetches the default webcam's snapshot, uploads the
// bytes unchanged into kPhotosDir and writes the job's record, the files and
// record the plugin would have written.

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "print_photos.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {

class IMoonrakerClient;

class PrintPhotosController {
  public:
    /// Fetch @p url on a worker thread and call @p done there with the body,
    /// empty on failure. nullptr in a build that cannot fetch a snapshot.
    using SnapshotFetch =
        std::function<void(const std::string& url, std::function<void(std::string)> done)>;

    explicit PrintPhotosController(SnapshotFetch fetch);
    ~PrintPhotosController() = default;

    PrintPhotosController(const PrintPhotosController&) = delete;
    PrintPhotosController& operator=(const PrintPhotosController&) = delete;

    /// A job history knows about. Main thread.
    struct Job {
        std::string job_id;
        std::string filename;
        double start_time = 0.0;
    };

    /// History added @p job. Main thread.
    void on_job_added(const Job& job);
    /// History finished @p job with @p status. Main thread.
    void on_job_finished(const Job& job, const std::string& status);
    /// print_stats.info reported @p layer. Main thread.
    void on_slicer_layer(int layer);

  private:
    void on_plugin_probed();
    void subscribe_history();
    /// Take the photo of @p kind for @p job, if this screen is the one to.
    void capture(const Job& job, const std::string& kind);
    void fetch_and_store(const Job& job, const std::string& kind, const std::string& url);
    void upload(const Job& job, const std::string& kind, std::string bytes);
    void write_record(const Job& job, const std::string& kind, const std::string& path,
                      size_t size);
    /// Keep at most kMaxPhotoJobs records, deleting the photos of the rest.
    void prune();

    SnapshotFetch fetch_;
    ObserverGuard probe_observer_;
    ObserverGuard layer_observer_;

    /// The job history has open, from its `added` event; nullopt otherwise.
    std::optional<Job> job_;
    /// The last print_stats.info layer seen in that job.
    std::optional<int> layer_;
    bool layer_photo_due_ = false;

    /// Guards the notification, fetch and database replies, which arrive off
    /// the main thread.
    AsyncLifetimeGuard lifetime_;
    /// The client the history notification is registered on.
    const IMoonrakerClient* subscribed_client_ = nullptr;
};

} // namespace helix
