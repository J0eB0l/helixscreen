// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_features_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "plr_plugin_rpc.h"
#include "printer_state.h"
#include "state/subject_macros.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

PluginFeaturesOverlay::~PluginFeaturesOverlay() = default;

void PluginFeaturesOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Constant 1: an unavailable feature's row names it as its `disabled`
        // subject, so the row greys its label and switch like any settings row.
        UI_MANAGED_SUBJECT_INT(feature_disabled_subject_, 1, kFeatureDisabledSubject, subjects_);
    });
}

void PluginFeaturesOverlay::register_callbacks() {
    register_xml_callbacks({{"on_plugin_feature_toggled", on_feature_toggled}});
}

lv_obj_t* PluginFeaturesOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    rows_container_ = find_required(overlay_root_, "feature_rows", get_name());
    // The plugin's answer to each switch comes back as a new feature map.
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PluginFeaturesOverlay* self, int) { self->refresh(); }, ps.get_subjects_lifetime());
    return overlay_root_;
}

void PluginFeaturesOverlay::before_show() {
    refresh();
}

void PluginFeaturesOverlay::refresh() {
    if (!overlay_root_ || !rows_container_) {
        return;
    }
    safe_clean_children(rows_container_);
    auto& status = get_printer_state().plugin_status_state();
    rows_ = status.service_has_helix_plugin() ? plugin_feature_rows(status.features())
                                              : std::vector<PluginFeatureRow>{};
    // DECLARATIVE_OK: the rows are the plugin's feature map, data the XML cannot
    // enumerate; each is the shared setting_toggle_row component.
    for (const PluginFeatureRow& row : rows_) {
        create_row(row);
    }
    spdlog::debug("[{}] {} feature rows", get_name(), rows_.size());
}

std::string plugin_feature_unavailable_text(const std::string& reason) {
    if (reason.empty()) {
        return lv_tr("Not available on this printer.");
    }
    return fmt::format(lv_tr("Not available on this printer ({})."), reason);
}

void PluginFeaturesOverlay::create_row(const PluginFeatureRow& row) {
    // A known feature carries English text with a translation; an unknown one
    // is named by its key, which is not a translation key.
    const bool known = row.label != row.name;
    const std::string label = known ? lv_tr(row.label.c_str()) : row.label;
    const std::string description = row.available ? lv_tr(row.description.c_str())
                                                  : plugin_feature_unavailable_text(row.reason);

    std::vector<std::pair<std::string, std::string>> attrs = {
        {"label", label},
        {"label_tag", known ? row.label : std::string()},
        {"icon", row.icon},
        {"description", description},
        {"description_tag", row.available ? row.description : std::string()},
        {"callback", "on_plugin_feature_toggled"},
    };
    // lv_xml_create applies every attribute to the row's root too, where
    // `disabled` is the state flag, so an available row must not name it.
    if (!row.available) {
        attrs.push_back({"disabled", kFeatureDisabledSubject});
    }
    std::vector<const char*> pairs;
    pairs.reserve(attrs.size() * 2 + 1);
    for (const auto& [name, value] : attrs) {
        pairs.push_back(name.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    auto* obj =
        static_cast<lv_obj_t*>(lv_xml_create(rows_container_, "setting_toggle_row", pairs.data()));
    if (!obj) {
        spdlog::warn("[{}] Failed to create the row for '{}'", get_name(), row.name);
        return;
    }
    // The toggle callback reads the feature from the row's name, and
    // `helix-screen ctl` addresses the row by it.
    lv_obj_set_name(obj, row.name.c_str());
    // required-names: setting_toggle_row
    if (lv_obj_t* toggle = find_required(obj, "toggle", get_name()); toggle && row.enabled) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
}

void write_plugin_feature_opt_in(const std::string& name, bool enabled) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    spdlog::info("[PluginFeatures] {} -> {}", name, enabled ? "on" : "off");
    // A dotted key sets one field of the record and leaves the others.
    api->database_post_item(
        kPluginFeaturesNamespace, std::string(kPluginFeaturesKey) + "." + name, enabled,
        [api]() { plr_rpc::refresh_features(*api); },
        [api, name](const MoonrakerError& err) {
            spdlog::warn("[PluginFeatures] Writing the opt-in for {} failed: {}", name,
                         err.message);
            plr_rpc::refresh_features(*api);
        });
}

void PluginFeaturesOverlay::on_feature_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginFeaturesOverlay] on_feature_toggled");
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* row = sw ? lv_obj_get_parent(sw) : nullptr;
    const char* name = row ? lv_obj_get_name(row) : nullptr;
    if (name == nullptr || *name == '\0') {
        spdlog::warn("[PluginFeaturesOverlay] Toggle outside a feature row");
        return;
    }
    write_plugin_feature_opt_in(name, lv_obj_has_state(sw, LV_STATE_CHECKED));
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
