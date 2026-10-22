// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_wizard_overlay.cpp
 * @brief The plugin resume wizard, driven end to end against the mock plugin
 *
 * Run with: ./build/bin/helix-tests "[plr][wizard_overlay]"
 *
 * The overlay talks to MoonrakerClientMock's server.helix.plr.* handlers
 * (HELIX_MOCK_PLR), so each case runs the real RPCs, the real progress
 * notifications and the real step machine. What a case pins is what reached
 * the plugin: which endpoints, with which confirmations.
 */

#include "ui_nav_manager.h"
#include "ui_plr_wizard_overlay.h"
#include "ui_update_queue.h"

#include "../../src/ui/ui_plr_prompt.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/plr_wizard_overlay_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "connection_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "plr_backend.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PlrWizardStep;
using helix::ui::get_plr_wizard_overlay;
using helix::ui::PlrWizardOverlayTestAccess;
using nlohmann::json;

namespace {

/// The mock, plus every JSON-RPC request sent through it.
struct RecordingClient : public MoonrakerClientMock {
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;
    std::vector<std::pair<std::string, json>> sent;

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        sent.emplace_back(method, params);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    [[nodiscard]] const json* last(const std::string& method) const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (it->first == method) {
                return &it->second;
            }
        }
        return nullptr;
    }

    [[nodiscard]] int count(const std::string& method) const {
        return static_cast<int>(std::count_if(sent.begin(), sent.end(),
                                              [&](const auto& s) { return s.first == method; }));
    }
};

