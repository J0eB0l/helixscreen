// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HelixPrint plugin features: the `features` map its server.helix.status
// reports, and the one question callers ask of it. A caller gates on a
// feature and the minimum contract version it needs, never on the plugin's
// version string, so an older plugin loses only the features it lacks.

#include <map>
#include <optional>
#include <string>

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

} // namespace helix
