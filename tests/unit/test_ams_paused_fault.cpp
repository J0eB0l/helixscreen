// SPDX-License-Identifier: GPL-3.0-or-later

// A filament fault standing while the print is paused raises its dialog once,
// whichever of the two came first, and says what the `!!` line said rather than
// a message AFC queued long before. Fixture: a Voron with four AFC lanes whose
// load of lane3 stopped short of the toolhead sensor, while AFC.message still
// held a moonraker error from 40 minutes earlier.

#include "ui_ams_loading_error_modal.h"
#include "ui_panel_ams.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_error_bridge.h"
#include "ams_fault_event.h"
#include "ams_paused_fault.h"
#include "ams_state.h"
#include "fault_surface_correlation.h"
#include "print_stop_reason.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "test_helpers/afc_test_access.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

const std::string kBangResponse =
    "!! lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH\n"
    "||=====||====||==>--||\n"
    "TRG   LOAD   HUB   TOOL\n"
    "To resolve set lane loaded with SET_LANE_LOADED LANE=lane3 macro. Manually move filament "
    "with LANE_MOVE macro.";
const std::string kLane3Fault =
    "lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH";
const std::string kStaleMessage = "Error getting data from moonraker, check AFC.log for more info";

/// A backend whose standing fault carries AFC.message's stale text.
class StaleFaultBackend : public AmsBackendMock {
  public:
    explicit StaleFaultBackend(int slots) : AmsBackendMock(slots) {}
    void set_fault(bool on) {
        fault_ = on;
    }
    std::optional<ErrorEvent> current_error() const override {
        if (!fault_) {
            return std::nullopt;
        }
        return make_ams_fault_event(ErrorSource::AFC, "Filament System Error", kStaleMessage,
                                    {{"Resume", "RESUME", "afc::resume", "primary"}});
    }

  private:
    bool fault_ = false;
};

void pump(LVGLUITestFixture& fx) {
    helix::ui::UpdateQueue::instance().drain();
    fx.process_lvgl(20);
}

/// Pause the print with the tracker's stop reason, the first line of the `!!`
/// response. The reason settles first, so only the pause can raise the fault.
void pause_with_bang(LVGLUITestFixture& fx) {
    fx.state().print_state().set_print_stop_reason(gcode_error_reason_text(kBangResponse));
    pump(fx);
    test::set_wire_state(fx.state(), PrintJobState::PAUSED);
    pump(fx);
}

StaleFaultBackend* install(LVGLUITestFixture& fx) {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ams.set_action(AmsAction::IDLE);
    test::set_wire_state(fx.state(), PrintJobState::PRINTING);
    fx.state().print_state().set_print_stop_reason("");
    auto backend = std::make_unique<StaleFaultBackend>(4);
    auto* raw = backend.get();
    ams.set_backend(std::move(backend));
    pump(fx);
    return raw;
}

void uninstall(LVGLUITestFixture& fx) {
    AmsState::instance().set_action(AmsAction::IDLE);
    test::set_wire_state(fx.state(), PrintJobState::STANDBY);
    fx.state().print_state().set_print_stop_reason("");
    pump(fx);
    AmsState::instance().set_backend(nullptr);
    fault_surface_correlation::clear_for_test();
}

lv_obj_t* find_on_screen(const char* name) {
    for (lv_obj_t* root : {lv_screen_active(), lv_layer_top()}) {
        if (lv_obj_t* obj = lv_obj_find_by_name(root, name)) {
            return obj;
        }
    }
    return nullptr;
}

} // namespace

namespace helix {

class AfcPausedFaultHelper : public AmsBackendAfc {
  public:
    ~AfcPausedFaultHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    AfcPausedFaultHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2", "lane3", "lane4"};
        AfcTestAccess::initialize_slots(*this, names);
    }
    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }
};

} // namespace helix

// ============================================================================
// The trigger decision
// ============================================================================

TEST_CASE("PausedFaultTrigger fires when a fault stands while paused", "[ams-paused-fault]") {
    PausedFaultTrigger t;

    SECTION("both at once") {
        CHECK(t.update(true, true, kLane3Fault));
    }
    SECTION("paused first, then the fault") {
        CHECK_FALSE(t.update(false, true, ""));
        CHECK(t.update(true, true, kLane3Fault));
    }
    SECTION("the fault first, then paused") {
        CHECK_FALSE(t.update(true, false, kStaleMessage));
        CHECK(t.update(true, true, kLane3Fault));
    }
    SECTION("neither") {
        CHECK_FALSE(t.update(false, false, ""));
        CHECK_FALSE(t.raised());
    }
    SECTION("the same fault twice raises once") {
        CHECK(t.update(true, true, kLane3Fault));
        CHECK_FALSE(t.update(true, true, kLane3Fault));
        CHECK(t.raised());
    }
    SECTION("a resume arms it again") {
        CHECK(t.update(true, true, kLane3Fault));
        CHECK_FALSE(t.update(true, false, kLane3Fault));
        CHECK_FALSE(t.raised());
        CHECK(t.update(true, true, kLane3Fault));
    }
}

// ============================================================================
// The detail: the `!!` line, not AFC.message
// ============================================================================