class PlrWizardFixture : public LVGLUITestFixture {
  protected:
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    helix::ScopedEnv tier_env{"HELIX_MOCK_PLR_TIER"};
    helix::ScopedEnv warnings_env{"HELIX_MOCK_PLR_WARNINGS"};
    helix::ScopedEnv fail_env{"HELIX_MOCK_PLR_FAIL"};
    RecordingClient client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, get_printer_state()};

    PlrWizardFixture() {
        for (auto& p : panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels_.data());
        set_connection(helix::ConnectionState::CONNECTED);
        // The jog goes through the motion API, which refuses while Klipper is not ready.
        lv_subject_set_int(get_printer_state().network_state().get_klippy_state_subject(),
                           static_cast<int>(helix::KlippyState::READY));
    }

    ~PlrWizardFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    static void set_connection(helix::ConnectionState s) {
        lv_subject_set_int(
            get_printer_state().network_state().get_printer_connection_state_subject(),
            static_cast<int>(s));
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    /// Run the mock's step timers until the wizard leaves @p step.
    bool run_past(PlrWizardStep step) {
        for (int i = 0; i < 40; ++i) {
            if (wizard_step() != step) {
                return true;
            }
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(500);
        }
        return wizard_step() != step;
    }

    void open() {
        get_plr_wizard_overlay().open(&api);
        settle();
    }

    static PlrWizardStep wizard_step() {
        return PlrWizardOverlayTestAccess::step(get_plr_wizard_overlay());
    }

    static int subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

    static void set_subject(const char* name, int value) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        lv_subject_set_int(s, value);
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: tier 2 resumes through prepare and commit",
                 "[plr][wizard_overlay]") {
    open();
    REQUIRE(wizard_step() == PlrWizardStep::Checks);
    CHECK(subject("plr_wizard_tier") == 2);
    CHECK(subject("plr_need_part") == 1);
    CHECK(subject("plr_need_nozzle") == 0);
    CHECK(subject("plr_need_no_mesh") == 0);

    // Nothing moves until the part is confirmed attached.
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Checks);
    CHECK(client.count(helix::kPlrPrepareRpc) == 0);

    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Preparing);
    REQUIRE(client.last(helix::kPlrPrepareRpc) != nullptr);
    const std::string plan_id =
        client.last(helix::kPlrPrepareRpc)->at("plan_id").get<std::string>();
    CHECK_FALSE(plan_id.empty());

    REQUIRE(run_past(PlrWizardStep::Preparing));
    REQUIRE(wizard_step() == PlrWizardStep::ZStep);
    CHECK(client.count(helix::kPlrCommitRpc) == 0);

    PlrWizardOverlayTestAccess::tap_resume(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Committing);
    const json* commit = client.last(helix::kPlrCommitRpc);
    REQUIRE(commit != nullptr);
    CHECK(commit->at("plan_id") == plan_id);
    CHECK(commit->at("z_reference") == "trusted");
    CHECK(commit->at("confirmations") == json{{"part_attached", true}, {"z_confirmed", true}});

    REQUIRE(run_past(PlrWizardStep::Committing));
    CHECK(wizard_step() == PlrWizardStep::Done);
    CHECK_FALSE(client.mock_plr_interrupted());
    CHECK(client.count(helix::kPlrAbortRpc) == 0);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: tier 1 jogs Z and commits from the part top",
                 "[plr][wizard_overlay]") {
    helix::ScopedEnv tier{"HELIX_MOCK_PLR_TIER", "1"};
    open();
    REQUIRE(wizard_step() == PlrWizardStep::Checks);
    CHECK(subject("plr_wizard_tier") == 1);
    REQUIRE(subject("plr_need_nozzle") == 1);

    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Checks);
    set_subject("plr_ack_nozzle", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(run_past(PlrWizardStep::Preparing));
    REQUIRE(wizard_step() == PlrWizardStep::ZStep);

    const size_t before = client.sent.size();
    PlrWizardOverlayTestAccess::tap_jog(get_plr_wizard_overlay(), -1);
    settle();
    // The jog is the motion panel's relative move: one gcode script with a Z delta.
    bool jogged = false;
    for (size_t i = before; i < client.sent.size(); ++i) {
        if (client.sent[i].first == "printer.gcode.script" &&
            client.sent[i].second.value("script", "").find("Z-0.1") != std::string::npos) {
            jogged = true;
        }
    }
    CHECK(jogged);

    PlrWizardOverlayTestAccess::tap_resume(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Committing);
    const json* commit = client.last(helix::kPlrCommitRpc);
    REQUIRE(commit != nullptr);
    CHECK(commit->at("z_reference") == "part_top");
    CHECK(commit->at("confirmations") ==
          json{{"part_attached", true}, {"nozzle_clear", true}, {"z_jogged", true}});
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: plan warnings ask for their confirmations",
                 "[plr][wizard_overlay]") {
    helix::ScopedEnv warnings{"HELIX_MOCK_PLR_WARNINGS", "mesh_unavailable,cold_bed,layer_start"};
    open();
    REQUIRE(wizard_step() == PlrWizardStep::Checks);
    CHECK(subject("plr_need_no_mesh") == 1);
    CHECK(subject("plr_need_cold_bed") == 1);
    CHECK(subject("plr_warn_layer_start") == 1);
    CHECK(subject("plr_need_next_layer") == 1);

    set_subject("plr_ack_part", 1);
    set_subject("plr_ack_no_mesh", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Checks);
    set_subject("plr_ack_cold_bed", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Checks);
    CHECK(client.count(helix::kPlrPrepareRpc) == 0);
    set_subject("plr_ack_next_layer", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Preparing);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: a failed RPC fails the wizard",
                 "[plr][wizard_overlay]") {
    SECTION("plan") {
        helix::ScopedEnv fail{"HELIX_MOCK_PLR_FAIL", "plan"};
        open();
        CHECK(wizard_step() == PlrWizardStep::Failed);
        CHECK(client.count(helix::kPlrPrepareRpc) == 0);
    }
    SECTION("prepare stops the sequence") {
        helix::ScopedEnv fail{"HELIX_MOCK_PLR_FAIL", "prepare"};
        open();
        set_subject("plr_ack_part", 1);
        PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
        settle();
        CHECK(wizard_step() == PlrWizardStep::Failed);
        CHECK(client.count(helix::kPlrAbortRpc) == 1);
        CHECK(client.count(helix::kPlrCommitRpc) == 0);
        lv_subject_t* error = lv_xml_get_subject(nullptr, "plr_wizard_error");
        REQUIRE(error != nullptr);
        CHECK(std::string(lv_subject_get_string(error)).find("plr_klippy_error") !=
              std::string::npos);
    }
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: cancel mid-prepare aborts the plugin's sequence",
                 "[plr][wizard_overlay]") {
    open();
    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Preparing);

    PlrWizardOverlayTestAccess::tap_abort(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Aborted);
    const json* abort = client.last(helix::kPlrAbortRpc);
    REQUIRE(abort != nullptr);
    CHECK(abort->at("plan_id") == client.last(helix::kPlrPrepareRpc)->at("plan_id"));
    // The snapshot survives a cancel.
    CHECK(client.mock_plr_interrupted());
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: cancel before anything moved sends nothing",
                 "[plr][wizard_overlay]") {
    open();
    REQUIRE(wizard_step() == PlrWizardStep::Checks);
    PlrWizardOverlayTestAccess::tap_abort(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Aborted);
    CHECK(client.count(helix::kPlrAbortRpc) == 0);
    CHECK(client.count(helix::kPlrPrepareRpc) == 0);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: a disconnect mid-prepare fails and never commits",
                 "[plr][wizard_overlay]") {
    open();
    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Preparing);

    set_connection(helix::ConnectionState::DISCONNECTED);
    settle();
    CHECK(wizard_step() == PlrWizardStep::Failed);
    // The prepare reply arriving later must not move the wizard on.
    run_past(PlrWizardStep::Failed);
    CHECK(wizard_step() == PlrWizardStep::Failed);
    CHECK(client.count(helix::kPlrCommitRpc) == 0);
    set_connection(helix::ConnectionState::CONNECTED);
}

