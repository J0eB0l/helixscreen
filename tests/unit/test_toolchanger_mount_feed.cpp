// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_mount_feed.cpp
 * @brief Loading and unloading through the mounted tool (MedusaHC): the stage
 *        plan, the gcode each stage sends, the operator prompt, the carriage
 *        restore, the printer's own macros, and the print gate.
 */

#include "ui_update_queue.h"

#include "../busy_guard_fixture.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_toolchanger.h"
#include "filament_op_slot_resolver.h"
#include "macro_param_cache.h"
#include "mount_feed_sequence.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace mf = helix::mount_feed;
using helix::test::ToolChangerHelper;
using json = nlohmann::json;

namespace {

mf::Request load_request() {
    mf::Request r;
    r.load = true;
    r.heads = {{/*head=*/2, /*temp_c=*/215, /*mount_gcode=*/"SELECT_TOOL T=2"}};
    r.restore_gcode = "SELECT_TOOL T=0";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tool 3, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

/// A Load of tools 1 and 3 (0-based) with tool 0 on the carriage at the start.
mf::Request multi_load_request() {
    mf::Request r;
    r.load = true;
    r.heads = {{1, 210, "SELECT_TOOL T=1"}, {3, 220, "SELECT_TOOL T=3"}};
    r.restore_gcode = "SELECT_TOOL T=0";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tools 2, 4 as far as it goes, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

std::vector<std::string> gcodes(const std::vector<mf::Stage>& stages) {
    std::vector<std::string> out;
    for (const auto& s : stages) {
        out.push_back(s.gcode);
    }
    return out;
}

/// A MedusaHC-shaped changer whose acks the test hands out one at a time.
class AckingChanger : public ToolChangerHelper {
  public:
    explicit AckingChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete,
                           std::function<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        sent_.push_back(gcode);
        acks_.push_back(std::move(on_complete));
        errors_.push_back(std::move(on_error));
        return AmsErrorHelper::success();
    }

    /// Ack the newest stage and let the deferred handlers run.
    void ack() {
        REQUIRE_FALSE(acks_.empty());
        auto cb = acks_.back();
        cb();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail() {
        REQUIRE_FALSE(errors_.empty());
        MoonrakerError err;
        err.message = "simulated";
        errors_.back()(err);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

  private:
    std::vector<std::function<void()>> acks_;
    std::vector<std::function<void(const MoonrakerError&)>> errors_;
};

} // namespace

// ============================================================================
// The plan
// ============================================================================

TEST_CASE("mount-to-feed: a load mounts, heats, prompts, purges and restores",
          "[toolchanger][mount_feed]") {
    const auto stages = mf::plan(load_request());
    REQUIRE(stages.size() == 5);
    CHECK(stages[0].step == mf::Step::Mount);
    CHECK(stages[0].gcode == "SELECT_TOOL T=2");
    CHECK(stages[1].gcode == "M109 T2 S215");
    CHECK(stages[2].step == mf::Step::Insert);
    CHECK(stages[2].awaits_operator);
    CHECK(stages[2].gcode.find("action:prompt_begin Load filament") != std::string::npos);
    CHECK(stages[2].gcode.find("MSG=HELIX_MOUNT_FEED_CONTINUE|primary") != std::string::npos);
    CHECK(stages[2].gcode.find("MSG=HELIX_MOUNT_FEED_CANCEL|error") != std::string::npos);
    CHECK(stages[3].step == mf::Step::Purge);
    CHECK(stages[3].gcode ==
          mf::prompt_end_gcode() + "\nM83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                                   "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");
    CHECK(stages[4].gcode == "SELECT_TOOL T=0");
}

TEST_CASE("mount-to-feed: the tool already on the carriage is neither mounted nor restored",
          "[toolchanger][mount_feed]") {
    mf::Request r = load_request();
    r.heads[0].mount_gcode.clear();
    r.restore_gcode.clear();
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 3);
    CHECK(stages.front().step == mf::Step::Heat);
    CHECK(stages.back().step == mf::Step::Purge);
}

TEST_CASE("mount-to-feed: the printer's own macro replaces the built-in moves",
          "[toolchanger][mount_feed][macro]") {
    mf::Request load = load_request();
    load.macro = "LOAD_FILAMENT";
    CHECK(mf::plan(load)[3].gcode ==
          mf::prompt_end_gcode() +
              "\nLOAD_FILAMENT T=2 TEMP=215\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");

    mf::Request unload = load_request();
    unload.load = false;
    // No feeder release: the retract alone takes the strand fully out.
    CHECK(
        gcodes(mf::plan(unload)) ==
        std::vector<std::string>{
            "SELECT_TOOL T=2", "M109 T2 S215",
            "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0",
            "SELECT_TOOL T=0"});
    unload.macro = "UNLOAD_FILAMENT";
    CHECK(mf::plan(unload)[2].gcode ==
          "UNLOAD_FILAMENT T=2\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");
}

TEST_CASE("mount-to-feed: a macro stands in only when it takes the tool as T=",
          "[toolchanger][mount_feed][macro]") {
    CachedMacroInfo with_t;
    with_t.knowledge = MacroParamKnowledge::KNOWN_PARAMS;
    with_t.params = {MacroParam{"T"}, MacroParam{"TEMP"}};
    CHECK(mf::macro_takes_tool(with_t));

    CachedMacroInfo without_t = with_t;
    without_t.params = {MacroParam{"TEMP"}};
    CHECK_FALSE(mf::macro_takes_tool(without_t));

    CachedMacroInfo unknown;
    CHECK_FALSE(mf::macro_takes_tool(unknown));
}

TEST_CASE("mount-to-feed: prompt text cannot break out of its RESPOND line",
          "[toolchanger][mount_feed]") {
    mf::Request r = load_request();
    r.prompt_text = "a \"quoted\" ; comment | pipe";
    const std::string g = mf::prompt_gcode(r);
    CHECK(g.find("a 'quoted' , comment / pipe") != std::string::npos);
}

TEST_CASE("mount-to-feed: only the button's own echo is a reply", "[toolchanger][mount_feed]") {
    CHECK(mf::parse_reply("// HELIX_MOUNT_FEED_CONTINUE") == mf::Reply::Continue);
    CHECK(mf::parse_reply("// HELIX_MOUNT_FEED_CANCEL ") == mf::Reply::Cancel);
    // The prompt's own button lines carry the marker text too.
    CHECK(mf::parse_reply("// action:prompt_footer_button Continue|RESPOND TYPE=command "
                          "MSG=HELIX_MOUNT_FEED_CONTINUE|primary") == mf::Reply::None);
    CHECK(mf::parse_reply("echo: something") == mf::Reply::None);
}

// ============================================================================
// The run
// ============================================================================

TEST_CASE("mount-to-feed: the run waits on the operator and cleans up on Cancel",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    CHECK(run.current_gcode() == std::optional<std::string>("SELECT_TOOL T=2"));
    run.stage_done();
    CHECK(run.step_index() == 1);
    run.stage_done();
    REQUIRE(run.current_step() == mf::Step::Insert);
    run.stage_done(); // the prompt is up
    CHECK(run.awaiting_operator());
    CHECK_FALSE(run.current_gcode().has_value());
    run.stage_done(); // a stray ack moves nothing
    CHECK(run.awaiting_operator());

    run.operator_reply(mf::Reply::Cancel);
    CHECK(run.aborted());
    CHECK(run.current_gcode() ==
          std::optional<std::string>(mf::prompt_end_gcode() +
                                     "\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0\n"
                                     "SELECT_TOOL T=0"));
    run.stage_done();
    CHECK(run.finished());
}

TEST_CASE("mount-to-feed: a failure turns the rest into heater off and restore",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done(); // mounted
    run.abort();      // the heat failed
    CHECK(run.current_gcode() ==
          std::optional<std::string>("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0\n"
                                     "SELECT_TOOL T=0"));
    run.abort(); // a failure inside the cleanup ends the run
    CHECK(run.finished());
}

// ============================================================================
// The backend
// ============================================================================

TEST_CASE("MedusaHC loads through the carriage and puts the old tool back",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.get_current_slot() == 0);
    CHECK(tc.head_filament_ops(2).load == FilamentOpSupport::Automatic);
    CHECK(tc.head_filament_ops(2).unload == FilamentOpSupport::Automatic);

    REQUIRE(tc.load_filament(2).success());
    REQUIRE(tc.sent_.size() == 1);
    CHECK(tc.sent_.back() == "SELECT_TOOL T=2");
    CHECK(tc.get_current_action() == AmsAction::SELECTING); // the Mount step
    CHECK(tc.get_system_info().operation_phase == 0);

    // A swap's own frames must not end the load or move its step.
    tc.feed_status("ready", 2);
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.get_system_info().operation_phase == 0);

    tc.ack();
    CHECK(tc.sent_.back() == "M109 T2 S220"); // no material named: the default
    CHECK(tc.get_current_action() == AmsAction::HEATING);
    tc.ack();
    CHECK(tc.sent_.back().find("action:prompt_show") != std::string::npos);
    tc.ack();
    const size_t before_reply = tc.sent_.size();
    CHECK(tc.get_system_info().is_busy()); // waiting on the operator
    CHECK(tc.can_cancel_operation());

    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.sent_.size() == before_reply + 1);
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    CHECK(tc.get_current_action() == AmsAction::PURGING);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
}

