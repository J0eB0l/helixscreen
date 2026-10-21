// SPDX-License-Identifier: GPL-3.0-or-later

// AmsBackendIndx against synthetic status shaped like Bondtech's upstream
// macros (BondtechAB/INDX). Gcode is captured, never sent.

#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/gcode_recording_api.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_indx.h"
#include "macro_param_cache.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <any>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// The macros phase 3 reads beyond the required contract.
const std::vector<std::string> kPhase3Macros = {
    "MODE_STEALTH",     "MODE_NORMAL",        "MODE_SPORT",       "TC_CYCLE_ALL",
    "MANUAL_TOOL_SEAT", "MANUAL_TOOL_REMOVE", "SET_TOOL_FILAMENT"};

json indx_objects(int tools = 4, const std::vector<std::string>& extra = {}) {
    json objects = json::array({"indx", "save_variables", "extruder", "heater_bed", "toolhead",
                                "gcode_move", "configfile", "gcode_macro TOOL_POSITIONS",
                                "gcode_macro _TOOLCHANGE_VARS", "gcode_macro CHANGE_TOOL",
                                "gcode_macro PARK_TOOL", "gcode_macro LOAD_FILAMENT"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
    for (const auto& macro : extra) {
        objects.push_back("gcode_macro " + macro);
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

// ============================================================================
// Device actions
// ============================================================================

namespace {

/// An INDX backend whose nozzle target the test sets.
class HeatedIndx : public IndxHelper {
  public:
    double target_c = 0.0;

  protected:
    [[nodiscard]] double nozzle_target_c() const override {
        return target_c;
    }
};

const helix::printer::DeviceAction* find_action(const std::vector<helix::printer::DeviceAction>& as,
                                                const std::string& id, int tool_index = -1) {
    for (const auto& a : as) {
        if (a.id == id && (tool_index < 0 || a.tool_index == tool_index)) {
            return &a;
        }
    }
    return nullptr;
}

std::vector<std::string> section_ids(const AmsBackend& b) {
    std::vector<std::string> ids;
    for (const auto& s : b.get_device_sections()) {
        ids.push_back(s.id);
    }
    return ids;
}

std::string info_text(const helix::printer::DeviceAction* a) {
    REQUIRE(a != nullptr);
    const auto* text = std::any_cast<std::string>(&a->current_value);
    REQUIRE(text != nullptr);
    return *text;
}

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX device: sections follow the macros present",
                 "[ams][indx][device]") {
    HeatedIndx all;
    all.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    all.feed(active(1));
    CHECK(section_ids(all) ==
          std::vector<std::string>{"speed", "calibration", "maintenance", "tool_offsets"});

    HeatedIndx some;
    some.set_discovery(
        discover(indx_objects(4, {"MANUAL_TOOL_SEAT", "MANUAL_TOOL_REMOVE", "SET_TOOL_FILAMENT"})));
    some.feed(active(1));
    CHECK(section_ids(some) == std::vector<std::string>{"calibration", "tool_offsets"});

    HeatedIndx unrecognized;
    unrecognized.set_discovery(
        discover(without(indx_objects(4, kPhase3Macros), "gcode_macro CHANGE_TOOL")));
    unrecognized.feed(active(1));
    for (const auto& a : unrecognized.get_device_actions()) {
        INFO(a.id);
        CHECK_FALSE((a.section == "speed" || a.section == "maintenance"));
    }
}

TEST_CASE_METHOD(IndxFixture, "INDX device: speed mode reads _TOOLCHANGE_VARS.mode",
                 "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"mode", "sport"}}}});
    auto actions = b.get_device_actions();
    const auto* speed = find_action(actions, "speed_mode");
    REQUIRE(speed != nullptr);
    CHECK(speed->type == helix::printer::ActionType::DROPDOWN);
    CHECK(speed->options == std::vector<std::string>{"Stealth", "Normal", "Sport"});
    CHECK(std::any_cast<std::string>(speed->current_value) == "Sport");
    CHECK_FALSE(speed->moves_machine);

    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"mode", "ludicrous"}}}});
    actions = b.get_device_actions();
    speed = find_action(actions, "speed_mode");
    REQUIRE(speed != nullptr);
    CHECK_FALSE(speed->current_value.has_value());
    CHECK(speed->enabled);
    CHECK(b.sent().empty());

    REQUIRE(b.execute_device_action("speed_mode", std::string("Stealth")).success());
    CHECK(b.sent() == std::vector<std::string>{"MODE_STEALTH"});
    CHECK_FALSE(b.execute_device_action("speed_mode", std::string("Warp")).success());
    CHECK(b.sent().size() == 1);
}

