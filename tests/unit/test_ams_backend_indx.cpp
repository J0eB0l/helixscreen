// SPDX-License-Identifier: GPL-3.0-or-later

// AmsBackendIndx against synthetic status shaped like Bondtech's upstream
// macros (BondtechAB/INDX). Gcode is captured, never sent.

#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_indx.h"
#include "macro_param_cache.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

json indx_objects(int tools = 4) {
    json objects = json::array({"indx", "save_variables", "extruder", "heater_bed", "toolhead",
                                "gcode_move", "configfile", "gcode_macro TOOL_POSITIONS",
                                "gcode_macro _TOOLCHANGE_VARS", "gcode_macro CHANGE_TOOL",
                                "gcode_macro PARK_TOOL", "gcode_macro LOAD_FILAMENT"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
    return objects;
}

json without(const json& objects, const std::string& name) {
    json out = json::array();
    for (const auto& o : objects) {
        if (o.get<std::string>() != name) {
            out.push_back(o);
        }
    }
    return out;
}

PrinterDiscovery discover(const json& objects, int config_tools = 4) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    const json config = {
        {"gcode_macro TOOL_POSITIONS", {{"variable_tool_count", std::to_string(config_tools)}}}};
    hw.parse_config_keys(config);
    MacroParamCache::instance().populate_from_configfile(config, hw.macros());
    return hw;
}

/// MacroParamCache is process-wide: every case here leaves it empty.
struct IndxFixture {
    ~IndxFixture() {
        MacroParamCache::instance().clear();
    }
};

using IndxHelper = test::GcodeCaptureBackend<AmsBackendIndx>;

json active(int tool) {
    return json{{"save_variables", {{"variables", {{"active_tool", tool}}}}}};
}

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX backend: discovery sizes the tools", "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(), 6));
    CHECK(b.get_type() == AmsType::INDX);
    CHECK(b.get_system_info().total_slots == 6);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: active_tool is the mounted tool",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(2));
    CHECK(b.get_current_tool() == 2);
    CHECK(b.get_current_slot() == 2);
    b.feed(json{{"save_variables", {{"variables", {{"toolchange_count", 9}}}}}});
    CHECK(b.get_current_tool() == 2);
    b.feed(active(-1));
    CHECK(b.get_current_tool() == -1);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: a status tool_count re-sizes the slots",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(), 0));
    CHECK(b.get_system_info().total_slots == 0);
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"tool_count", 3}}}});
    CHECK(b.get_system_info().total_slots == 3);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: change sends T<n>, or CHANGE_TOOL without the macro",
                 "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(without(indx_objects(), "gcode_macro T3")));
    b.feed(active(0));
    REQUIRE(b.change_tool(1).success());
    CHECK(b.sent().back() == "T1");

    IndxHelper c;
    c.set_discovery(discover(without(indx_objects(), "gcode_macro T3")));
    c.feed(active(0));
    REQUIRE(c.change_tool(3).success());
    CHECK(c.sent().back() == "CHANGE_TOOL TOOL=3");
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: park sends PARK_TOOL", "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(1));
    REQUIRE(b.park_active_tool().success());
    CHECK(b.sent().back() == "PARK_TOOL");
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: nothing is sent when the macros are not recognized",
                 "[ams][indx][commands][contract]") {
    IndxHelper b;
    b.set_discovery(discover(without(indx_objects(), "gcode_macro CHANGE_TOOL")));
    b.feed(active(0));
    CHECK_FALSE(b.change_tool(0).success());
    CHECK_FALSE(b.park_active_tool().success());
    CHECK(b.sent().empty());
    auto err = b.current_error();
    REQUIRE(err.has_value());
    CHECK(err->detail.find("gcode_macro CHANGE_TOOL") != std::string::npos);
}