TEST_CASE("MedusaHC unloads by retracting fully, sends no OPEN, and parks when nothing was "
          "mounted",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_feeder(toolchanger_addon::Feeder{true, "MedusaHC", "OPEN", "CLOSE"});
    tc.feed_status("ready", -1);

    REQUIRE(tc.unload_filament(1).success());
    CHECK(tc.sent_.back() == "SELECT_TOOL T=1");
    tc.ack();
    tc.ack();
    CHECK(tc.sent_.back() ==
          "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    tc.ack();
    CHECK(tc.sent_.back() == "UNSELECT_TOOL");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "OPEN") == tc.sent_.end());
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC runs the printer's LOAD_FILAMENT when it takes T=",
          "[toolchanger][mount_feed][medusahc][macro]") {
    auto& cache = MacroParamCache::instance();
    cache.populate_from_configfile(
        json{{"gcode_macro load_filament",
              {{"gcode", "{% set t = params.T|int %}{% set temp = params.TEMP|int %}M109"}}}},
        std::unordered_set<std::string>{"LOAD_FILAMENT"});
    struct ClearCache {
        ~ClearCache() {
            MacroParamCache::instance().clear();
        }
    } clear_cache;

    AckingChanger tc(4);
    tc.feed_status("ready", 2);
    REQUIRE(tc.load_filament(2).success());
    CHECK(tc.sent_.front() == "M109 T2 S220"); // already mounted: no mount stage
    tc.ack();
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    CHECK(tc.sent_.back().find("LOAD_FILAMENT T=2 TEMP=220") != std::string::npos);
    CHECK(tc.sent_.back().find("G1 E50") == std::string::npos);
}

