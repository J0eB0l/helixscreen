// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Which shutdown dialog opens, and which scope its split buttons start on.
 *
 * A screen on the printer's own host gets one row of buttons that go through
 * Moonraker, or straight to local power when Moonraker is down or the host
 * does not run systemd (Moonraker's own power path needs it). A screen on a different
 * host gets the Both / Printer / Screen split buttons, starting on Screen: the
 * user is standing at the screen, and taking a printer down from another
 * machine is the choice that has to be made on purpose.
 *
 * A screen whose service user has no polkit grant for logind power-off and
 * reboot gets a toast that says so, since restarting the service installs it.
 */

#include "ui_split_button.h"

#include "../../src/ui/panel_widgets/shutdown_widget.h"
#include "../test_fixtures.h"
#include "async_lifetime_guard.h"
#include "system_power.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("Shutdown dialog plan: remote screen defaults to Screen", "[shutdown][power]") {
    SECTION("connected, live verdict remote") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }

    SECTION("connected: the live verdict outranks a stale configured host") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }

    SECTION("disconnected: the configured host decides") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }
}

TEST_CASE("Shutdown dialog plan: a remote printer splits whatever this host runs",
          "[shutdown][power]") {
    for (const bool connected : {true, false}) {
        const auto plan = plan_shutdown_dialog(connected,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/false);
        CHECK(plan.mode == ShutdownDialogMode::Dual);
        CHECK(plan.dual_default == ShutdownScope::Screen);
    }
}

TEST_CASE("Shutdown dialog plan: same host stays single-scope", "[shutdown][power]") {
    SECTION("connected goes through Moonraker") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/false,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaMoonraker);
    }

    SECTION("connected on a non-systemd host goes straight to local power") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/true,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/false);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }

    SECTION("disconnected on a non-systemd host uses local power too") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/false,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/false);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }

    SECTION("disconnected falls back to local power") {
        const auto plan = plan_shutdown_dialog(/*api_connected=*/false,
                                               /*live_moonraker_remote=*/true,
                                               /*configured_host_same=*/true,
                                               /*this_host_runs_systemd=*/true);
        CHECK(plan.mode == ShutdownDialogMode::SingleViaLocalPower);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "Dual-scope shutdown modal opens on the requested scope",
                 "[shutdown][power]") {
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();

    ShutdownModal modal;
    auto noop = []() {};
    modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, ShutdownScope::Screen);
    REQUIRE(modal.show(test_screen()));
    process_lvgl(10);

    lv_obj_t* restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
    lv_obj_t* shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
    REQUIRE(restart != nullptr);
    REQUIRE(shutdown != nullptr);
    CHECK(ui_split_button_get_selected(restart) == static_cast<uint32_t>(ShutdownScope::Screen));
    CHECK(ui_split_button_get_selected(shutdown) == static_cast<uint32_t>(ShutdownScope::Screen));

    SECTION("a narrowed selection does not survive a reopen") {
        ui_split_button_set_selected(restart, static_cast<uint32_t>(ShutdownScope::Printer));
        ui_split_button_set_selected(shutdown, static_cast<uint32_t>(ShutdownScope::Both));
        modal.hide();
        process_lvgl(10);

        modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, ShutdownScope::Screen);
        REQUIRE(modal.show(test_screen()));
        process_lvgl(10);
        restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
        shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
        REQUIRE(restart != nullptr);
        REQUIRE(shutdown != nullptr);
        CHECK(ui_split_button_get_selected(restart) ==
              static_cast<uint32_t>(ShutdownScope::Screen));
        CHECK(ui_split_button_get_selected(shutdown) ==
              static_cast<uint32_t>(ShutdownScope::Screen));
    }

    modal.hide();
    process_lvgl(10);
}

TEST_CASE_METHOD(XMLTestFixture, "Dual-scope shutdown modal applies a non-XML default scope",
                 "[shutdown][power]") {
    // The XML parses with Screen selected, so only on_show() can put the
    // buttons on any other scope.
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();

    for (const ShutdownScope scope : {ShutdownScope::Printer, ShutdownScope::Both}) {
        ShutdownModal modal;
        auto noop = []() {};
        modal.set_dual_callbacks(noop, noop, noop, noop, noop, noop, scope);
        REQUIRE(modal.show(test_screen()));
        process_lvgl(10);

        lv_obj_t* restart = lv_obj_find_by_name(modal.dialog(), "split_restart");
        lv_obj_t* shutdown = lv_obj_find_by_name(modal.dialog(), "split_shutdown");
        REQUIRE(restart != nullptr);
        REQUIRE(shutdown != nullptr);
        CHECK(ui_split_button_get_selected(restart) == static_cast<uint32_t>(scope));
        CHECK(ui_split_button_get_selected(shutdown) == static_cast<uint32_t>(scope));

        modal.hide();
        process_lvgl(10);
    }
}

TEST_CASE("logind authorization errors are recognised", "[shutdown][power]") {
    CHECK(is_authorization_error("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired"));
    CHECK(is_authorization_error("org.freedesktop.DBus.Error.AccessDenied"));
    CHECK_FALSE(is_authorization_error("org.freedesktop.DBus.Error.NoReply"));
    CHECK_FALSE(is_authorization_error("org.freedesktop.login1.OperationInProgress"));
    CHECK_FALSE(is_authorization_error(""));
    CHECK_FALSE(is_authorization_error(nullptr));
}

TEST_CASE("Screen power failure toast names a missing permission", "[shutdown][power]") {
    CHECK(screen_power_failure_message(PowerResult::Dispatched, false) == nullptr);
    CHECK(screen_power_failure_message(PowerResult::Dispatched, true) == nullptr);

    const std::string denied = screen_power_failure_message(PowerResult::NotAuthorized, false);
    CHECK(denied.find("not permitted") != std::string::npos);
    CHECK(denied.find("Restart HelixScreen") != std::string::npos);
    CHECK(std::string(screen_power_failure_message(PowerResult::NotAuthorized, true)) == denied);

    CHECK(std::string(screen_power_failure_message(PowerResult::Failed, false)) ==
          "Screen shutdown failed");
    CHECK(std::string(screen_power_failure_message(PowerResult::Failed, true)) ==
          "Screen reboot failed");
}