TEST_CASE_METHOD(IndxFixture,
                 "INDX device: calibrations need a mounted tool with a hotend and a cold target",
                 "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(active(-1));
    auto actions = b.get_device_actions();
    const auto* heater = find_action(actions, "calibrate_heater");
    const auto* fan = find_action(actions, "calibrate_fan");
    REQUIRE(heater != nullptr);
    REQUIRE(fan != nullptr);
    CHECK_FALSE(heater->enabled);
    CHECK_FALSE(fan->enabled);
    CHECK_THAT(heater->disable_reason, Catch::Matchers::ContainsSubstring("No tool"));

    b.feed(active(1));
    b.target_c = 210.0;
    actions = b.get_device_actions();
    heater = find_action(actions, "calibrate_heater");
    fan = find_action(actions, "calibrate_fan");
    CHECK_FALSE(heater->enabled);
    CHECK_THAT(heater->disable_reason, Catch::Matchers::ContainsSubstring("heater off"));
    CHECK(fan->enabled);

    b.target_c = 0.0;
    actions = b.get_device_actions();
    heater = find_action(actions, "calibrate_heater");
    fan = find_action(actions, "calibrate_fan");
    CHECK(heater->enabled);
    CHECK(fan->enabled);
    for (const auto* a : {heater, fan}) {
        CHECK(a->moves_machine);
        CHECK_FALSE(a->allowed_while_paused);
    }
    CHECK(heater->guided_flow == "indx_heater_calibration");
    CHECK(fan->guided_flow == "indx_fan_calibration");
}

TEST_CASE_METHOD(IndxFixture, "INDX device: seat a tool by hand", "[ams][indx][device]") {
    HeatedIndx bare;
    bare.set_discovery(discover(indx_objects()));
    const auto bare_actions = bare.get_device_actions();
    CHECK(find_action(bare_actions, "seat_by_hand") == nullptr);

    HeatedIndx b;
    b.set_discovery(discover(indx_objects(6, kPhase3Macros), 6));
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", -1}, {"seat_suspect", 2}}}}},
                {"gcode_macro TOOL_POSITIONS", {{"docks_calibrated", false}}}});
    const auto actions = b.get_device_actions();
    const auto* seat = find_action(actions, "seat_by_hand");
    REQUIRE(seat != nullptr);
    CHECK(seat->section == "calibration");
    CHECK(seat->enabled);
    CHECK(seat->moves_machine);
    CHECK_FALSE(seat->allowed_while_paused);
    CHECK(seat->guided_flow == indx::kSeatByHandFlowId);

    REQUIRE(b.execute_device_action("seat_by_hand", std::any(3)).success());
    CHECK(b.sent() == std::vector<std::string>{"MANUAL_TOOL_SEAT TOOL=3"});
    CHECK_FALSE(b.execute_device_action("seat_by_hand", std::any(9)).success());
    CHECK_FALSE(b.execute_device_action("seat_by_hand", std::any(std::string("3"))).success());
    CHECK(b.sent().size() == 1);
}

TEST_CASE_METHOD(IndxFixture, "INDX device: cycle every tool", "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", 0}, {"seat_suspect", 2}}}}}});
    auto actions = b.get_device_actions();
    const auto* cycle = find_action(actions, "cycle_all");
    REQUIRE(cycle != nullptr);
    CHECK_FALSE(cycle->enabled);
    CHECK_THAT(cycle->disable_reason, Catch::Matchers::ContainsSubstring("half seated"));
    CHECK(cycle->guided_flow == "indx_cycle_all");

    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}}});
    actions = b.get_device_actions();
    cycle = find_action(actions, "cycle_all");
    REQUIRE(cycle != nullptr);
    CHECK(cycle->enabled);
    REQUIRE(b.execute_device_action("cycle_all").success());
    CHECK(b.sent() == std::vector<std::string>{"TC_CYCLE_ALL"});
}