TEST_CASE("MedusaHC cancels a load waiting on the operator",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    tc.ack();
    tc.ack(); // the prompt is up
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                 "\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                 "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC refuses a second op while one runs, and cleans up a failed stage",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(1).success());
    CHECK_FALSE(tc.load_filament(2).success());
    CHECK_FALSE(tc.unload_filament(1).success());

    tc.ack();  // mounted
    tc.fail(); // the heat failed
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SELECT_TOOL T=0");
    // One tool: the failure toast is about the load, not a batch.
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Load stopped");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC filament ops are refused while a print is running or paused",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    // The backend's print gate and every surface's button gating read this:
    // a paused print is no time to mount another tool.
    REQUIRE(tc.filament_ops_self_home());
    CHECK(helix::print_blocks_filament_op(PrintState::Printing, tc.filament_ops_self_home()));
    CHECK(helix::print_blocks_filament_op(PrintState::Paused, tc.filament_ops_self_home()));
    CHECK_FALSE(helix::print_blocks_filament_op(PrintState::Idle, tc.filament_ops_self_home()));
    // The backend heats the tool it mounts: the UI must not heat the active one.
    CHECK(tc.supports_auto_heat_on_load());
    CHECK(tc.supports_auto_heat_on_unload());
}

TEST_CASE("MedusaHC shows the mount-to-feed steps for a load and an unload",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    const auto load = tc.get_operation_step_model(StepOperationType::LOAD_FRESH);
    REQUIRE(load.steps.size() == 5);
    CHECK(load.steps[2].label == "Insert filament");
    const auto unload = tc.get_operation_step_model(StepOperationType::UNLOAD);
    REQUIRE(unload.steps.size() == 4);
    CHECK(unload.steps[2].label == "Retract filament");
    CHECK(unload.steps[3].label == "Restore tool");
}

