// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plugin_feature_row.cpp
 * @brief A plugin_feature_toggle_row switched through plugin_features
 *
 * Run with: ./build/bin/helix-tests "[plugin_feature][settings]"
 *
 * The Printing page's print_hooks row: its switch shows the plugin's
 * `enabled`, and flipping it writes helix-screen/plugin_features.print_hooks.
 */

#include "ui_panel_settings.h"
#include "ui_plugin_feature_row.h"
#include "ui_settings_printing.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standing_print_options.h"

#include "../catch_amalgamated.hpp"

namespace {

struct PrintHooksRowFixture : LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, get_printer_state()};
    lv_obj_t* root_ = nullptr;

    PrintHooksRowFixture() {
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        helix::settings::get_printing_settings_overlay().register_callbacks();
        helix::ui::register_plugin_feature_row_callbacks();
        set_moonraker_api(&api);
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_printing_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~PrintHooksRowFixture() override {
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

    static void set_print_hooks(bool enabled) {
        auto& plugin = get_printer_state().plugin_status_state();
        helix::PluginFeatureEntry entry;
        entry.version = helix::kPrintHooksMinVersion;
        entry.available = true;
        entry.enabled = enabled;
        plugin.set_installed(true);
        plugin.set_features({{helix::kPrintHooksFeature, entry}});
    }

    lv_obj_t* toggle() const {
        lv_obj_t* row = lv_obj_find_by_name(root_, "row_print_hooks");
        lv_obj_t* switch_row =
            row ? lv_obj_find_by_name(row, "plugin_feature_row_print_hooks") : nullptr;
        return switch_row ? lv_obj_find_by_name(switch_row, "toggle") : nullptr;
    }
};

} // namespace

TEST_CASE_METHOD(PrintHooksRowFixture, "the print_hooks switch shows the plugin's enabled",
                 "[plugin_feature][settings]") {
    REQUIRE(toggle() != nullptr);
    set_print_hooks(true);
    process_lvgl(5);
    CHECK(lv_obj_has_state(toggle(), LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(root_, "row_print_hooks"), LV_OBJ_FLAG_HIDDEN));

    set_print_hooks(false);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_has_state(toggle(), LV_STATE_CHECKED));
}

TEST_CASE_METHOD(PrintHooksRowFixture, "the print_hooks switch writes plugin_features",
                 "[plugin_feature][settings]") {
    set_print_hooks(false);
    process_lvgl(5);
    REQUIRE(toggle() != nullptr);
    CHECK(helix::ui::plugin_feature_of(toggle()) == "print_hooks");

    lv_obj_add_state(toggle(), LV_STATE_CHECKED);
    lv_obj_send_event(toggle(), LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.mock_db_get(helix::kPluginFeaturesNamespace, "plugin_features.print_hooks") ==
          true);

    lv_obj_remove_state(toggle(), LV_STATE_CHECKED);
    lv_obj_send_event(toggle(), LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.mock_db_get(helix::kPluginFeaturesNamespace, "plugin_features.print_hooks") ==
          false);
}

TEST_CASE("a control outside a plugin feature row names no feature", "[plugin_feature][settings]") {
    CHECK(helix::ui::plugin_feature_of(nullptr).empty());
}
