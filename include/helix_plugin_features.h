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

/// What this app knows about a plugin feature, for its settings row. Names and
/// descriptions are English; the UI translates them.
struct KnownPluginFeature {
    const char* name;
    const char* label;
    const char* description;
    const char* icon;
    /// The feature has its own switch elsewhere, so it gets no row here.
    bool own_switch;
};

/// Every feature this app can name. A later feature adds one row; a feature
/// the plugin reports that is not listed still gets a generic row.
inline constexpr KnownPluginFeature kKnownPluginFeatures[] = {
    {"print_hooks", TR_NOOP("Options for prints started elsewhere"),
     TR_NOOP("Applies your standing print options, such as unload at end, to prints started "
             "from another app or the job queue."),
     "play_circle", false},
    // Switched by its Power-loss recovery setting
    {"plr", TR_NOOP("Power-loss recovery"), "", "", true},
    // Switched by convert_3mf in moonraker.conf
    {"convert_3mf", TR_NOOP("3MF conversion"), "", "", true},
};

/// The label, description and icon a feature this app does not know shows.
inline constexpr const char* kUnknownPluginFeatureDescription =
    TR_NOOP("A HelixPrint plugin feature.");
inline constexpr const char* kUnknownPluginFeatureIcon = "puzzle_outline";

/// One row of the plugin features settings screen.
struct PluginFeatureRow {
    std::string name;        ///< the feature's key in the map and in plugin_features
    std::string label;       ///< English, or the key itself for an unknown feature
    std::string description; ///< English
    std::string icon;
    bool enabled = false;   ///< the switch's state as the plugin applies it
    bool available = false; ///< false: shown disabled with @ref reason
    std::string reason;
};

/// The rows for @p features: every feature that does not own its switch,
/// known ones in kKnownPluginFeatures order, then unknown ones by name.
std::vector<PluginFeatureRow> plugin_feature_rows(const PluginFeatureMap& features);

} // namespace helix
