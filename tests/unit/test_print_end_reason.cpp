// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "print_end_reason.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

constexpr double kEnd = 1'700'000'000.0;

JournalEntry gcode_error(const char* text, double time) {
    JournalEntry e;
    e.kind = journal_kind::kGcodeError;
    e.text = text;
    e.time = time;
    return e;
}

json response(std::vector<std::string> lines) {
    return json{{"params", lines}};
}

/// Counts reads and keeps the last continuation so a test can answer it.
class StoreClient : public test::FakeMoonrakerClient {
  public:
    void get_gcode_store(int count,
                         std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                         std::function<void(const MoonrakerError&)>) override {
        ++reads;
        last_count = count;
        reply = std::move(on_success);
    }
    int reads = 0;
    int last_count = 0;
    std::function<void(const std::vector<GcodeStoreEntry>&)> reply;
};

class TrackerFixture : public LVGLTestFixture {
  public:
    TrackerFixture() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        test::set_wire_state(ps, PrintJobState::PRINTING);
        tracker_ = std::make_unique<PrintEndReasonTracker>(client_, ps, [this] { return now_; });
        settle();
    }

    ~TrackerFixture() override {
        tracker_.reset();
        test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        settle();
    }

    static void settle() {
        ui::UpdateQueueTestAccess::drain_all(ui::UpdateQueue::instance());
    }

    static std::string reason() {
        return lv_subject_get_string(
            get_printer_state().print_state().get_print_end_reason_subject());
    }

    static int reason_visible() {
        return lv_subject_get_int(
            get_printer_state().print_state().get_print_end_reason_visible_subject());
    }

    void end(const char* state, const char* message = nullptr) {
        json stats = {{"state", state}};
        if (message) {
            stats["message"] = message;
        }
        get_printer_state().update_from_status(json{{"print_stats", stats}});
        settle();
    }

    double now_ = kEnd;
    StoreClient client_;
    std::unique_ptr<PrintEndReasonTracker> tracker_;
};

} // namespace

TEST_CASE("an event is put down to a cause shortly before it", "[plugin_event_journal]") {
    CHECK(is_recent_cause(kEnd - kEventCauseWindowS, kEnd));
    CHECK_FALSE(is_recent_cause(kEnd - kEventCauseWindowS - 0.5, kEnd));
    CHECK(is_recent_cause(kEnd + kEventCauseLateS, kEnd));
    CHECK_FALSE(is_recent_cause(kEnd + kEventCauseLateS + 0.5, kEnd));
}

TEST_CASE("end reason: Klipper's message wins over a gcode error",
          "[print_completion][end_reason]") {
    const std::vector<JournalEntry> errors = {gcode_error("Probe triggered prior", kEnd - 1)};
    CHECK(decide_print_end_reason(PrintOutcome::ERROR, "MCU 'mcu' shutdown", errors, kEnd) ==
          "MCU 'mcu' shutdown");
}

TEST_CASE("end reason: the newest gcode error in the window explains a canceled or failed print",
          "[print_completion][end_reason]") {
    const std::vector<JournalEntry> errors = {
        gcode_error("Too far out of range", kEnd - 40),
        gcode_error("Retries exceeded", kEnd - 3),
        gcode_error("Quad gantry level failed", kEnd - 1),
    };
    CHECK(decide_print_end_reason(PrintOutcome::CANCELLED, "", errors, kEnd) ==
          "Quad gantry level failed");
    CHECK(decide_print_end_reason(PrintOutcome::ERROR, "", errors, kEnd) ==
          "Quad gantry level failed");
}

TEST_CASE("end reason: an error outside the window, or a completed print, says nothing",
          "[print_completion][end_reason]") {
    const std::vector<JournalEntry> old = {gcode_error("Must home axis first", kEnd - 60)};
    CHECK(decide_print_end_reason(PrintOutcome::CANCELLED, "", old, kEnd).empty());
    CHECK(decide_print_end_reason(PrintOutcome::CANCELLED, "", {}, kEnd).empty());

    const std::vector<JournalEntry> recent = {gcode_error("Must home axis first", kEnd - 1)};
    CHECK(decide_print_end_reason(PrintOutcome::COMPLETE, "", recent, kEnd).empty());
    CHECK(decide_print_end_reason(PrintOutcome::NONE, "Runout", recent, kEnd).empty());
}

