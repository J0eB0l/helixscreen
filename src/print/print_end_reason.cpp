// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_end_reason.h"

#include "error_event.h"
#include "firmware_error_envelope.h"
#include "gcode_response_lines.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace helix {

namespace {

double wall_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

bool is_bad_end(PrintOutcome outcome) {
    return outcome == PrintOutcome::CANCELLED || outcome == PrintOutcome::ERROR;
}

} // namespace

std::optional<std::string> last_gcode_error_text(const nlohmann::json& msg) {
    std::optional<std::string> out;
    for_each_gcode_response_line(msg, [&out](const std::string& line) {
        if (parse_gcode_error_line(line).prefix == GcodeErrorPrefix::Bang) {
            out = decode_firmware_error_text(journal_error_text(line));
        }
    });
    if (out && out->empty()) {
        return std::nullopt;
    }
    return out;
}

std::string decide_print_end_reason(PrintOutcome outcome, const std::string& print_message,
                                    const std::vector<JournalEntry>& entries, double end_time) {
    if (!is_bad_end(outcome)) {
        return {};
    }
    if (!print_message.empty()) {
        return print_message;
    }
    const JournalEntry* newest = nullptr;
    for (const auto& e : entries) {
        if (e.kind != journal_kind::kGcodeError || e.text.empty() ||
            !is_recent_cause(e.time, end_time)) {
            continue;
        }
        if (newest == nullptr || e.time >= newest->time) {
            newest = &e;
        }
    }
    return newest ? newest->text : std::string();
}

PrintEndReasonTracker::PrintEndReasonTracker(IMoonrakerClient& client, PrinterState& state,
                                             std::function<double()> now)
    : client_(client), state_(state), now_(now ? std::move(now) : wall_now) {
    // The prefilter runs on the WebSocket thread, so only a frame that carries
    // an error reaches the update queue.
    auto token = lifetime_.token();
    client_.register_method_callback(
        "notify_gcode_response", kHandlerName, [this, token](const nlohmann::json& msg) {
            auto text = last_gcode_error_text(msg);
            if (!text) {
                return;
            }
            JournalEntry e;
            e.kind = journal_kind::kGcodeError;
            e.time = now_();
            e.text = std::move(*text);
            token.defer("PrintEndReason::gcode_error",
                        [this, e = std::move(e)]() mutable { on_gcode_error(std::move(e)); });
        });

    auto& ps = state_.print_state();
    // Immediate: the reason is in place before the lifecycle publishes the end,
    // so the toast and modal that observer raises can read it.
    outcome_observer_ = ui::observe<int>(
        ps.get_print_outcome_subject(), this,
        [](PrintEndReasonTracker* self, int outcome) { self->on_outcome(outcome); },
        state_.get_subjects_lifetime(), ui::Dispatch::Immediate);
    message_observer_ = ui::observe<const char*>(
        ps.get_print_message_subject(), this,
        [](PrintEndReasonTracker* self, const char*) { self->publish(); },
        state_.get_subjects_lifetime());
}

PrintEndReasonTracker::~PrintEndReasonTracker() {
    lifetime_.invalidate();
    client_.unregister_method_callback("notify_gcode_response", kHandlerName);
}

bool PrintEndReasonTracker::ended_badly() const {
    return is_bad_end(static_cast<PrintOutcome>(outcome_));
}

void PrintEndReasonTracker::on_outcome(int outcome) {
    if (outcome == outcome_) {
        return;
    }
    outcome_ = outcome;
    recovered_.clear();
    end_message_.clear();
    if (!ended_badly()) {
        publish();
        return;
    }
    end_time_ = now_();
    const std::uint32_t end_id = ++end_id_;
    publish();
    if (lv_subject_get_int(state_.print_state().get_print_end_reason_visible_subject()) != 0) {
        return;
    }
    // Nothing seen live explains the end: the error may have come before this
    // screen connected, or before the tap was registered.
    client_.get_gcode_store(
        kGcodeStoreCount,
        lifetime_.bg_cb("PrintEndReason::gcode_store",
                        [this, end_id](const std::vector<GcodeStoreEntry>& store) {
                            on_gcode_store(store, end_id);
                        }),
        [](const MoonrakerError& err) {
            spdlog::debug("[PrintEndReason] gcode_store read failed: {}", err.message);
        });
}

void PrintEndReasonTracker::on_gcode_error(JournalEntry e) {
    last_error_ = std::move(e);
    if (ended_badly()) {
        publish();
    }
}

void PrintEndReasonTracker::on_gcode_store(const std::vector<GcodeStoreEntry>& store,
                                           std::uint32_t end_id) {
    if (end_id != end_id_ || !ended_badly() || store.empty()) {
        return;
    }
    // Moonraker stamps the store with its own clock. Its newest line was
    // written moments before this read, so the lines are placed relative to
    // it rather than compared with the screen's clock.
    double newest = 0.0;
    for (const auto& item : store) {
        newest = std::max(newest, item.time);
    }
    const double shift = now_() - newest;
    recovered_ = backfill_from_gcode_store(store, newest - kEventCauseWindowS - kEventCauseLateS);
    for (auto& e : recovered_) {
        e.time += shift;
        e.text = decode_firmware_error_text(e.text);
    }
    publish();
}

void PrintEndReasonTracker::publish() {
    std::vector<JournalEntry> entries = recovered_;
    if (last_error_) {
        entries.push_back(*last_error_);
    }
    // Klipper clears print_stats.message when the ended job is reset to
    // standby, so the one that explained the end is kept until the next end.
    const char* message = lv_subject_get_string(state_.print_state().get_print_message_subject());
    if (ended_badly() && message && *message) {
        end_message_ = message;
    }
    const std::string reason = decide_print_end_reason(static_cast<PrintOutcome>(outcome_),
                                                       end_message_, entries, end_time_);
    if (!reason.empty()) {
        spdlog::info("[PrintEndReason] {}", reason);
    }
    state_.print_state().set_print_end_reason(reason);
}

} // namespace helix