TEST_CASE_METHOD(IndxFixture, "INDX device: tool offsets are read-only rows",
                 "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(json{{"save_variables",
                 {{"variables",
                   {{"active_tool", 0},
                    {"t1_offset_x", 0.12},
                    {"t1_offset_y", -0.045},
                    {"t1_offset_z", 0.03},
                    {"global_z_offset", -0.05}}}}}});
    auto actions = b.get_device_actions();
    CHECK(info_text(find_action(actions, "tool_offset", 1)) == "X 0.120  Y -0.045  Z 0.030 mm");
    CHECK(info_text(find_action(actions, "tool_offset", 0)) == "-");
    CHECK(info_text(find_action(actions, "global_z_offset")) == "-0.050 mm");
    for (const auto& a : actions) {
        if (a.section == "tool_offsets") {
            CHECK(a.type == helix::printer::ActionType::INFO);
        }
    }

    b.feed(active(2));
    actions = b.get_device_actions();
    CHECK(info_text(find_action(actions, "tool_offset", 1)) == "X 0.120  Y -0.045  Z 0.030 mm");
    CHECK(info_text(find_action(actions, "global_z_offset")) == "-0.050 mm");

    b.feed(json{{"save_variables", {{"variables", {{"t2_offset_z", 0.1}}}}}});
    actions = b.get_device_actions();
    CHECK(info_text(find_action(actions, "tool_offset", 2)) == "X -  Y -  Z 0.100 mm");
}

namespace {

/// An INDX backend sending through a recording API, so the rpc's own answer
/// and timeout are what the test drives.
class RecordingIndx : public AmsBackendIndx {
  public:
    RecordingIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
        : AmsBackendIndx(api, client) {
        running_ = true;
    }
    ~RecordingIndx() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

  protected:
    [[nodiscard]] double nozzle_target_c() const override {
        return 0.0;
    }
};

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX device: a calibration reports when it ends",
                 "[ams][indx][device]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    test::GcodeRecordingApi api{client, state};
    RecordingIndx b{&api, &client};
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(active(1));

    int calls = 0;
    AmsError last;
    auto on_done = [&](const AmsError& r) {
        ++calls;
        last = r;
    };

    SECTION("success arrives with the macro's answer") {
        api.hold_acks = true;
        REQUIRE(b.run_device_action("calibrate_heater", {}, on_done).success());
        REQUIRE(api.sent == std::vector<std::string>{"INDX_CALIBRATE"});
        CHECK(api.timeouts == std::vector<uint32_t>{indx::kCalibrationTimeoutMs});
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 0);
        api.ack_held();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK(last.success());
    }
    SECTION("a timeout is reported as one") {
        api.time_out = {"INDX_CALIBRATE"};
        REQUIRE(b.run_device_action("calibrate_heater", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK(last.result == AmsResult::TIMEOUT);
    }
    SECTION("Klipper's refusal is a failure") {
        api.fail = {"INDX_FAN_CALIBRATE"};
        REQUIRE(b.run_device_action("calibrate_fan", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK_FALSE(last.success());
        CHECK(last.result != AmsResult::TIMEOUT);
    }
    SECTION("the tool cycle is tracked too") {
        REQUIRE(b.run_device_action("cycle_all", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(api.sent == std::vector<std::string>{"TC_CYCLE_ALL"});
        CHECK(calls == 1);
        CHECK(last.success());
    }
}

// ============================================================================
// Seat-fault recovery
// ============================================================================

TEST_CASE_METHOD(IndxFixture,
                 "INDX recovery: a half-seated tool offers seat and remove behind confirms",
                 "[ams][indx][recovery]") {
    const json fault = {{"save_variables", {{"variables", {{"seat_suspect", 2}}}}}};

    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(fault);
    auto err = b.current_error();
    REQUIRE(err.has_value());
    REQUIRE(err->recovery_actions.size() == 2);
    const auto& seat = err->recovery_actions[0];
    const auto& remove = err->recovery_actions[1];
    CHECK(seat.gcode == "MANUAL_TOOL_SEAT TOOL=2");
    CHECK(remove.gcode == "MANUAL_TOOL_REMOVE");
    CHECK(seat.confirm_message == indx::seat_confirm_message(2));
    CHECK(remove.confirm_message == indx::remove_confirm_message(2));
    CHECK_THAT(seat.confirm_message, Catch::Matchers::ContainsSubstring("Tool 3 (T2)"));
    CHECK_THAT(remove.confirm_message, Catch::Matchers::ContainsSubstring("Tool 3 (T2)"));
    CHECK(seat.style == "primary");
    CHECK(remove.style == "danger");

    IndxHelper bare;
    bare.set_discovery(discover(indx_objects()));
    bare.feed(fault);
    REQUIRE(bare.current_error().has_value());
    CHECK(bare.current_error()->recovery_actions.empty());

    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}}});
    CHECK_FALSE(b.current_error().has_value());
}
