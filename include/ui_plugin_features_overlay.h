// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings for the HelixPrint plugin's features: one switch per feature the
// plugin reports that does not own a switch elsewhere, writing the user's
// opt-in to `helix-screen` / `plugin_features` in Moonraker's database, where
// the plugin reads it. The rows are the plugin's feature map, which the XML
// cannot enumerate.

#include "ui_observer_guard.h"

#include "helix_plugin_features.h"
#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

namespace helix::ui {

class PluginFeaturesOverlay : public OverlayBase {
  public:
    ~PluginFeaturesOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "HelixPrint Plugin";
    }
    const char* xml_component() const override {
        return "plugin_features_overlay";
    }

    lv_obj_t* create(lv_obj_t* parent) override;

    /// Rebuild the rows from the plugin's current feature map.
    void refresh();

    /// The rows on screen, in order.
    const std::vector<PluginFeatureRow>& rows() const {
        return rows_;
    }

  protected:
    void before_show() override;

  private:
    /// The always-1 subject an unavailable feature's row names as `disabled`.
    static constexpr const char* kFeatureDisabledSubject = "plugin_feature_disabled";

    void create_row(const PluginFeatureRow& row);
    static void on_feature_toggled(lv_event_t* e);

    lv_obj_t* rows_container_ = nullptr;
    std::vector<PluginFeatureRow> rows_;
    ObserverGuard probe_observer_;

    lv_subject_t feature_disabled_subject_{};
    /// Declared after the subject it deinits, so it is destroyed first.
    SubjectManager subjects_;
};

/// The user-facing line under an unavailable feature's name.
std::string plugin_feature_unavailable_text(const std::string& reason);

/// Write the user's opt-in for plugin feature @p name, then re-read the
/// plugin's feature map so every surface shows the plugin's verdict.
void write_plugin_feature_opt_in(const std::string& name, bool enabled);

inline PluginFeaturesOverlay& get_plugin_features_overlay() {
    return lazy_global<PluginFeaturesOverlay>("PluginFeaturesOverlay");
}

} // namespace helix::ui