// ============================================================================
// The operator wait
// ============================================================================

TEST_CASE("mount-to-feed: the operator wait times out into the cleanup",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done();
    run.stage_done();
    run.stage_done(); // the prompt is up
    REQUIRE(run.awaiting_operator());

    CHECK(run.advance_wait(1000) == mf::Run::WaitResult::Waiting);
    CHECK(run.advance_wait(1000) == mf::Run::WaitResult::Waiting);
    CHECK(run.awaiting_operator());
    // Three minutes is the ceiling; the wait never outlives it.
    CHECK(run.advance_wait(mf::kPromptTimeoutMs) == mf::Run::WaitResult::Expired);
    CHECK(run.aborted());
    CHECK(run.current_gcode() ==
          std::optional<std::string>(mf::prompt_end_gcode() +
                                     "\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0\n"
                                     "SELECT_TOOL T=0"));
    // A wait that is not running is not expired either.
    CHECK(run.advance_wait(1000) == mf::Run::WaitResult::NotWaiting);
}

TEST_CASE("mount-to-feed: a prompt dismissed without an answer reads as a dismissal",
          "[toolchanger][mount_feed]") {
    // The screen (or another client) closed the prompt: the echo of the
    // prompt_end RESPOND is the only trace, and the operator can no longer
    // answer what is gone.
    CHECK(mf::parse_console("// action:prompt_end") == mf::ConsoleSignal::Dismissed);
    CHECK(mf::parse_console("action:prompt_end") == mf::ConsoleSignal::Dismissed);
    CHECK(mf::may_be_prompt_signal("// action:prompt_end"));
    // The run's own stages send prompt_end too, but by then it is not waiting.
    CHECK(mf::parse_console("// HELIX_MOUNT_FEED_CONTINUE") == mf::ConsoleSignal::Continue);
    CHECK(mf::parse_console("// HELIX_MOUNT_FEED_CANCEL") == mf::ConsoleSignal::Cancel);
    CHECK(mf::parse_console("echo: unrelated") == mf::ConsoleSignal::None);
    CHECK_FALSE(mf::may_be_prompt_signal("echo: unrelated"));
}

// ============================================================================
// Several tools in one run
// ============================================================================

