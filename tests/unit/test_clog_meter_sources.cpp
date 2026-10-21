// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_clog_meter_sources.cpp
 * @brief What feeds the clog meter: clog detectors only. A buffer reading is
 *        drawn by the filament buffer surfaces, never by the clog meter. A
 *        plain Klipper filament_motion_sensor is the last-priority detector
 *        (#1823).
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "clog_meter_geometry.h"
#include "filament_sensor_manager.h"
#include "panel_widget_registry.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterMode;
using helix::ui::ClogMeterStatus;

namespace {

int mode(AmsState& ams) {
    return lv_subject_get_int(ams.get_clog_meter_mode_subject());
}

AmsSystemInfo fps_info(float pressure) {
    AmsSystemInfo info;
    AmsUnit unit;
    unit.slot_count = 4;
    BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = pressure;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    unit.buffer_health = fps;
    info.units.push_back(unit);
    info.sync_feedback_bias = info.pressure_sensor_bias();
    return info;
}

/// The meter subjects and the override are the singleton's, read by later tests.
struct ResetMeter {
    AmsState& ams;
    ~ResetMeter() {
        ams.set_source_override(0);
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "clog meter: a buffer reading alone is not clog detection",
                 "[ams][clog][sources]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetMeter reset{ams};

    SECTION("Happy Hare sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == 0);
    }

    SECTION("a filament pressure sensor") {
        AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));
        CHECK(mode(ams) == 0);
    }

    SECTION("beside a detector, the meter shows the detector") {
        AmsSystemInfo info = fps_info(0.9f);
        info.encoder_info.enabled = true;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Encoder));
        CHECK(std::string(lv_subject_get_string(ams.get_clog_meter_mode_text_subject())) != "FPS");
    }

    SECTION("a forced detector this printer lacks leaves the meter empty") {
        ams.set_source_override(2); // Flowguard
        AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));
        CHECK(mode(ams) == 0);
    }
}

// ===========================================================================
// A Klipper filament_motion_sensor as the last-priority source (#1823)
// ===========================================================================

namespace {

constexpr const char* kEncoder = "filament_motion_sensor encoder";
constexpr const char* kSecond = "filament_motion_sensor jam_b";
constexpr const char* kSwitch = "filament_switch_sensor runout";

nlohmann::json motion_frame(const char* klipper, bool detected, bool enabled) {
    return nlohmann::json{{klipper, {{"filament_detected", detected}, {"enabled", enabled}}}};
}

std::string text(lv_subject_t* subject) {
    return lv_subject_get_string(subject);
}

/// A discovery from printer.objects.list names.
helix::PrinterDiscovery discovery_of(std::initializer_list<const char*> objects) {
    helix::PrinterDiscovery hw;
    nlohmann::json list = nlohmann::json::array();
    for (const char* o : objects) {
        list.push_back(o);
    }
    hw.parse_objects(list);
    return hw;
}

/// No AMS backend on a printer with no MMU, the meter observing the sensor
/// manager the way SubjectInitializer wires it, and every singleton put back
/// afterwards.
struct MotionSensorFixture : LVGLTestFixture {
    AmsState& ams = AmsState::instance();
    FilamentSensorManager& fsm = FilamentSensorManager::instance();
    helix::PrinterDiscovery saved_discovery;

    MotionSensorFixture() {
        get_printer_state().init_subjects(false);
        saved_discovery = get_printer_state().get_discovery();
        get_printer_state().set_hardware(discovery_of({"extruder", "heater_bed", "gcode_move"}));
        ams.init_subjects(false);
        ams.clear_backends();
        fsm.init_subjects();
        FilamentSensorManagerTestAccess::reset(fsm);
        ams.install_motion_sensor_observer();
    }

    ~MotionSensorFixture() override {
        AmsStateTestAccess::remove_motion_sensor_observer(ams);
        FilamentSensorManagerTestAccess::reset(fsm);
        ams.set_source_override(0);
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
        get_printer_state().set_hardware(saved_discovery);
        helix::ui::UpdateQueue::instance().drain();
    }

    void discover(std::vector<std::string> names) {
        fsm.discover_sensors(names);
    }