TEST_CASE_METHOD(IndxFixture,
                 "INDX backend: a half-seated tool or uncalibrated docks refuse motion",
                 "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", 0}, {"seat_suspect", 2}}}}}});
    CHECK_FALSE(b.change_tool(1).success());
    REQUIRE(b.current_error().has_value());
    CHECK(b.current_error()->recovery_actions.empty());

    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}},
                {"gcode_macro TOOL_POSITIONS", {{"docks_calibrated", false}}}});
    CHECK_FALSE(b.current_error().has_value());
    CHECK_FALSE(b.change_tool(1).success());
    CHECK(b.sent().empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: load names a preset or a temperature",
                 "[ams][indx][commands]") {
    test::RegisteredBackend<IndxHelper> harness;
    IndxHelper& b = *harness;
    b.set_discovery(discover(indx_objects()));
    b.feed(json{
        {"save_variables", {{"variables", {{"active_tool", -1}}}}},
        {"gcode_macro LOAD_FILAMENT",
         {{"filament_presets", {{"PETG", {{"temp", 240}, {"density", 1.27}, {"hc", 1.3}}}}}}}});
    SlotInfo petg = b.get_slot_info(1);
    petg.material = "PETG";
    REQUIRE(test::apply_edit(b, 1, petg).success());
    REQUIRE(b.get_slot_info(1).material == "PETG");
    REQUIRE(b.load_filament(1).success());
    CHECK(b.sent().back() == "LOAD_FILAMENT TOOL=1 TYPE=PETG");
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: unload is manual, a no-heat tool has no filament ops",
                 "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({3})}}}});
    CHECK(b.head_filament_ops(0).load == FilamentOpSupport::Automatic);
    CHECK(b.head_filament_ops(0).unload == FilamentOpSupport::Manual);
    CHECK(b.head_filament_ops(3).load == FilamentOpSupport::Unavailable);
    CHECK_FALSE(b.load_filament(3).success());
    CHECK(b.sent().empty());

    IndxHelper no_load;
    no_load.set_discovery(discover(without(indx_objects(), "gcode_macro LOAD_FILAMENT")));
    CHECK(no_load.head_filament_ops(0).load == FilamentOpSupport::Manual);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: heating needs a mounted tool", "[ams][indx][heat]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    CHECK(b.nozzle_heat_blocked_reason().empty()); // no reading yet
    b.feed(active(-1));
    CHECK_FALSE(b.nozzle_heat_blocked_reason().empty());
    b.feed(active(1));
    CHECK(b.nozzle_heat_blocked_reason().empty());
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({1})}}}});
    CHECK_FALSE(b.nozzle_heat_blocked_reason().empty());
    CHECK(b.heads_share_heater());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: per-extruder usage is never charged to a slot",
                 "[ams][indx][spool]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(2));
    CHECK_FALSE(b.slot_for_extruder(0).has_value());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: own spool macros manage the active spool",
                 "[ams][indx][spool]") {
    auto hw = discover(indx_objects(2), 2);
    MacroParamCache::instance().populate_from_configfile(
        json{{"gcode_macro T0", {{"gcode", "SET_ACTIVE_SPOOL ID=3\nCHANGE_TOOL TOOL=0"}}}},
        hw.macros());
    IndxHelper managed;
    managed.set_discovery(hw);
    CHECK(managed.manages_active_spool());

    MacroParamCache::instance().clear();
    IndxHelper stock;
    stock.set_discovery(hw);
    CHECK_FALSE(stock.manages_active_spool());
}

namespace {

/// An INDX backend whose print is paused, without a printer behind it.
class PausedIndx : public IndxHelper {
  public:
    bool paused = true;

  protected:
    [[nodiscard]] bool print_paused() const override {
        return paused;
    }
};

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX backend: no swap while a print is paused",
                 "[ams][indx][commands][paused]") {
    PausedIndx b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(1));
    const AmsError change = b.change_tool(2);
    CHECK_FALSE(change.success());
    CHECK(change.user_msg.find("different nozzle") != std::string::npos);
    CHECK_FALSE(b.park_active_tool().success());
    CHECK_FALSE(b.load_filament(2).success());
    CHECK(b.sent().empty());
    CHECK_FALSE(b.traits().filament_ops_self_home);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: the mounted tool loads while a print is paused",
                 "[ams][indx][commands][paused]") {
    PausedIndx b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(1));
    REQUIRE(b.load_filament(1).success());
    CHECK(b.sent().back().rfind("LOAD_FILAMENT TOOL=1 ", 0) == 0);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: an active_tool past the tool count is unknown",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(), 4));
    b.feed(active(1));
    REQUIRE(b.get_current_tool() == 1);
    b.feed(active(7));
    CHECK(b.get_current_tool() == 1);
    CHECK(b.nozzle_heat_blocked_reason().empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: a frame that changes nothing is not news",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    int events = 0;
    b.set_event_callback([&events](const std::string&, const std::string&) { ++events; });
    b.feed(active(2));
    const int after_first = events;
    REQUIRE(after_first > 0);
    b.feed(active(2));
    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"toolhead_state", 0}}}});
    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"toolhead_state", 0}}}});
    CHECK(events == after_first + 1);
}
