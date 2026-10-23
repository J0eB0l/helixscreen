// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keeps the filament_spools map (filament_accounting.h) current in Moonraker's
// database for the HelixPrint plugin, and switches the Spoolman active spool at
// a tool change itself when the plugin cannot. Tools a filament backend feeds
// are switched by AmsState::sync_current_loaded_from_backend() on the lane
// load; this covers the tools that print from their own extruder with a spool
// assigned to the tool.

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "filament_accounting.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {

struct ToolInfo;

class FilamentAccountingController {
  public:
    FilamentAccountingController();
    ~FilamentAccountingController() = default;

    FilamentAccountingController(const FilamentAccountingController&) = delete;
    FilamentAccountingController& operator=(const FilamentAccountingController&) = delete;

    /// The feed signals the printer's tools and backends publish now.
    static std::vector<FeedSignal> signals_now();
    /// Whether a filament backend switches the active spool itself.
    static bool backends_manage_active_spool();
    /// Whether the printer's T<n> macros switch the active spool.
    static bool macros_set_active_spool();
    /// What tool @p tool_index feeds from, by its own spool or its lane's.
    static FeedEntry feed_for_tool(const ToolInfo& tool, int tool_index);

    /// The active tool is now @p tool_index. Main thread.
    void on_active_tool_changed(int tool_index);

  private:
    void on_plugin_probed();
    void on_connection_changed(int state);
    /// Rebuild the map and write it when it changed. Main thread.
    void refresh_record();
    void write_record(FallbackSenderRole role);
    /// Switch Moonraker's active spool for @p feed after reading the current one.
    void switch_active_spool(const FeedEntry& feed, const std::vector<int>& mapped);

    ObserverGuard probe_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard backend_count_observer_;
    ObserverGuard data_revision_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard active_tool_observer_;

    /// Guards the database and Spoolman replies, which arrive on the
    /// WebSocket thread.
    AsyncLifetimeGuard lifetime_;

    /// The record as last written this connection; nullopt until then.
    std::optional<std::string> written_;
    /// The rendering last weighed for writing this connection, written or not.
    std::optional<std::string> considered_;
    /// Tool changes count only once this connection's discovery has read the
    /// printer: a tool carried over a reconnect is no change.
    bool armed_ = false;
    int baseline_tool_ = -1;
};

} // namespace helix
