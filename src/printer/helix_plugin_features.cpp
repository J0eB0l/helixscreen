// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_features.h"

#include "json_utils.h"

#include <algorithm>
#include <iterator>

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

bool plugin_feature_opted_in(const nlohmann::json& record, const std::string& name, bool acts) {
    const nlohmann::json* value = json_util::find_member(record, name.c_str());
    if (value != nullptr && value->is_boolean()) {
        return value->get<bool>();
    }
    return !acts;
}

std::vector<PluginFeatureRow> plugin_feature_rows(const PluginFeatureMap& features) {
    std::vector<PluginFeatureRow> rows;
    auto row_for = [](const std::string& name, const PluginFeatureEntry& entry) {
        PluginFeatureRow row;
        row.name = name;
        row.label = name;
        row.description = kUnknownPluginFeatureDescription;
        row.icon = kUnknownPluginFeatureIcon;
        row.enabled = entry.enabled;
        row.available = entry.available;
        row.reason = entry.available ? "" : entry.reason;
        return row;
    };
    for (const KnownPluginFeature& known : kKnownPluginFeatures) {
        const auto it = features.find(known.name);
        if (it == features.end() || known.own_switch) {
            continue;
        }
        PluginFeatureRow row = row_for(it->first, it->second);
        row.label = known.label;
        row.description = known.description;
        row.icon = known.icon;
        rows.push_back(std::move(row));
    }
    for (const auto& [name, entry] : features) {
        const bool known =
            std::any_of(std::begin(kKnownPluginFeatures), std::end(kKnownPluginFeatures),
                        [&name](const KnownPluginFeature& k) { return name == k.name; });
        if (!known) {
            rows.push_back(row_for(name, entry));
        }
    }
    return rows;
}

} // namespace helix