    /// The meter's observer is deferred through the UpdateQueue.
    static void settle() {
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed(const nlohmann::json& frame) {
        fsm.update_from_status(frame);
        settle();
    }

    int status() {
        return lv_subject_get_int(ams.get_clog_meter_status_subject());
    }
};

} // namespace

TEST_CASE_METHOD(MotionSensorFixture, "clog meter: a motion sensor is a state, not a gauge",
                 "[ams][clog][sources][motion][1823]") {
    discover({kEncoder});

    SECTION("armed and moving reads OK with an empty fill") {
        feed(motion_frame(kEncoder, true, true));
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Motion));
        CHECK(lv_subject_get_int(ams.get_clog_meter_value_subject()) ==
              helix::ui::kMotionMovingValue);
        CHECK(lv_subject_get_int(ams.get_clog_meter_warning_subject()) == 0);
        CHECK(status() == static_cast<int>(ClogMeterStatus::Ok));
        CHECK(text(ams.get_clog_meter_mode_text_subject()) == "encoder");
        CHECK(text(ams.get_clog_meter_center_text_subject()).empty());
    }

    SECTION("armed and not moving is a jam: warning, full fill") {
        feed(motion_frame(kEncoder, false, true));
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Motion));
        CHECK(lv_subject_get_int(ams.get_clog_meter_value_subject()) ==
              helix::ui::kMotionJammedValue);
        CHECK(lv_subject_get_int(ams.get_clog_meter_warning_subject()) == 1);
        CHECK(status() == static_cast<int>(ClogMeterStatus::Fault));
        CHECK(text(ams.get_clog_meter_center_text_subject()) == "Jammed");
    }

    SECTION("disarmed in Klipper reads Off, whatever the encoder says") {
        feed(motion_frame(kEncoder, false, false));
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Motion));
        CHECK(lv_subject_get_int(ams.get_clog_meter_value_subject()) ==
              helix::ui::kMotionDisarmedValue);
        CHECK(lv_subject_get_int(ams.get_clog_meter_warning_subject()) == 0);
        CHECK(status() == static_cast<int>(ClogMeterStatus::Off));
        CHECK(text(ams.get_clog_meter_center_text_subject()) == "Disarmed");
    }

    SECTION("a later frame moves the meter with no backend sync") {
        feed(motion_frame(kEncoder, true, true));
        REQUIRE(status() == static_cast<int>(ClogMeterStatus::Ok));
        feed(motion_frame(kEncoder, false, true));
        CHECK(status() == static_cast<int>(ClogMeterStatus::Fault));
    }

    SECTION("the danger threshold override does not draw a band on a state") {
        ams.set_danger_threshold_override(60);
        feed(motion_frame(kEncoder, true, true));
        CHECK(lv_subject_get_int(ams.get_clog_meter_danger_pct_subject()) == 0);
        ams.set_danger_threshold_override(0);
    }
}

TEST_CASE_METHOD(MotionSensorFixture, "clog meter: the motion sensor only stands in last",
                 "[ams][clog][sources][motion][1823]") {
    discover({kEncoder});
    feed(motion_frame(kEncoder, false, true));
    REQUIRE(mode(ams) == static_cast<int>(ClogMeterMode::Motion));

    SECTION("a Happy Hare encoder wins") {
        AmsSystemInfo info;
        info.encoder_info.enabled = true;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Encoder));
    }

    SECTION("FlowGuard wins") {
        AmsSystemInfo info;
        info.flowguard_info.enabled = true;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Flowguard));
    }

    SECTION("an AFC buffer with fault detection wins") {
        AmsSystemInfo info;
        AmsUnit unit;
        BufferHealth health;
        health.fault_detection_enabled = true;
        unit.buffer_health = health;
        info.units.push_back(unit);
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Buffer));
    }

    SECTION("legacy clog detection wins") {
        AmsSystemInfo info;
        info.clog_detection = 1;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Encoder));
    }

    SECTION("a forced source this printer lacks still falls back to the sensor") {
        ams.set_source_override(2); // Flowguard
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Motion));
    }

    SECTION("a forced source beside a backend detector does not fall back") {
        ams.set_source_override(2); // Flowguard, absent
        AmsSystemInfo info;
        info.encoder_info.enabled = true;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == 0);
    }
}