TEST_CASE("mount-to-feed: a load of several tools asks once, then works tool by tool",
          "[toolchanger][mount_feed][batch]") {
    const auto stages = mf::plan(multi_load_request());
    REQUIRE(stages.size() == 8);
    // One prompt up front, naming every tool.
    CHECK(stages[0].step == mf::Step::Insert);
    CHECK(stages[0].awaits_operator);
    CHECK(stages[0].gcode.find("Insert filament into Tools 2, 4 as far as it goes") !=
          std::string::npos);
    // The stage after the reply closes the prompt, mounts the first tool, and
    // warms the second partway while the first works.
    CHECK(stages[1].step == mf::Step::Mount);
    CHECK(stages[1].gcode == mf::prompt_end_gcode() +
                                 "\nSELECT_TOOL T=1\n"
                                 "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=143");
    CHECK(stages[2].gcode == "M109 T1 S210");
    CHECK(stages[3].gcode == "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                             "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    // The last tool mounts with nothing left to preheat, and the carriage
    // goes back to the tool the run found on it.
    CHECK(stages[4].gcode == "SELECT_TOOL T=3");
    CHECK(stages[5].gcode == "M109 T3 S220");
    CHECK(stages[6].gcode == "M83\nG1 E5 F60\nG1 E50 F300\nG1 E50 F300\nG1 E-10 "
                             "F1800\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(stages[7].gcode == "SELECT_TOOL T=0");
}

TEST_CASE("mount-to-feed: an unload of several tools preheats the next and restores at the end",
          "[toolchanger][mount_feed][batch]") {
    mf::Request r = multi_load_request();
    r.load = false;
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 7);
    // No prompt in the unload direction.
    CHECK(stages[0].step == mf::Step::Mount);
    CHECK(stages[0].gcode == "SELECT_TOOL T=1\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=143");
    CHECK(stages[1].gcode == "M109 T1 S210");
    CHECK(stages[2].gcode ==
          "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(stages[3].gcode == "SELECT_TOOL T=3");
    CHECK(stages[4].gcode == "M109 T3 S220");
    CHECK(stages[5].gcode ==
          "M83\nG1 E-25 F1800\nG1 E-150 F1200\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(stages[6].gcode == "SELECT_TOOL T=0");
}

TEST_CASE("mount-to-feed: a multi-tool run's step index walks every tool's stride",
          "[toolchanger][mount_feed][batch]") {
    mf::Run run(multi_load_request());
    REQUIRE(run.phase_index() == 2); // the prompt reads as tool 1's insert
    run.stage_done();
    REQUIRE(run.awaiting_operator());
    run.operator_reply(mf::Reply::Continue);
    CHECK(run.phase_index() == 0); // tool 1: mount
    run.stage_done();
    CHECK(run.phase_index() == 1); // tool 1: heat
    run.stage_done();
    CHECK(run.phase_index() == 3); // tool 1: purge
    run.stage_done();
    CHECK(run.phase_index() == 5); // tool 2: mount
    run.stage_done();
    CHECK(run.completed_heads() == std::vector<int>{1}); // tool 1's work is done
    run.stage_done();
    run.stage_done();
    CHECK(run.phase_index() == 9); // restore belongs to the last tool
    // The restore in flight counts the tool it is putting back as complete:
    // its filament work is done.
    CHECK(run.completed_heads() == std::vector<int>({1, 3}));
    run.stage_done();
    CHECK(run.finished());
}

TEST_CASE("mount-to-feed: a multi-tool stop turns every heater in the plan off",
          "[toolchanger][mount_feed][batch]") {
    mf::Run run(multi_load_request());
    run.stage_done();
    run.operator_reply(mf::Reply::Continue);
    run.stage_done();
    run.stage_done();
    run.stage_done(); // tool 1 purged
    CHECK(run.completed_heads() == std::vector<int>{1});
    run.abort(); // tool 2's heat failed
    CHECK(run.aborted());
    CHECK(run.current_gcode() ==
          std::optional<std::string>("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                                     "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                     "SELECT_TOOL T=0"));
}

// ============================================================================
// Load all / Unload all on the backend
// ============================================================================

TEST_CASE("MedusaHC loads several tools in one sequenced run, preheating the next",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    CHECK(tc.max_heads_per_filament_op() == 4);

    REQUIRE(tc.load_filament_batch({1, 3}).success());
    REQUIRE(tc.sent_.size() == 1);
    // One prompt naming both tools, before anything mounts.
    CHECK(tc.sent_.back().find("Insert filament into Tools 2, 4 as far as it goes") !=
          std::string::npos);
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    // The prompt closes on the first mount, which also preheats tool 3 (0-based
    // head 3, 220C default material -> 143C).
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                 "\nSELECT_TOOL T=1\n"
                                 "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=143");
    tc.ack();
    CHECK(tc.sent_.back() == "M109 T1 S220");
    tc.ack();
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL T=3");
    tc.ack();
    CHECK(tc.sent_.back() == "M109 T3 S220");
    tc.ack();
    CHECK(tc.sent_.back().find("G1 E50 F300") != std::string::npos);
    tc.ack();
    CHECK(tc.sent_.back() == "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC times a multi-tool load out and stops with every heater off",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    tc.ack(); // the prompt is up
    REQUIRE(tc.get_system_info().is_busy());

    ToolChangerTestAccess::feed_wait_elapsed(tc, mf::kPromptTimeoutMs);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                 "\nSET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                                 "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                 "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC cancels a multi-tool load mid-sequence and unwinds every heater",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted tool 1 (with tool 3 preheating)
    tc.ack(); // heated
    tc.ack(); // purged
    REQUIRE(tc.sent_.back() == "SELECT_TOOL T=3");
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                             "SELECT_TOOL T=0");
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Load all stopped Completed: Tool 2");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC counts a dismissed prompt as a Cancel", "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    tc.ack();
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_console_signal(tc, mf::ConsoleSignal::Dismissed);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                 "\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                 "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC times a single-tool load out when nobody inserts",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    tc.ack();
    tc.ack(); // the prompt is up
    // A wait inside the ceiling keeps waiting.
    ToolChangerTestAccess::feed_wait_elapsed(tc, mf::kPromptTimeoutMs - 1000);
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.sent_.size() == 3);
    ToolChangerTestAccess::feed_wait_elapsed(tc, 1000);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                 "\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                 "SELECT_TOOL T=0");
    tc.ack();
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC sends the printer's LOAD_ALL_FILAMENT for a full selection",
          "[toolchanger][mount_feed][medusahc][batch][macro]") {
    auto& cache = MacroParamCache::instance();
    cache.populate_from_configfile(
        json{{"gcode_macro load_all_filament", {{"gcode", "{% if params.TEMP %}M109{% endif %}"}}},
             {"gcode_macro unload_all_filament", {{"gcode", "M104 S0"}}}},
        std::unordered_set<std::string>{"LOAD_ALL_FILAMENT", "UNLOAD_ALL_FILAMENT"});
    struct ClearCache {
        ~ClearCache() {
            MacroParamCache::instance().clear();
        }
    } clear_cache;

    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    // Every tool selected: the printer's own macro runs the whole batch.
    REQUIRE(tc.load_filament_batch({0, 1, 2, 3}).success());
    REQUIRE(tc.sent_.size() == 1);
    CHECK(tc.sent_.back() == "LOAD_ALL_FILAMENT TEMP=220");
    CHECK(tc.get_current_action() == AmsAction::LOADING);

    // A partial selection sequences tool by tool, macro or no macro. A fresh
    // changer: this fixture drops the two-arg ack, so the optimistic action
    // above never resolves and would refuse a second op.
    AckingChanger partial(4);
    partial.feed_status("ready", 0);
    REQUIRE(partial.load_filament_batch({1, 2}).success());
    CHECK(partial.sent_.back().find("action:prompt_begin") != std::string::npos);
    CHECK(partial.sent_.back().find("Tools 2, 3") != std::string::npos);
}

TEST_CASE("MedusaHC sends the printer's UNLOAD_ALL_FILAMENT for a full selection",
          "[toolchanger][mount_feed][medusahc][batch][macro]") {
    auto& cache = MacroParamCache::instance();
    cache.populate_from_configfile(
        json{{"gcode_macro unload_all_filament", {{"gcode", "M104 S0"}}}},
        std::unordered_set<std::string>{"UNLOAD_ALL_FILAMENT"});
    struct ClearCache {
        ~ClearCache() {
            MacroParamCache::instance().clear();
        }
    } clear_cache;

    AckingChanger tc(4);
    tc.feed_status("ready", -1);
    REQUIRE(tc.unload_filament_batch({0, 1, 2, 3}).success());
    REQUIRE(tc.sent_.size() == 1);
    // No TEMP: this macro takes none, and none is invented for it.
    CHECK(tc.sent_.back() == "UNLOAD_ALL_FILAMENT");
}

TEST_CASE("MedusaHC's step bar tracks every filament-op action",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    CHECK(tc.action_tracks_step_operation(AmsAction::SELECTING));
    CHECK(tc.action_tracks_step_operation(AmsAction::HEATING));
    CHECK(tc.action_tracks_step_operation(AmsAction::LOADING));
    CHECK(tc.action_tracks_step_operation(AmsAction::PURGING));
    CHECK(tc.action_tracks_step_operation(AmsAction::UNLOADING));
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::IDLE));
    CHECK_FALSE(tc.action_tracks_step_operation(AmsAction::ERROR));
}