TEST_CASE("AFC paused_fault tells the fault by the bang line", "[ams-paused-fault][afc]") {
    AfcPausedFaultHelper afc;
    afc.feed_afc({{"error_state", true},
                  {"current_state", "Error"},
                  {"message", {{"message", kStaleMessage}, {"type", "error"}}}});

    const auto ev = afc.paused_fault(gcode_error_reason_text(kBangResponse));
    REQUIRE(ev.has_value());
    CHECK(ev->detail == kLane3Fault);
    // A failed load leaves no lane loaded; the fault names lane3.
    bool ejects_lane3 = false;
    for (const auto& a : ev->recovery_actions) {
        ejects_lane3 |= a.gcode == "LANE_UNLOAD LANE=lane3";
    }
    CHECK(ejects_lane3);

    SECTION("with no cause it is current_error()") {
        const auto plain = afc.paused_fault("");
        REQUIRE(plain.has_value());
        CHECK(plain->detail == kStaleMessage);
    }
    SECTION("no error_state, no fault") {
        afc.feed_afc({{"error_state", false}, {"current_state", "Idle"}});
        CHECK_FALSE(afc.paused_fault(kLane3Fault).has_value());
    }
}

TEST_CASE("AFC status detail drops an error message AFC is no longer in",
          "[ams-paused-fault][afc]") {
    AfcPausedFaultHelper afc;

    SECTION("error_state false: the stale error is not the detail") {
        afc.feed_afc({{"error_state", false},
                      {"current_state", "Idle"},
                      {"message", {{"message", kStaleMessage}, {"type", "error"}}}});
        CHECK(afc.get_system_info().operation_detail.empty());
    }
    SECTION("the fault clearing in a later frame drops it too") {
        afc.feed_afc({{"error_state", true},
                      {"current_state", "Error"},
                      {"message", {{"message", kStaleMessage}, {"type", "error"}}}});
        REQUIRE(afc.get_system_info().operation_detail == kStaleMessage);
        afc.feed_afc({{"error_state", false}});
        CHECK(afc.get_system_info().operation_detail.empty());
    }
    SECTION("a warning still shows") {
        afc.feed_afc({{"error_state", false},
                      {"current_state", "Idle"},
                      {"message", {{"message", "Buffer nudged"}, {"type", "warning"}}}});
        CHECK(afc.get_system_info().operation_detail == "Buffer nudged");
    }
}

// ============================================================================
// Desktop: AmsErrorBridge raises the recovery modal
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "AmsErrorBridge raises a fault that stood before the pause",
                 "[ams-paused-fault][ams-bridge]") {
    auto* backend = install(*this);
    backend->set_fault(true);
    AmsState::instance().set_action(AmsAction::ERROR);

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    AmsErrorBridge bridge(presenter);
    bridge.start();
    pump(*this);
    // The earlier fault, answered before this print reached it.
    REQUIRE(presenter.is_visible());
    RecoveryModalPresenterTestAccess::user_dismiss(presenter);
    pump(*this);
    REQUIRE_FALSE(presenter.is_visible());

    // No ERROR edge: error_state never fell. The pause alone raises it.
    pause_with_bang(*this);
    CHECK(presenter.is_visible());
    CHECK(RecoveryModalPresenterTestAccess::shown_detail(presenter) == kLane3Fault);

    SECTION("once per fault") {
        RecoveryModalPresenterTestAccess::user_dismiss(presenter);
        pump(*this);
        fault_surface_correlation::clear_for_test();
        test::set_wire_state(state(), PrintJobState::PAUSED);
        state().print_state().set_print_stop_reason(kLane3Fault);
        AmsState::instance().set_action(AmsAction::ERROR);
        pump(*this);
        CHECK_FALSE(presenter.is_visible());
    }

    uninstall(*this);
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsErrorBridge raises a fault that comes after the pause",
                 "[ams-paused-fault][ams-bridge]") {
    auto* backend = install(*this);
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    AmsErrorBridge bridge(presenter);
    bridge.start();
    pause_with_bang(*this);
    REQUIRE_FALSE(presenter.is_visible());

    backend->set_fault(true);
    AmsState::instance().set_action(AmsAction::ERROR);
    pump(*this);
    CHECK(presenter.is_visible());
    CHECK(RecoveryModalPresenterTestAccess::shown_detail(presenter) == kLane3Fault);

    uninstall(*this);
}

// ============================================================================
// Firmware: the AMS loading-error dialog, AMS panel closed
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "AmsPausedFaultDialog opens without the AMS panel",
                 "[ams-paused-fault][ams]") {
    auto* backend = install(*this);
    const AmsPanel* panel_before = get_existing_ams_panel();

    helix::ui::AmsPausedFaultDialog dialog(state());
    backend->set_fault(true);
    AmsState::instance().set_action(AmsAction::ERROR);
    pump(*this);
    CHECK_FALSE(dialog.is_visible());

    pause_with_bang(*this);
    REQUIRE(dialog.is_visible());
    lv_obj_t* message = find_on_screen("error_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)) == kLane3Fault);
    // Raised without building the AMS panel.
    CHECK(get_existing_ams_panel() == panel_before);

    SECTION("a resume takes it down") {
        test::set_wire_state(state(), PrintJobState::PRINTING);
        pump(*this);
        CHECK_FALSE(dialog.is_visible());
    }
    SECTION("closed, the same fault stays down") {
        lv_obj_t* close = find_on_screen("btn_secondary");
        REQUIRE(close != nullptr);
        lv_obj_send_event(close, LV_EVENT_CLICKED, nullptr);
        pump(*this);
        REQUIRE_FALSE(dialog.is_visible());
        // The mock's fault outlives clear_fault(), so only the trigger decides.
        lv_subject_notify(state().print_state().get_print_state_enum_subject());
        pump(*this);
        CHECK_FALSE(dialog.is_visible());
    }

    uninstall(*this);
    pump(*this);
}
