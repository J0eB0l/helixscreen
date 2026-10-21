// SPDX-License-Identifier: GPL-3.0-or-later

// AmsState's shared-heater sync: the heat-blocked subject the nozzle controls
// dim on, and the swap preheat latch a nozzle change clears.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_indx.h"
#include "ams_state.h"
#include "app_globals.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using IndxHelper = helix::test::GcodeCaptureBackend<helix::AmsBackendIndx>;

namespace {

helix::PrinterDiscovery indx_discovery() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"indx", "save_variables", "extruder",
                                  "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
                                  "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"}));
    return hw;
}

json mounted(int tool) {
    return json{{"save_variables", {{"variables", {{"active_tool", tool}}}}},
                {"gcode_macro TOOL_POSITIONS", {{"tool_count", 4}}}};
}

int heat_blocked() {
    return lv_subject_get_int(helix::AmsState::instance().get_nozzle_heat_blocked_subject());
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Shared heater sync: heat_blocked follows the mounted tool",
                 "[ams][indx][heat][subject]") {
    helix::test::RegisteredBackend<IndxHelper> harness;
    harness->set_discovery(indx_discovery());
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);

    harness->feed(mounted(-1));
    ams.sync_from_backend();
    CHECK(heat_blocked() == 1);

    harness->feed(mounted(1));
    ams.sync_from_backend();
    CHECK(heat_blocked() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Shared heater sync: a nozzle change clears the preheat latch",
                 "[ams][indx][heat][subject]") {
    auto& temps = get_printer_state().temperature_state();
    get_printer_state().init_subjects(false);
    temps.init_extruders({"extruder"});
    helix::test::RegisteredBackend<IndxHelper> harness;
    harness->set_discovery(indx_discovery());
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);

    harness->feed(mounted(1));
    ams.sync_from_backend();
    get_printer_state().update_from_status(json{{"extruder", {{"target", 250.0}}}});
    REQUIRE(temps.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    // The same nozzle again keeps it: its melt zone still holds that material.
    ams.sync_from_backend();
    CHECK(temps.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    harness->feed(mounted(2));
    ams.sync_from_backend();
    CHECK(temps.get_active_extruder_last_nonzero_target() == Catch::Approx(0.0));
}
