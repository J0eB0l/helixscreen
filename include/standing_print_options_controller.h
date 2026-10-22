// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keeps the standing print options record (standing_print_options.h) current
// in Moonraker's database, and sends it at a job start some other client made
// when the HelixPrint plugin's print_hooks feature cannot.

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"

#include <optional>
#include <string>

namespace helix {

class IMoonrakerAPI;
class IMoonrakerClient;

class StandingPrintOptionsController {
  public:
    StandingPrintOptionsController();
    ~StandingPrintOptionsController() = default;

    StandingPrintOptionsController(const StandingPrintOptionsController&) = delete;
    StandingPrintOptionsController& operator=(const StandingPrintOptionsController&) = delete;

    /// The options every backend asks for now, rendered.
    static std::string render_now();

    /// History added a job for @p filename. Main thread.
    void on_job_added(const std::string& filename);

  private:
    /// The plugin answered a status probe: a new connection, or its feature
    /// map changed.
    void on_plugin_probed();
    /// Re-render and write the record when it changed. Main thread.
    void refresh_record();
    void subscribe_history(IMoonrakerAPI& api);
    void load_opt_in(IMoonrakerAPI& api);

    ObserverGuard probe_observer_;
    ObserverGuard backend_count_observer_;
    ObserverGuard data_revision_observer_;
    ObserverGuard unload_tools_observer_;

    /// Guards the database and notification replies, which arrive on the
    /// WebSocket thread.
    AsyncLifetimeGuard lifetime_;

    /// The record as last written this connection; nullopt until then.
    std::optional<std::string> written_;
    /// The user's print_hooks opt-in as stored, for when the plugin reports
    /// no such feature.
    bool opted_in_ = false;
    /// The client the history notification is registered on.
    const IMoonrakerClient* subscribed_client_ = nullptr;
};

} // namespace helix
