// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offset_calibration.cpp
 * @brief Tests for the automatic tool offset calibration module.
 *
 * klipper-toolchanger's CALIBRATE_TOOL_OFFSETS macro does the whole job in one
 * blocking gcode. helix::tool_offset_calibration owns its name and the
 * capability question; the rpc's completion is the run's.
 */

#include "printer_discovery.h"
#include "snapmaker_nozzle_offset.h"
#include "tool_offset_calibration.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace cal = helix::tool_offset_calibration;

namespace {

PrinterDiscovery printer_with(std::initializer_list<const char*> extra) {
    json objects = json::array({"gcode_move", "toolhead", "extruder"});
    for (const char* o : extra) {
        objects.push_back(o);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A klipper-toolchanger with the example config's calibration macro.
PrinterDiscovery calibrating_toolchanger() {
    return printer_with({"toolchanger", "tool T0", "tool T1", "tool T2",
                         "gcode_macro CALIBRATE_TOOL_OFFSETS", "tools_calibrate"});
}

/// A Snapmaker U1, whose firmware ships the per-head offset calibration macros.
PrinterDiscovery snapmaker_u1() {
    json objects = json::array({"gcode_move", "toolhead", "extruder",
                                "gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                                "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"});
    for (int t = 0; t < 4; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            objects.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                              std::to_string(t));
        }
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

} // namespace

// ============================================================================
// Capability
// ============================================================================

TEST_CASE("tool offset calibration: needs a tool changer AND the macro", "[tool_offset_cal]") {
    // A tool changer without the macro has nothing to run; a single-toolhead
    // printer that happens to define the macro has nothing to calibrate.
    CHECK(cal::supported(calibrating_toolchanger()));
    CHECK_FALSE(cal::supported(printer_with({"toolchanger", "tool T0", "tool T1"})));
    CHECK_FALSE(cal::supported(printer_with({"gcode_macro CALIBRATE_TOOL_OFFSETS"})));
    CHECK_FALSE(cal::supported(printer_with({})));
}

TEST_CASE("tool offset calibration: the macro name is case-insensitive on detection",
          "[tool_offset_cal]") {
    // Klipper uppercases the command alias whatever case the section uses.
    PrinterDiscovery hw =
        printer_with({"toolchanger", "tool T0", "tool T1", "gcode_macro Calibrate_Tool_Offsets"});

    CHECK(cal::supported(hw));
    CHECK(cal::calibrate_all_gcode(hw) == "CALIBRATE_TOOL_OFFSETS");
}

TEST_CASE("tool offset calibration: the gcode is the bare macro, or nothing", "[tool_offset_cal]") {
    CHECK(cal::calibrate_all_gcode(calibrating_toolchanger()) == "CALIBRATE_TOOL_OFFSETS");
    CHECK(cal::calibrate_all_gcode(printer_with({"toolchanger", "tool T0"})).empty());
}

TEST_CASE("tool offset calibration: a U1 calibrates through its own guided flow",
          "[tool_offset_cal]") {
    const PrinterDiscovery u1 = snapmaker_u1();
    CHECK(cal::supported(u1));
    CHECK(cal::guided_flow(u1) == helix::snapmaker::nozzle_offset::kGuidedFlowId);
    CHECK(cal::calibrate_all_gcode(u1).empty());

    // A macro-calibrating tool changer names no flow: the macro panel runs it.
    CHECK(cal::guided_flow(calibrating_toolchanger()).empty());
}

// ============================================================================
// INDX: the entry seats a tool by hand
// ============================================================================

namespace {

PrinterDiscovery indx_printer(bool manual_seat) {
    json objects = json::array({"gcode_move", "toolhead", "extruder", "indx", "save_variables",
                                "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
                                "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"});
    if (manual_seat) {
        objects.push_back("gcode_macro MANUAL_TOOL_SEAT");
        objects.push_back("gcode_macro MANUAL_TOOL_REMOVE");
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("tool offset calibration: an INDX seats a tool by hand", "[tool_offset_cal]") {
    const PrinterDiscovery indx = indx_printer(true);
    CHECK(cal::guided_flow(indx) == "indx_seat_by_hand");
    CHECK(cal::supported(indx));
    CHECK(cal::calibrate_all_gcode(indx).empty());

    const PrinterDiscovery bare = indx_printer(false);
    CHECK(cal::guided_flow(bare).empty());
    CHECK_FALSE(cal::supported(bare));
}

TEST_CASE("tool offset calibration: the entry describes what it does", "[tool_offset_cal]") {
    const std::string measure = "Measure every tool's XYZ offset automatically";
    CHECK(cal::entry_description(calibrating_toolchanger()) == measure);
    CHECK(cal::entry_description(snapmaker_u1()) == measure);
    CHECK(std::string(cal::entry_description(indx_printer(true))) ==
          "Seat a tool by hand for the first Z home");
}
