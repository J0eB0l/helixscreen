// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_settings_row.cpp
 * @brief The Printing settings page's power-loss recovery switch
 *
 * Run with: ./build/bin/helix-tests "[plr][settings]"
 *
 * The row shows only while the HelixPrint plugin's `plr` feature can run, and
 * flipping it writes the user's choice where the plugin reads it.
 */

#include "ui_panel_settings.h"
#include "ui_settings_printing.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "plr_backend.h"
#include "plr_profile.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <optional>

#include "../catch_amalgamated.hpp"

namespace {

struct PlrSettingsRowFixture : LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, get_printer_state()};
    lv_obj_t* root_ = nullptr;

    PlrSettingsRowFixture() {
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        helix::settings::get_printing_settings_overlay().register_callbacks();
        set_moonraker_api(&api);
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_printing_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~PlrSettingsRowFixture() override {
        set_moonraker_api(nullptr);
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_features({});
        plugin.set_installed(false);
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    static void set_plr_feature(bool available, bool enabled,
                                std::optional<bool> saving = std::nullopt) {
        auto& plugin = get_printer_state().plugin_status_state();
        helix::PluginFeatureEntry entry;
        entry.version = helix::kPlrFeatureMinVersion;
        entry.available = available;
        entry.enabled = enabled;
        entry.saving = saving;
        plugin.set_installed(true);
        plugin.set_features({{helix::kPlrFeatureName, entry}});
    }

    lv_obj_t* find(const char* name) const {
        return lv_obj_find_by_name(root_, name);
    }
};

} // namespace

TEST_CASE_METHOD(PlrSettingsRowFixture,
                 "PLR settings row shows only while the plugin's plr feature can run",
                 "[plr][settings]") {
    lv_obj_t* container = find("container_power_loss_recovery");
    REQUIRE(container != nullptr);
    REQUIRE(find("row_power_loss_recovery") != nullptr);
    CHECK(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));

    set_plr_feature(true, false);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "PLR settings switch writes the user's choice",
                 "[plr][settings]") {
    set_plr_feature(true, false);
    process_lvgl(5);
    lv_obj_t* row = find("row_power_loss_recovery");
    REQUIRE(row != nullptr);
    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));

    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrUserKey) ==
          helix::plr_user_json(true));

    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrUserKey) ==
          helix::plr_user_json(false));
}

TEST_CASE_METHOD(PlrSettingsRowFixture, "PLR settings switch follows saving, not enabled",
                 "[plr][settings]") {
    lv_obj_t* container = find("container_power_loss_recovery");
    REQUIRE(container != nullptr);
    lv_obj_t* toggle = lv_obj_find_by_name(find("row_power_loss_recovery"), "toggle");
    REQUIRE(toggle != nullptr);

    SECTION("saving off while a snapshot keeps the feature enabled") {
        set_plr_feature(true, true, false);
        process_lvgl(5);
        CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    }
    SECTION("saving on") {
        set_plr_feature(true, true, true);
        process_lvgl(5);
        CHECK(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    }
    SECTION("a plugin without saving: enabled stands in") {
        set_plr_feature(true, true);
        process_lvgl(5);
        CHECK(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    }
}