TEST_CASE("MedusaHC's step model spans every tool of a multi-tool run",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    const auto model = tc.get_operation_step_model(StepOperationType::LOAD_FRESH);
    // Two tools x five steps, each label naming its tool.
    REQUIRE(model.steps.size() == 10);
    CHECK(model.steps[0].label == "Tool 1 of 2: Mount tool");
    CHECK(model.steps[2].label == "Tool 1 of 2: Insert filament");
    CHECK(model.steps[5].label == "Tool 2 of 2: Mount tool");
    CHECK(model.steps[9].label == "Tool 2 of 2: Restore tool");
    // The phase the run publishes lands inside this model.
    tc.ack(); // prompt up -> phase 2
    CHECK(tc.get_system_info().operation_phase == 2);
}

// ============================================================================
// Review fixes
// ============================================================================

TEST_CASE("mount-to-feed: an abort before the first mount never touches the previous tool's "
          "heater",
          "[toolchanger][mount_feed]") {
    // The run is stopped while the carriage still carries the tool that was on
    // it, whose heater may hold a target its user set. The cleanup turns the
    // RUN's heaters off by name; a blanket M104 S0 would zero that target.
    mf::Run run(load_request());
    REQUIRE(run.current_step() == mf::Step::Mount);
    run.abort();
    const std::string cleanup = run.current_gcode().value_or("");
    CHECK(cleanup == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0\nSELECT_TOOL T=0");
    CHECK(cleanup.find("M104") == std::string::npos);
}

TEST_CASE("mount-to-feed: the next tool's preheat never lowers a hotter target",
          "[toolchanger][mount_feed][batch]") {
    // Tool 3 idles at 200C (its user set it, or a job just used it); its
    // preheat value would be 143C. Sending that would PULL IT DOWN.
    mf::Request r = multi_load_request();
    r.heads[1].heater_target_c = 200;
    const auto stages = mf::plan(r);
    REQUIRE(stages.size() == 8);
    CHECK(stages[1].gcode == mf::prompt_end_gcode() + "\nSELECT_TOOL T=1");
    CHECK(stages[1].gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder3") == std::string::npos);

    // A cooler idle tool (50C) still warms to the preheat value, and a cold
    // one starts from it.
    r.heads[1].heater_target_c = 50;
    CHECK(mf::plan(r)[1].gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=143") !=
          std::string::npos);
    r.heads[1].heater_target_c = 0;
    CHECK(mf::plan(r)[1].gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=143") !=
          std::string::npos);
}

TEST_CASE("MedusaHC unwinds a live run when the connection is lost or restored",
          "[toolchanger][mount_feed][medusahc]") {
    // The reconnect path: the prompt is up, nothing will ever ack it, and the
    // cleanup has to reach the printer while the new connection still carries
    // gcode.
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack();
        tc.ack();
        tc.ack(); // the prompt is up
        REQUIRE(tc.get_system_info().is_busy());

        ToolChangerTestAccess::feed_disconnect_started(tc);
        CHECK(tc.sent_.back() == mf::prompt_end_gcode() +
                                     "\nSET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                     "SELECT_TOOL T=0");
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.can_cancel_operation());
    }
    // And the stopping path, mid-purge rather than mid-prompt: no prompt_end,
    // every heater in the plan off, carriage back.
    {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // mounted
        ToolChangerTestAccess::feed_disconnect_stopping(tc);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                 "SELECT_TOOL T=0");
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.can_cancel_operation());
    }
}