namespace {

/// Every gcode script sent from index @p from on that moves Z.
int z_moves_since(const RecordingClient& client, size_t from) {
    int n = 0;
    for (size_t i = from; i < client.sent.size(); ++i) {
        if (client.sent[i].first == "printer.gcode.script" &&
            client.sent[i].second.value("script", "").find(" Z") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

std::string subject_text(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}

} // namespace

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: a jog queued before Abort is never sent",
                 "[plr][wizard_overlay]") {
    helix::ScopedEnv tier{"HELIX_MOCK_PLR_TIER", "1"};
    open();
    set_subject("plr_ack_part", 1);
    set_subject("plr_ack_nozzle", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(run_past(PlrWizardStep::Preparing));
    REQUIRE(wizard_step() == PlrWizardStep::ZStep);

    // The first jog stays in flight; the next taps queue behind it.
    client.defer_next("printer.gcode.script");
    PlrWizardOverlayTestAccess::tap_jog(get_plr_wizard_overlay(), -1);
    PlrWizardOverlayTestAccess::tap_jog(get_plr_wizard_overlay(), -1);
    PlrWizardOverlayTestAccess::tap_jog(get_plr_wizard_overlay(), -1);
    settle();
    PlrWizardOverlayTestAccess::tap_abort(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Aborted);

    const size_t before = client.sent.size();
    client.fire_deferred("printer.gcode.script");
    settle();
    CHECK(z_moves_since(client, before) == 0);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: the screensaver does not abort a bed wait",
                 "[plr][wizard_overlay]") {
    open();
    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Preparing);

    get_plr_wizard_overlay().on_deactivating(DeactivateReason::Suspended);
    settle();
    CHECK(wizard_step() == PlrWizardStep::Preparing);
    // The prepare reply still lands in this opening.
    REQUIRE(run_past(PlrWizardStep::Preparing));
    CHECK(wizard_step() == PlrWizardStep::ZStep);
    CHECK(client.count(helix::kPlrAbortRpc) == 0);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: leaving the wizard mid-prepare aborts",
                 "[plr][wizard_overlay]") {
    open();
    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Preparing);

    NavigationManager::instance().go_back();
    settle();
    CHECK(client.count(helix::kPlrAbortRpc) == 1);
    CHECK(wizard_step() == PlrWizardStep::Aborted);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: tier 1 resumes only after jogging down",
                 "[plr][wizard_overlay]") {
    helix::ScopedEnv tier{"HELIX_MOCK_PLR_TIER", "1"};
    open();
    set_subject("plr_ack_part", 1);
    set_subject("plr_ack_nozzle", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(run_past(PlrWizardStep::Preparing));
    REQUIRE(wizard_step() == PlrWizardStep::ZStep);
    CHECK(subject("plr_z_jogged") == 0);

    PlrWizardOverlayTestAccess::tap_resume(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::ZStep);

    // Raising never reaches the part.
    PlrWizardOverlayTestAccess::tap_jog(get_plr_wizard_overlay(), 1);
    settle();
    PlrWizardOverlayTestAccess::tap_resume(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::ZStep);
    CHECK(client.count(helix::kPlrCommitRpc) == 0);

    PlrWizardOverlayTestAccess::tap_jog(get_plr_wizard_overlay(), -1);
    settle();
    CHECK(subject("plr_z_jogged") == 1);
    PlrWizardOverlayTestAccess::tap_resume(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Committing);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: progress shows the plugin's step names",
                 "[plr][wizard_overlay]") {
    open();
    set_subject("plr_ack_part", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Preparing);
    bool saw_bed = false;
    for (int i = 0; i < 20 && wizard_step() == PlrWizardStep::Preparing; ++i) {
        saw_bed = saw_bed || subject_text("plr_wizard_progress") == "Heating the bed";
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(500);
    }
    CHECK(saw_bed);

    PlrWizardOverlayTestAccess::tap_resume(get_plr_wizard_overlay());
    settle();
    REQUIRE(wizard_step() == PlrWizardStep::Committing);
    bool saw_start = false;
    bool saw_nozzle = false;
    for (int i = 0; i < 40 && wizard_step() == PlrWizardStep::Committing; ++i) {
        saw_start = saw_start || subject_text("plr_wizard_progress") == "Starting the print";
        saw_nozzle = saw_nozzle || subject_text("plr_wizard_progress") == "Heating the nozzle";
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(500);
    }
    CHECK(saw_start);
    CHECK(saw_nozzle);
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR wizard: a background plan failure shows an error",
                 "[plr][wizard_overlay]") {
    SECTION("the plugin reports the error with plan_ready") {
        helix::ScopedEnv fail{"HELIX_MOCK_PLR_FAIL", "plan_background"};
        open();
        REQUIRE(run_past(PlrWizardStep::Offer));
        CHECK(wizard_step() == PlrWizardStep::Failed);
        CHECK(subject_text("plr_wizard_error").find("multi_material") != std::string::npos);
    }
    SECTION("the plan stays pending after plan_ready") {
        helix::ScopedEnv fail{"HELIX_MOCK_PLR_FAIL", "plan_loop"};
        open();
        REQUIRE(run_past(PlrWizardStep::Offer));
        CHECK(wizard_step() == PlrWizardStep::Failed);
        CHECK(client.count(helix::kPlrPlanRpc) == 2);
    }
}

TEST_CASE_METHOD(PlrWizardFixture, "PLR prompt: Resume on a plugin plan opens the guided resume",
                 "[plr][wizard_overlay][prompt]") {
    const helix::PlrRecoveryPlan plan =
        helix::plr_build_plan(helix::PlrBackendType::GENERIC, "part.gcode", {});
    REQUIRE(plan.resume_allowed());
    helix::ui::run_plr_resume(&api, plan);
    settle();
    // Resume only opens the wizard: it asks for a plan and moves nothing.
    CHECK(client.count(helix::kPlrPlanRpc) == 1);
    CHECK(client.count("printer.gcode.script") == 0);
    CHECK(wizard_step() == PlrWizardStep::Checks);
}

TEST_CASE_METHOD(PlrWizardFixture,
                 "PLR prompt: Discard on a plugin plan clears the plugin snapshot",
                 "[plr][wizard_overlay][prompt]") {
    const helix::PlrRecoveryPlan plan =
        helix::plr_build_plan(helix::PlrBackendType::GENERIC, "part.gcode", {});
    REQUIRE(client.mock_plr_interrupted());
    helix::ui::run_plr_discard(&api, plan);
    settle();
    CHECK(client.count(helix::kPlrDiscardRpc) == 1);
    CHECK_FALSE(client.mock_plr_interrupted());
}

TEST_CASE("PLR wizard: every plugin progress step has words", "[plr][wizard_overlay]") {
    using helix::ui::plr_progress_step_text;
    for (const char* step :
         {"bed_wait", "chamber_wait", "lift", "home_xy", "position", "z_reference", "restore",
          "heat_nozzle", "purge", "move_to_resume", "start"}) {
        INFO(step);
        CHECK(plr_progress_step_text(step) != nullptr);
    }
    CHECK(std::string(plr_progress_step_text("bed_wait")) == "Heating the bed");
    CHECK(std::string(plr_progress_step_text("heat_nozzle")) == "Heating the nozzle");
    CHECK(std::string(plr_progress_step_text("purge")) == "Purging nozzle");
    CHECK(std::string(plr_progress_step_text("move_to_resume")) == "Moving back to the print");
    CHECK(plr_progress_step_text("not_a_step") == nullptr);
}