TEST_CASE("end reason tap: only !! lines, and the last one of a frame wins",
          "[print_completion][end_reason]") {
    CHECK_FALSE(last_gcode_error_text(response({"// QGL probing", "ok"})).has_value());
    CHECK_FALSE(last_gcode_error_text(response({"Error: unknown command"})).has_value());
    CHECK(last_gcode_error_text(response({"!! First failure", "// note", "!! Second failure"})) ==
          "Second failure");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterState: an observer of a terminal outcome sees the same frame's message",
                 "[state][end_reason]") {
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    test::set_wire_state(state, PrintJobState::PRINTING);

    std::string seen;
    lv_observer_t* obs = lv_subject_add_observer(
        state.print_state().get_print_outcome_subject(),
        [](lv_observer_t* o, lv_subject_t* s) {
            if (lv_subject_get_int(s) == static_cast<int>(PrintOutcome::ERROR)) {
                *static_cast<std::string*>(lv_observer_get_user_data(o)) = lv_subject_get_string(
                    get_printer_state().print_state().get_print_message_subject());
            }
        },
        &seen);
    state.update_from_status(
        json{{"print_stats", {{"state", "error"}, {"message", "Heater extruder not heating"}}}});
    lv_observer_remove(obs);

    CHECK(seen == "Heater extruder not heating");
    test::set_wire_state(state, PrintJobState::STANDBY);
}

TEST_CASE_METHOD(TrackerFixture, "end reason tracker: the tapped error explains a cancel",
                 "[print_completion][end_reason]") {
    now_ = kEnd - 2;
    REQUIRE(client_.fire_notification("notify_gcode_response",
                                      response({"!! Quad gantry level failed"})));
    settle();
    CHECK(reason().empty());

    now_ = kEnd;
    end("cancelled");
    CHECK(reason() == "Quad gantry level failed");
    CHECK(reason_visible() == 1);
    CHECK(client_.reads == 0);

    // Klipper's reset to standby leaves the reason up; a new print clears it.
    end("standby");
    CHECK(reason() == "Quad gantry level failed");
    end("printing");
    CHECK(reason().empty());
    CHECK(reason_visible() == 0);
}

TEST_CASE_METHOD(TrackerFixture, "end reason tracker: Klipper's message outlives its reset",
                 "[print_completion][end_reason]") {
    end("error", "Heater bed not heating at expected rate");
    CHECK(reason() == "Heater bed not heating at expected rate");
    get_printer_state().update_from_status(json{{"print_stats", {{"message", ""}}}});
    settle();
    CHECK(reason() == "Heater bed not heating at expected rate");
}

TEST_CASE_METHOD(TrackerFixture,
                 "end reason tracker: reads the gcode store when the tap saw nothing",
                 "[print_completion][end_reason]") {
    now_ = kEnd - 120;
    client_.fire_notification("notify_gcode_response", response({"!! An old failure"}));
    settle();

    now_ = kEnd;
    end("cancelled");
    CHECK(reason().empty());
    REQUIRE(client_.reads == 1);
    CHECK(client_.last_count == PrintEndReasonTracker::kGcodeStoreCount);

    // Moonraker's clock runs 500s behind the screen's; the store places itself.
    const double host = kEnd - 500;
    std::vector<GcodeStoreEntry> store(3);
    store[0].type = "response";
    store[0].time = host - 30;
    store[0].message = "!! Too old to matter";
    store[1].type = "response";
    store[1].time = host - 2;
    store[1].message = "!! Quad gantry level failed";
    store[2].type = "command";
    store[2].time = host;
    store[2].message = "CANCEL_PRINT";
    client_.reply(store);
    settle();
    CHECK(reason() == "Quad gantry level failed");
}

TEST_CASE_METHOD(TrackerFixture,
                 "end reason tracker: an error arriving just after the end fills in",
                 "[print_completion][end_reason]") {
    end("cancelled");
    CHECK(reason().empty());
    now_ = kEnd + 1;
    client_.fire_notification("notify_gcode_response", response({"!! Probe triggered prior"}));
    settle();
    CHECK(reason() == "Probe triggered prior");
}

TEST_CASE_METHOD(TrackerFixture, "end reason tracker: a completed print has no reason",
                 "[print_completion][end_reason]") {
    client_.fire_notification("notify_gcode_response", response({"!! Unknown command"}));
    settle();
    end("complete");
    CHECK(reason().empty());
    CHECK(client_.reads == 0);
}

TEST_CASE_METHOD(TrackerFixture, "end reason tracker: unregisters its tap",
                 "[print_completion][end_reason]") {
    tracker_.reset();
    CHECK_FALSE(client_.fire_notification("notify_gcode_response", response({"!! x"})));
}
