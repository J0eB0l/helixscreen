// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_features.h"

#include "json_utils.h"

namespace helix {

PluginFeatureMap parse_plugin_features(const nlohmann::json& status_result) {
    PluginFeatureMap features;
    const nlohmann::json* map = json_util::find_member(status_result, "features");
    if (map == nullptr || !map->is_object()) {
        return features;
    }
    for (auto it = map->begin(); it != map->end(); ++it) {
        const nlohmann::json& value = it.value();
        if (!value.is_object()) {
            continue;
        }
        PluginFeatureEntry entry;
        entry.version = json_util::safe_int(value, "version", 0);
        entry.available = json_util::safe_bool(value, "available", false);
        entry.enabled = json_util::safe_bool(value, "enabled", false);
        entry.reason = json_util::safe_string(value, "reason", "");
        if (const nlohmann::json* saving = json_util::find_member(value, "saving");
            saving != nullptr && saving->is_boolean()) {
            entry.saving = saving->get<bool>();
        }
        features[it.key()] = std::move(entry);
    }
    return features;
}

PluginFeatureState plugin_feature_state(bool plugin_available, const PluginFeatureMap& features,
                                        const std::string& name, int min_version) {
    if (!plugin_available) {
        return PluginFeatureState::Absent;
    }
    const auto it = features.find(name);
    if (it == features.end()) {
        return PluginFeatureState::Absent;
    }
    const PluginFeatureEntry& entry = it->second;
    if (entry.version < min_version || !entry.available) {
        return PluginFeatureState::Absent;
    }
    return entry.enabled ? PluginFeatureState::Ready : PluginFeatureState::Disabled;
}

} // namespace helix
