// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HelixPrint plugin features: the `features` map its server.helix.status
// reports, and the one question callers ask of it. A caller gates on a
// feature and the minimum contract version it needs, never on the plugin's
// version string, so an older plugin loses only the features it lacks.

#include "translation_loader.h" // TR_NOOP

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/// What a caller may do with one plugin feature.
enum class PluginFeatureState {
    Absent,   ///< no plugin, no such feature, too old a contract, or it cannot run
    Disabled, ///< the feature can run but its switch is off
    Ready,    ///< the feature can run and is switched on
};

/// One entry of the status `features` map.
struct PluginFeatureEntry {
    int version = 0;        ///< the feature's own contract version
    bool available = false; ///< its module loaded and its requirements hold
    bool enabled = false;   ///< switched on (never true while unavailable)
    /// The feature's own saving switch as applied, where it reports one apart
    /// from `enabled` (plr: `enabled` also holds while a snapshot waits).
    std::optional<bool> saving;
    std::string reason; ///< why it is unavailable; empty when it is available
};

using PluginFeatureMap = std::map<std::string, PluginFeatureEntry>;

/// The `features` map of a server.helix.status result. Empty for a plugin
/// that predates the map, or when it is not an object; an entry that is not
/// an object is skipped.
PluginFeatureMap parse_plugin_features(const nlohmann::json& status_result);

/// The state of feature @p name for a caller that needs contract
/// @p min_version or newer. @p plugin_available is the plugin itself being
/// installed and enabled: without it every feature is Absent.
PluginFeatureState plugin_feature_state(bool plugin_available, const PluginFeatureMap& features,
                                        const std::string& name, int min_version);

/// Where the user's opt-ins live: the app's settings namespace, this key,
/// {"<feature>": bool, ...}. The plugin reads it when a feature acts.
inline constexpr const char* kPluginFeaturesNamespace = "helix-screen";
inline constexpr const char* kPluginFeaturesKey = "plugin_features";

/// The user's opt-in for feature @p name from the plugin_features record, by
/// the plugin's own rule: a bool under the name is the choice, anything else
/// the default, on for a feature that only observes and off for one that
/// @p acts on a print or a spool.
bool plugin_feature_opted_in(const nlohmann::json& record, const std::string& name, bool acts);

/// How a settings row for a plugin feature's switch shows, by the plugin's
/// state. Values are what the row's mode subject carries.
enum class PluginSwitchMode {
    Hidden = 0,      ///< the plugin runs, but the feature cannot on this printer
    Switch = 1,      ///< a working switch (Ready or Disabled)
    NeedsPlugin = 2, ///< no plugin: greyed, with an Install action
    NeedsUpdate = 3, ///< a plugin too old for the feature: greyed, update it
};

/// The row mode for feature @p name at contract @p min_version. Absent folds
/// together cases the row treats apart: no plugin and an outdated plugin are
/// fixed by installing or updating it, while a feature the plugin reports
/// `available: false` cannot run here at all and shows no row.
PluginSwitchMode plugin_switch_mode(bool plugin_available, const PluginFeatureMap& features,
                                    const std::string& name, int min_version);

/// Where a feature's setting lives, for the plugin overview to lead to it.
enum class PluginFeatureHome {
    None,     ///< no setting: the overview shows its status only
    Printing, ///< Settings > Printing
};

/// What this app knows about a plugin feature. Labels are English; the UI
/// translates them.
struct KnownPluginFeature {
    const char* name;
    const char* label;
    const char* icon;
    PluginFeatureHome home;
};

/// Every feature this app can name. A feature the plugin reports that is not
/// listed shows by its key, with no setting to lead to.
inline constexpr KnownPluginFeature kKnownPluginFeatures[] = {
    {"print_hooks", TR_NOOP("Start options for prints from other apps"), "play_circle",
     PluginFeatureHome::Printing},
    {"plr", TR_NOOP("Power-loss recovery"), "power_plug", PluginFeatureHome::Printing},
    {"convert_3mf", TR_NOOP("3MF conversion"), "cube_outline", PluginFeatureHome::None},
};

/// The icon of a feature this app does not know.
inline constexpr const char* kUnknownPluginFeatureIcon = "puzzle_outline";

/// A feature's state as the plugin overview reports it.
enum class PluginFeatureStatus {
    Active,      ///< available and switched on
    Off,         ///< available and switched off
    Unavailable, ///< cannot run here; the plugin says why
};

/// One line of the plugin overview.
struct PluginFeatureLine {
    std::string name;  ///< the feature's key in the status map
    std::string label; ///< English, or the key itself for an unknown feature
    std::string icon;
    PluginFeatureStatus status = PluginFeatureStatus::Unavailable;
    std::string reason; ///< why it is unavailable; empty otherwise
    PluginFeatureHome home = PluginFeatureHome::None;
};

/// The overview lines for @p features: known features in kKnownPluginFeatures
/// order, then unknown ones by name.
std::vector<PluginFeatureLine> plugin_feature_lines(const PluginFeatureMap& features);

} // namespace helix