TEST_CASE("MedusaHC ends the run and says so when the cleanup itself cannot be sent",
          "[toolchanger][mount_feed][medusahc]") {
    // A feed stage's send fails synchronously only when the connection cannot
    // carry it; the stock helper's 4-arg override always succeeds, so the
    // cleanup's dispatch fails through a local flag.
    struct FailingCleanupChanger : AckingChanger {
        bool fail_sends = false;
        explicit FailingCleanupChanger(int tools) : AckingChanger(tools) {}
        AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete,
                               std::function<void(const MoonrakerError&)> on_error,
                               bool silent) override {
            if (fail_sends) {
                sent_.push_back(gcode);
                return AmsErrorHelper::command_failed(gcode, "simulated send failure");
            }
            return AckingChanger::execute_gcode(gcode, std::move(on_complete), std::move(on_error),
                                                silent);
        }
    };
    FailingCleanupChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // mounted; the heat stage is in flight
    tc.fail_sends = true;
    tc.fail(); // the heat failed, and the cleanup cannot leave either

    // The cleanup was attempted, once - and the run is over, not stuck busy
    // waiting for an ack that cannot arrive.
    const std::string cleanup = "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                                "SELECT_TOOL T=0";
    REQUIRE(tc.sent_.back() == cleanup);
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == "Could not turn the heaters off");
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(std::count(tc.sent_.begin(), tc.sent_.end(), cleanup) == 1);
}

