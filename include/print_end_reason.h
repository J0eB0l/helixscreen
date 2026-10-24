// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Why a print was canceled or failed. Klipper says so in print_stats.message
// for most failures, but a print canceled after a failed command (a quad gantry
// level that did not converge, say) leaves the message empty and the reason
// only in the `!!` line the command answered with. The tracker remembers the
// last such line and, when it has none, reads Moonraker's gcode_store.
//
// Compiled into the ESP32 firmware: no exceptions, no RTTI.

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "printer_event_journal.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

class IMoonrakerClient;
class PrinterState;
enum class PrintOutcome;

/// The newest `!!` line of a notify_gcode_response frame, without its prefix
/// and decoded like print_stats.message; nullopt when the frame has none.
std::optional<std::string> last_gcode_error_text(const nlohmann::json& msg);

/// The reason a print with @p outcome ended at @p end_time: @p print_message
/// when Klipper set one, else the newest gcode_error among @p entries that
/// is_recent_cause() of the end. Empty for a completed print, or when nothing
/// explains the end.
std::string decide_print_end_reason(PrintOutcome outcome, const std::string& print_message,
                                    const std::vector<JournalEntry>& entries, double end_time);

/// Publishes PrinterPrintState's print_end_reason. Main thread: built,
/// destroyed and run there; the gcode response tap hops to it.
class PrintEndReasonTracker {
  public:
    static constexpr const char* kHandlerName = "print_end_reason";
    /// gcode_store lines read when the tap saw no error near the end.
    static constexpr int kGcodeStoreCount = 30;

    /// @param now Unix seconds; called on the WebSocket thread too.
    PrintEndReasonTracker(IMoonrakerClient& client, PrinterState& state,
                          std::function<double()> now = {});
    ~PrintEndReasonTracker();

    PrintEndReasonTracker(const PrintEndReasonTracker&) = delete;
    PrintEndReasonTracker& operator=(const PrintEndReasonTracker&) = delete;

  private:
    void on_outcome(int outcome);
    void on_gcode_error(JournalEntry e);
    void on_gcode_store(const std::vector<GcodeStoreEntry>& store, std::uint32_t end_id);
    void publish();
    [[nodiscard]] bool ended_badly() const;

    IMoonrakerClient& client_;
    PrinterState& state_;
    std::function<double()> now_;

    int outcome_ = 0;
    double end_time_ = 0.0;
    /// Bumped on every end, so a store read answers only the end that asked.
    std::uint32_t end_id_ = 0;
    std::string end_message_;
    std::optional<JournalEntry> last_error_;
    /// From gcode_store, moved onto the screen's clock.
    std::vector<JournalEntry> recovered_;

    ObserverGuard outcome_observer_;
    ObserverGuard message_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
