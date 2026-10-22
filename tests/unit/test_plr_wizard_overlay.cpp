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

    set_subject("plr_ack_part", 1);
    set_subject("plr_ack_no_mesh", 1);
    PlrWizardOverlayTestAccess::tap_continue(get_plr_wizard_overlay());
    settle();
    CHECK(wizard_step() == PlrWizardStep::Checks);
    set_subject("plr_ack_cold_bed", 1);
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