TEST_CASE("MedusaHC stops under a live run without deadlocking, and still sends the cleanup",
          "[toolchanger][mount_feed][medusahc]") {
    // Heap-allocated so a deadlocked stop() can be abandoned: the stuck thread
    // holds the backend's mutex for good, so destroying it would hang the run.
    auto* tc = new AckingChanger(4);
    tc->feed_status("ready", 0);
    REQUIRE(tc->load_filament(3).success());
    tc->ack(); // mounted; the heat stage is in flight
    REQUIRE(tc->is_running());

    auto stopped = std::make_shared<std::promise<void>>();
    std::future<void> done = stopped->get_future();
    std::thread stopper([tc, stopped]() {
        tc->stop();
        stopped->set_value();
    });
    if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        stopper.detach();
        FAIL("stop() never returned under a live mount-to-feed run");
    }
    stopper.join();

    CHECK_FALSE(tc->is_running());
    CHECK(tc->sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0\n"
                              "SELECT_TOOL T=0");
    CHECK(tc->get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc->can_cancel_operation());
    delete tc;
}

namespace {

/// The topi314 MedusaHC: a [medusahc] object and the pin_watch dock sensors,
/// no klipper-toolchanger.
PrinterDiscovery pin_watch_medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array(
        {"medusahc", "pin_watch io", "extruder", "extruder1", "extruder2", "extruder3"}));
    return hw;
}

json pin_watch_frame(int current_tool) {
    return json{{"pin_watch io", {{"current_tool", current_tool}}}};
}

} // namespace

TEST_CASE("MedusaHC: the dock sensors' mid-swap -2 under a mount-to-feed run is no fault",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.unload_blocked_reason(0).empty());

    REQUIRE(tc.load_filament(1).success());
    REQUIRE(tc.sent_.size() == 1); // the Mount stage is in flight

    // Between the head sensor and the dock sensor changing, pin_watch reports
    // -2 for a moment. That is the swap the run itself asked for.
    tc.feed(pin_watch_frame(-2));
    CHECK(tc.unload_blocked_reason(1).empty());
    CHECK(tc.get_current_action() == AmsAction::SELECTING);
    tc.feed(pin_watch_frame(1));

    tc.ack();
    CHECK(tc.sent_.back() == "M109 T1 S220");
    CHECK(tc.get_current_action() == AmsAction::HEATING);

    // Outside a swap the same reading still means the sensors lost the tool.
    tc.feed(pin_watch_frame(-2));
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());
}

namespace {

/// A tool changer on a real MoonrakerAPI, so a stage's send meets the API's
/// busy gate exactly as it does on a printer.
class LiveApiChanger : public AmsBackendToolChanger {
  public:
    LiveApiChanger(IMoonrakerAPI* api, IMoonrakerClient* client)
        : AmsBackendToolChanger(api, client) {
        set_discovered_tools({"T0", "T1", "T2", "T3"});
        set_mounts_to_feed(true);
        running_ = true;
    }
    ~LiveApiChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    [[nodiscard]] bool toolhead_homed() const override {
        return true;
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }
};

} // namespace

TEST_CASE_METHOD(BusyGuardFixture,
                 "MedusaHC's heat stage is answered even while the printer reads busy",
                 "[toolchanger][mount_feed][medusahc]") {
    // A swap leaves idle_timeout reading "Printing" for a while. A discretionary
    // command sent then (a bare M109 is one) goes out fire-and-forget and its
    // answer is dropped, which would leave the run waiting at Heat forever.
    LiveApiChanger tc(api.get(), &mock_client);
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    begin_blocking_episode();

    REQUIRE(tc.load_filament(2).success()); // already mounted: Heat goes first
    REQUIRE_FALSE(mock_client.gcode_script_history().empty());
    CHECK(mock_client.gcode_script_history().back().find("M109 T2") != std::string::npos);

    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    // The heat stage's answer moved the run on to the operator prompt.
    CHECK(mock_client.gcode_script_history().back().find("action:prompt_") != std::string::npos);
    CHECK(tc.get_current_action() != AmsAction::HEATING);
    tc.cancel();
    helix::ui::UpdateQueue::instance().drain();
}
