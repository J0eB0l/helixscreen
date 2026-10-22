// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_overview_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_settings_printing.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <utility>

namespace helix::ui {

namespace {
constexpr const char* kLinePrefix = "plugin_overview_";
} // namespace

PluginOverviewOverlay::~PluginOverviewOverlay() = default;

void PluginOverviewOverlay::register_callbacks() {
    register_xml_callbacks({{"on_plugin_overview_feature_clicked", on_feature_clicked}});
}

lv_obj_t* PluginOverviewOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    rows_container_ = find_required(overlay_root_, "overview_rows", get_name());
    // A switch flipped on another page comes back as a new status answer.
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PluginOverviewOverlay* self, int) { self->refresh(); }, ps.get_subjects_lifetime());
    return overlay_root_;
}

void PluginOverviewOverlay::before_show() {
    refresh();
}

std::string plugin_feature_status_text(const PluginFeatureLine& line) {
    switch (line.status) {
    case PluginFeatureStatus::Active:
        return lv_tr("Active");
    case PluginFeatureStatus::Off:
        return lv_tr("Off");
    case PluginFeatureStatus::Unavailable:
        break;
    }
    if (line.reason.empty()) {
        return lv_tr("Unavailable");
    }
    return fmt::format(lv_tr("Unavailable: {}"), line.reason);
}

void PluginOverviewOverlay::refresh() {
    if (!overlay_root_ || !rows_container_) {
        return;
    }
    safe_clean_children(rows_container_);
    auto& status = get_printer_state().plugin_status_state();
    lines_ = plugin_feature_lines(status.features());

    const std::string version = status.version().empty() ? "-" : status.version();
    create_row("setting_info_row",
               {{"label", lv_tr("Version")},
                {"label_tag", "Version"},
                {"icon", "puzzle_outline"},
                {"value", version}},
               "plugin_overview_version");
    // DECLARATIVE_OK: the lines are the plugin's feature map, data the XML cannot
    // enumerate; each is a shared settings row component.
    for (const PluginFeatureLine& line : lines_) {
        create_line(line);
    }
}

void PluginOverviewOverlay::create_line(const PluginFeatureLine& line) {
    // A known feature carries English text with a translation; an unknown one
    // is named by its key, which is not a translation key.
    const bool known = line.label != line.name;
    const std::string label = known ? lv_tr(line.label.c_str()) : line.label;
    const std::string status = plugin_feature_status_text(line);
    const std::string name = std::string(kLinePrefix) + line.name;
    if (line.home == PluginFeatureHome::None) {
        create_row("setting_info_row",
                   {{"label", label},
                    {"label_tag", known ? line.label : ""},
                    {"icon", line.icon},
                    {"value", status}},
                   name);
        return;
    }
    create_row("setting_action_row",
               {{"label", label},
                {"label_tag", known ? line.label : ""},
                {"icon", line.icon},
                {"description", status},
                {"callback", "on_plugin_overview_feature_clicked"}},
               name);
}

void PluginOverviewOverlay::create_row(const char* component,
                                       std::vector<std::pair<std::string, std::string>> attrs,
                                       const std::string& name) {
    std::vector<const char*> pairs;
    pairs.reserve(attrs.size() * 2 + 1);
    for (const auto& [key, value] : attrs) {
        pairs.push_back(key.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(rows_container_, component, pairs.data()));
    if (!row) {
        spdlog::warn("[{}] Failed to create {} for '{}'", get_name(), component, name);
        return;
    }
    // The click callback reads the feature from the name, and `helix-screen
    // ctl` addresses the line by it.
    lv_obj_set_name(row, name.c_str());
}

void PluginOverviewOverlay::on_feature_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginOverviewOverlay] on_feature_clicked");
    auto& overlay = get_plugin_overview_overlay();
    const size_t prefix_len = std::strlen(kLinePrefix);
    for (auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e)); obj != nullptr;
         obj = lv_obj_get_parent(obj)) {
        const char* name = lv_obj_get_name(obj);
        if (name == nullptr || std::strncmp(name, kLinePrefix, prefix_len) != 0) {
            continue;
        }
        for (const PluginFeatureLine& line : overlay.lines()) {
            if (line.name != name + prefix_len) {
                continue;
            }
            switch (line.home) {
            case PluginFeatureHome::Printing:
                helix::settings::get_printing_settings_overlay().show(overlay.parent_screen_);
                break;
            case PluginFeatureHome::None:
                break;
            }
            return;
        }
        return;
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