TEST_CASE_METHOD(MotionSensorFixture, "clog meter: which motion sensors make the tile available",
                 "[ams][clog][sources][motion][1823]") {
    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->hardware_gate_subject) == "clog_meter_mode");

    SECTION("a switch sensor is not a jam detector") {
        discover({kSwitch});
        feed(nlohmann::json{{kSwitch, {{"filament_detected", true}, {"enabled", true}}}});
        CHECK(mode(ams) == 0);
    }

    SECTION("a motion sensor disabled in Settings does not count") {
        discover({kEncoder});
        feed(motion_frame(kEncoder, true, true));
        REQUIRE(mode(ams) != 0);
        fsm.set_sensor_enabled(kEncoder, false);
        settle();
        CHECK(mode(ams) == 0);
        fsm.set_sensor_enabled(kEncoder, true);
        settle();
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Motion));
    }

    SECTION("the master switch off disables every sensor") {
        discover({kEncoder});
        feed(motion_frame(kEncoder, true, true));
        REQUIRE(mode(ams) != 0);
        fsm.set_master_enabled(false);
        settle();
        CHECK(mode(ams) == 0);
        fsm.set_master_enabled(true);
        settle();
    }

    SECTION("discovery alone makes it available, reading OK until a frame says otherwise") {
        discover({kEncoder});
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Motion));
        CHECK(status() == static_cast<int>(ClogMeterStatus::Ok));
    }

    SECTION("an AMS-named motion sensor is not one Settings lists") {
        constexpr const char* kLane = "filament_motion_sensor lane1";
        discover({kLane});
        feed(motion_frame(kLane, true, true));
        CHECK(mode(ams) == 0);
    }

    SECTION("a sensor an AMS backend owns is not one Settings lists") {
        constexpr const char* kToolhead = "filament_motion_sensor toolhead";
        get_printer_state().set_hardware(discovery_of(
            {"mmu", "mmu_encoder mmu_encoder", "extruder", "heater_bed", "gcode_move"}));
        discover({kToolhead});
        feed(motion_frame(kToolhead, false, true));
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
        CHECK(mode(ams) == 0); // clog_meter_mode is the tile's gate: not placeable
    }

    SECTION("nothing discovered, nothing to show") {
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
        CHECK(mode(ams) == 0);
    }
}

TEST_CASE_METHOD(MotionSensorFixture, "clog meter: which of several motion sensors it reads",
                 "[ams][clog][sources][motion][1823]") {
    SECTION("the first one Klipper has armed") {
        discover({kEncoder, kSecond});
        feed(motion_frame(kEncoder, true, false));
        feed(motion_frame(kSecond, true, true));
        CHECK(text(ams.get_clog_meter_mode_text_subject()) == "jam_b");
        CHECK(status() == static_cast<int>(ClogMeterStatus::Ok));
    }

    SECTION("the first one when none is armed") {
        discover({kEncoder, kSecond});
        feed(motion_frame(kEncoder, true, false));
        feed(motion_frame(kSecond, true, false));
        CHECK(text(ams.get_clog_meter_mode_text_subject()) == "encoder");
        CHECK(status() == static_cast<int>(ClogMeterStatus::Off));
    }

    SECTION("the one on the active lane, armed or not") {
        constexpr const char* kHead0 = "filament_motion_sensor e0_filament";
        constexpr const char* kHead1 = "filament_motion_sensor e1_filament";
        discover({kHead0, kHead1});
        feed(motion_frame(kHead0, true, true));
        feed(motion_frame(kHead1, false, false));
        AmsSystemInfo info;
        info.current_slot = 1;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(text(ams.get_clog_meter_mode_text_subject()) == "e1_filament");
        CHECK(status() == static_cast<int>(ClogMeterStatus::Off));

        info.current_slot = -1;
        info.current_tool = 0;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(text(ams.get_clog_meter_mode_text_subject()) == "e0_filament");
    }
}
