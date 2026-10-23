// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Which heater the heater calibration tunes on a printer with several heads
// (prestonbrown/helixscreen#1870).

#include "ui_panel_calibration_pid.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using Access = helix::ui::PIDCalibrationPanelTestAccess;

namespace {

void load_tools(const nlohmann::json& objects) {
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    auto& ts = helix::ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ts.init_tools(hw);
}

struct PidHeadsFixture : LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    PIDCalibrationPanel panel;

    PidHeadsFixture() {
        panel.init_subjects();
        panel.set_api(&api);
    }
    ~PidHeadsFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    }

    void toolchanger(int active_tool) {
        load_tools(nlohmann::json::array(
            {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed"}));
        helix::ToolState::instance().update_from_status(
            {{"toolchanger", {{"tool_number", active_tool}}}});
    }

    void start() {
        client.clear_gcode_script_history();
        Access::start_clicked(panel);
        helix::ui::UpdateQueue::instance().drain();
    }

    bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(PidHeadsFixture, "Heater calibration tunes the active tool's heater",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=extruder1 "));
    CHECK_FALSE(sent("HEATER=extruder "));
}

TEST_CASE_METHOD(PidHeadsFixture, "Heater calibration tunes the head the user picks",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::head_clicked(panel, 0);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=extruder "));
}

TEST_CASE_METHOD(PidHeadsFixture, "A single-extruder printer calibrates its extruder",
                 "[pid][pid_heads]") {
    load_tools(nlohmann::json::array({"extruder", "heater_bed"}));
    Access::activate(panel);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=extruder "));
}

TEST_CASE_METHOD(PidHeadsFixture, "Picking the bed on a tool changer tunes the bed",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);
    Access::bed_clicked(panel);
    start();
    CHECK(sent("PID_CALIBRATE HEATER=heater_bed "));
}

TEST_CASE_METHOD(PidHeadsFixture, "The part fan runs only for the head holding the active tool",
                 "[pid][pid_heads]") {
    toolchanger(1);
    Access::activate(panel);

    SECTION("active head: the fan is set") {
        Access::set_fan_speed(panel, 50);
        start();
        CHECK(sent("M106"));
    }
    SECTION("parked head: M106 would cool the active tool, so none is sent") {
        Access::head_clicked(panel, 0);
        Access::set_fan_speed(panel, 50);
        start();
        CHECK(sent("PID_CALIBRATE HEATER=extruder "));
        CHECK_FALSE(sent("M106"));
    }
}
