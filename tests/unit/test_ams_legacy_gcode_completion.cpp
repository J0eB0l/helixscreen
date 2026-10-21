// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_legacy_gcode_completion.cpp
 * @brief The callback-taking execute_gcode() forms run their callbacks on the
 *        main thread, behind earlier status frames (#1761).
 *
 * The real MoonrakerAPI answers on the libhv response thread, while the
 * backend applies status frames on the main thread through the UpdateQueue. A
 * completion run inline can judge state those queued frames are about to
 * replace. The mock client answers inside the send, so a callback that has
 * already run when execute_gcode() returns ran inline.
 */

#include "ui_update_queue.h"

#include "../test_helpers/mock_printer.h"
#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "moonraker_client_mock.h"
#include "test_helpers/moonraker_client_mock_test_access.h"
#include "test_helpers/update_queue_test_access.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// A real ToolChanger backend talking to the mock client through the real
/// MoonrakerAPI. Nothing overrides execute_gcode(), so every send takes the
/// production path.
class LiveToolChanger : public helix::AmsBackendToolChanger {
  public:
    explicit LiveToolChanger(MockPrinter& p) : helix::AmsBackendToolChanger(&p.api, &p.client) {
        set_discovered_tools({"T0", "T1", "T2", "T3", "T4"});
        running_ = true;
    }

    ~LiveToolChanger() override {
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Put tool @p n on the carriage with the toolchanger idle.
    void seat_tool(int n) {
        handle_status_update(
            json{{"method", "notify_status_update"},
                 {"params",
                  json::array(
                      {json{{"toolchanger", {{"status", "ready"}, {"tool_number", n}}}}, 0.0})}});
        drain();
    }

    [[nodiscard]] bool toolhead_homed() const override {
        return true;
    }
};

/// Records whether a callback ran and which UpdateQueue callback it ran from.
struct Witness {
    bool ran = false;
    std::string ran_from;

    std::function<void()> on_complete() {
        return [this]() { record(); };
    }
    std::function<void(const MoonrakerError&)> on_error() {
        return [this](const MoonrakerError&) { record(); };
    }

  private:
    void record() {
        ran = true;
        const char* tag = helix::ui::UpdateQueue::current_callback_tag();
        ran_from = tag ? tag : "";
    }
};

void refuse_every_script(MockPrinter& p) {
    helix::MoonrakerClientMockTestAccess::set_method_handler(
        p.client, "printer.gcode.script",
        [](MoonrakerClientMock*, const json&, std::function<void(const json&)>,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
            if (error_cb) {
                error_cb(MoonrakerError::unknown("Unknown command", "printer.gcode.script"));
            }
            return true;
        });
}

} // namespace

TEST_CASE("Legacy execute_gcode completion runs from the UpdateQueue, not the answering thread",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    Witness w;

    REQUIRE(backend.execute_gcode("M400", w.on_complete()).success());
    // The mock answered inside that call; completion waits for the queue.
    CHECK_FALSE(w.ran);
    LiveToolChanger::drain();
    REQUIRE(w.ran);
    CHECK(w.ran_from == "AmsSubscriptionBackend::execute_gcode_complete");
}

TEST_CASE("Legacy execute_gcode with an error leg completes from the UpdateQueue",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    Witness complete;
    Witness error;

    REQUIRE(backend.execute_gcode("M400", complete.on_complete(), error.on_error()).success());
    CHECK_FALSE(complete.ran);
    LiveToolChanger::drain();
    REQUIRE(complete.ran);
    CHECK(complete.ran_from == "AmsSubscriptionBackend::execute_gcode_complete");
    CHECK_FALSE(error.ran);
}

TEST_CASE("Legacy execute_gcode error runs from the UpdateQueue, not the answering thread",
          "[ams][toolchanger][threading][1761]") {
    MockPrinter printer;
    refuse_every_script(printer);
    LiveToolChanger backend(printer);
    Witness complete;
    Witness error;

    REQUIRE(backend.execute_gcode("M400", complete.on_complete(), error.on_error()).success());
    CHECK_FALSE(error.ran);
    LiveToolChanger::drain();
    REQUIRE(error.ran);
    CHECK(error.ran_from == "AmsSubscriptionBackend::execute_gcode_error");
    CHECK_FALSE(complete.ran);
}

TEST_CASE("Toolchanger mount of the mounted tool finalizes on main through the real API",
          "[ams][toolchanger][dispatch][threading][1761]") {
    MockPrinter printer;
    LiveToolChanger backend(printer);
    backend.seat_tool(4);
    REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);

    std::vector<std::pair<helix::AmsAction, std::string>> trace;
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == helix::AmsBackend::EVENT_STATE_CHANGED) {
            const char* tag = helix::ui::UpdateQueue::current_callback_tag();
            trace.emplace_back(backend.get_current_action(), tag ? tag : "");
        }
    });

    REQUIRE(backend.change_tool(4).success());
    // The mock already acked SELECT_TOOL; the finalize is still queued.
    CHECK(backend.get_current_action() == helix::AmsAction::SELECTING);

    LiveToolChanger::drain();
    CHECK(backend.get_current_action() == helix::AmsAction::IDLE);
    REQUIRE_FALSE(trace.empty());
    CHECK(trace.front().first == helix::AmsAction::SELECTING);
    CHECK(trace.back().first == helix::AmsAction::IDLE);
    CHECK(trace.back().second == "AmsSubscriptionBackend::execute_gcode_complete");
}
