// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_offer_preparing.cpp
 * @brief Power-loss recovery must not ambush a start the user already committed to.
 *
 * Run with: ./build/bin/helix-tests "[plr][print_state]"
 *
 * PlrOfferController computed its `printer_idle` signal from
 * printer_has_job(print_stats.state), which counts only PRINTING and
 * PAUSED. During a host-side pre-print block the wire still reads standby, so
 * the controller considered the printer idle and offered "Resume interrupted
 * print?" on top of a start already under way - a modal ambush whose Resume
 * button starts a DIFFERENT file than the one the user just chose.
 *
 * The decision is asserted through evaluate_offer() + the one-shot latch rather
 * than by rendering the modal; test_plr_prompt.cpp explains why the rendering
 * itself is deliberately not covered here.
 *
 * The Qidi section below pins the same decision path for the other passive
 * backend: was_interrupted is ALSO true during every normal print, so the
 * idle gate is the only thing scoping the offer to a boot after power loss.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plr_offer_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "connection_state.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "plr_offer_controller.h"
#include "plr_profile.h"
#include "print_lifecycle_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrintJobState;
using helix::PrintStartPhase;
using helix::ui::PlrOfferController;
using json = nlohmann::json;

namespace {

class PlrOfferPreparingFixture : public LVGLTestFixture {
  public:
    PlrOfferPreparingFixture() {
        auto& ps = get_printer_state();
        // The global PrinterState is shared across the shard: reset and
        // re-init, or a prior case's subjects decide this one's answers.
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        // A validated Snapmaker snapshot: the passive backend, so availability
        // needs no probe and the offer decision reduces to the idle signal.
        ps.update_from_status(
            json{{"print_stats", {{"state", "standby"}}},
                 {"virtual_sdcard",
                  {{"pl_env_valid", true}, {"file_path", "gcodes/interrupted.gcode"}}}});
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
        REQUIRE(ps.print_state().is_pl_env_valid());
        REQUIRE_FALSE(ps.print_state().pl_recovery_file().empty());
    }

    ~PlrOfferPreparingFixture() override {
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    /// set_print_start_state defers, and its callback republishes the lifecycle.
    static void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Put the app in the state it is in while running the user's own pre-start
    /// block: committed to a job, printer still reporting standby.
    static void enter_host_side_preparing(helix::PrinterState& ps) {
        ps.print_state().begin_preparing(helix::PrintJobRef{"chosen.gcode", "", ""});
        ps.print_state().set_print_start_state(PrintStartPhase::HOMING, "", 0);
        settle();
        REQUIRE(ps.print_state().get_print_job_state() == PrintJobState::STANDBY);
        REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Preparing);
    }
};

} // namespace

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR offers recovery when the printer is truly idle",
                 "[plr][print_state]") {
    // Non-vacuity baseline. Without this, every suppression assertion below
    // would also pass against a controller that never offers at all.
    PlrOfferController controller;
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR does not offer while a print is running",
                 "[plr][print_state]") {
    auto& ps = get_printer_state();

    SECTION("printing") {
        helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        helix::test::set_wire_state(ps, PrintJobState::PAUSED);
    }
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR does not offer during a host-side pre-print block",
                 "[plr][print_state]") {
    // THE BUG. print_stats still says standby, so the wire-only idle test said
    // "idle" and the recovery prompt landed on top of the start.
    auto& ps = get_printer_state();
    enter_host_side_preparing(ps);

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture,
                 "PLR does not offer during a firmware-side PRINT_START either",
                 "[plr][print_state]") {
    // The other half of Preparing: Klipper already reports printing because the
    // pre-print work lives inside PRINT_START. The wire caught this one already;
    // it must stay caught.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    ps.print_state().set_print_start_state(PrintStartPhase::HOMING, "", 0);
    settle();
    REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Preparing);

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

// ===========================================================================
// Qidi backend: was_interrupted is the availability signal, so the offer
// decision reduces to the idle signal exactly as on Snapmaker.
// ===========================================================================

namespace {

class QidiPlrOfferFixture : public LVGLTestFixture {
  public:
    QidiPlrOfferFixture() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        // Discovery found the stock macros (set_hardware's half), then the
        // boot status carried the still-true was_interrupted a power loss
        // leaves behind (update_from_status's half).
        lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
        ps.update_from_status(
            json{{"print_stats", {{"state", "standby"}}},
                 {"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
        REQUIRE(ps.print_state().is_plr_interrupted_flag());
    }

    ~QidiPlrOfferFixture() override {
        auto& ps = get_printer_state();
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    static void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR offers recovery when the printer is idle",
                 "[plr][qidi]") {
    // Non-vacuity baseline: the Qidi path reaches the prompt at all.
    PlrOfferController controller;
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR does not offer while a print is running",
                 "[plr][qidi]") {
    // was_interrupted stays true through every normal print (only
    // CLEAR_LAST_FILE clears it), so the idle gate is the whole defence
    // against offering a resume on top of the print in progress.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR does not offer without the macro capability",
                 "[plr][qidi]") {
    // A was_interrupted variable on a printer without the stock recovery
    // macros: the variable alone must not select the backend (any Klipper user
    // can SAVE_VARIABLE that name).
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 0);
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR capability is wired from discovery",
                 "[plr][qidi]") {
    // Drives the real set_hardware wire rather than poking the subject: the
    // discovery snapshot's RESUME_INTERRUPTED macro is what marks the printer
    // as running Qidi stock firmware.
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 0);

    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"gcode_macro RESUME_INTERRUPTED"}));
    ps.set_hardware(std::move(hw));
    settle();

    CHECK(ps.print_state().is_plr_resume_macro_present());
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR: a disconnect reset lets a reconnect re-offer",
                 "[plr][qidi]") {
    auto& ps = get_printer_state();

    // CONNECTED before construction: the controller seeds its connection
    // baseline from the live subject, so only then does the drop below count
    // as a CONNECTED -> not-CONNECTED edge.
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));

    PlrOfferController controller;
    settle();
    REQUIRE(PlrOfferControllerTestAccess::prompted(controller));

    // Printer drops: the controller must force the PLR subjects back to 0, or
    // the subjects' same-value guard swallows the reconnect's identical
    // status and no observer ever fires again.
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    CHECK_FALSE(ps.print_state().is_plr_resume_macro_present());
    CHECK_FALSE(ps.print_state().is_plr_interrupted_flag());

    // Reconnect: discovery re-runs (the macro subject) and the boot status
    // re-arrives (was_interrupted), each a genuine 0 -> 1 edge back into the
    // observers, so the one-shot latch re-arms and the offer fires again.
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
    ps.update_from_status(json{{"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

// ===========================================================================
// HelixPrint plugin (GENERIC): the plugin's status reply is the availability
// signal, gated on the `plr` feature.
// ===========================================================================

namespace {

class PluginPlrOfferFixture : public LVGLTestFixture {
  public:
    PluginPlrOfferFixture() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        ps.update_from_status(json{{"print_stats", {{"state", "standby"}}}});
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        set_plr_feature(true, true);
        settle();
    }

    ~PluginPlrOfferFixture() override {
        auto& ps = get_printer_state();
        ps.plugin_status_state().set_features({});
        ps.plugin_status_state().set_installed(false);
        lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::DISCONNECTED));
        settle();
    }

    static void set_plr_feature(bool available, bool enabled) {
        auto& plugin = get_printer_state().plugin_status_state();
        helix::PluginFeatureEntry entry;
        entry.version = helix::kPlrFeatureMinVersion;
        entry.available = available;
        entry.enabled = enabled;
        plugin.set_features({{helix::kPlrFeatureName, entry}});
        plugin.set_installed(true);
    }

    static helix::PlrPluginStatus offering(helix::PlrPluginBackend backend) {
        helix::PlrPluginStatus s;
        s.backend = backend;
        s.tier = 2;
        s.offer_available = true;
        helix::PlrSnapshotInfo snap;
        snap.filename = "part.gcode";
        s.snapshot = snap;
        return s;
    }

    static void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR offers when its status offers a resume",
                 "[plr][generic]") {
    PlrOfferController controller;
    settle();
    REQUIRE_FALSE(PlrOfferControllerTestAccess::prompted(controller));

    PlrOfferControllerTestAccess::plugin_status_arrived(controller,
                                                        offering(helix::PlrPluginBackend::Generic));
    settle();
    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR does not offer without a resume to offer",
                 "[plr][generic]") {
    PlrOfferController controller;
    settle();
    auto status = offering(helix::PlrPluginBackend::Generic);

    SECTION("the plugin blocks the offer") {
        status.offer_available = false;
        status.blocked_by = {"too_early"};
    }
    SECTION("the plugin has no backend for this printer") {
        status.backend = helix::PlrPluginBackend::None;
    }
    SECTION("the feature is switched off") {
        set_plr_feature(true, false);
    }
    SECTION("the feature cannot run") {
        set_plr_feature(false, false);
    }
    SECTION("a print is running") {
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        settle();
    }
    PlrOfferControllerTestAccess::plugin_status_arrived(controller, status);
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
    helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR keeps asking while the plugin answered before Klipper was ready",
                 "[plr][generic]") {
    PlrOfferController controller;
    settle();
    auto status = offering(helix::PlrPluginBackend::Generic);
    status.offer_available = false;
    status.blocked_by = {"klippy_not_ready"};
    PlrOfferControllerTestAccess::plugin_status_arrived(controller, status);
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::has_plugin_status(controller));
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR: the legacy QIDI macros offer until the plugin answers",
                 "[plr][generic][qidi]") {
    auto& ps = get_printer_state();
    // The firmware layout both serve: the legacy macros and their flag.
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
    ps.update_from_status(json{{"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
    settle();

    PlrOfferController controller;
    settle();
    // Before the plugin answers, the legacy backend is all there is.
    REQUIRE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR print_state adapter outranks the legacy QIDI macros",
                 "[plr][generic][qidi]") {
    auto& ps = get_printer_state();
    PlrOfferController controller;
    settle();

    // The plugin answers first, with no resume to offer: the adapter owns this
    // layout, so the legacy macros' flag arriving afterwards offers nothing.
    auto status = offering(helix::PlrPluginBackend::QidiPrintState);
    status.offer_available = false;
    status.blocked_by = {"too_early"};
    PlrOfferControllerTestAccess::plugin_status_arrived(controller, status);
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
    ps.update_from_status(json{{"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR: a disconnect drops the plugin's answer",
                 "[plr][generic]") {
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    PlrOfferControllerTestAccess::plugin_status_arrived(controller,
                                                        offering(helix::PlrPluginBackend::Generic));
    settle();
    REQUIRE(PlrOfferControllerTestAccess::has_plugin_status(controller));

    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::has_plugin_status(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR feature state reaches its subjects",
                 "[plr][generic][profile]") {
    auto& plugin = get_printer_state().plugin_status_state();
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Ready));
    CHECK(lv_subject_get_int(plugin.get_plr_saving_enabled_subject()) == 1);

    set_plr_feature(true, false);
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Disabled));
    CHECK(lv_subject_get_int(plugin.get_plr_saving_enabled_subject()) == 0);

    plugin.set_installed(false);
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Absent));
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR writes the resolved profile while saving is still off",
                 "[plr][generic][profile]") {
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.set_printer_type_sync("Qidi Q2");
    // Disabled: the plugin can run but is not saving, which is what a missing
    // profile (tier 0) leaves it at.
    set_plr_feature(true, false);
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    settle();

    PlrOfferController controller;
    settle();

    const json profile = client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey);
    set_moonraker_api(nullptr);
    REQUIRE(profile.is_object());
    CHECK(profile["tier"] == 2);
    CHECK(profile["backend"] == "qidi_print_state");
    CHECK(profile["storage_hint"] == "emmc");
    ps.set_printer_type_sync("");
}
