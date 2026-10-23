// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_features.h"

#include <iterator>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

PluginFeatureMap plr_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    features["plr"] = PluginFeatureEntry{version, available, enabled, available ? "" : "why"};
    return features;
}

} // namespace

TEST_CASE("plugin_feature_state over plugin and feature states", "[plugin_feature]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        int min_version;
        PluginFeatureState expected;
    };
    const Row rows[] = {
        {"no plugin", false, plr_map(1, true, true), 1, PluginFeatureState::Absent},
        {"old plugin without a features map", true, {}, 1, PluginFeatureState::Absent},
        {"no minimum", true, plr_map(1, true, true), 0, PluginFeatureState::Ready},
        {"contract below the minimum", true, plr_map(1, true, true), 2, PluginFeatureState::Absent},
        {"contract at the minimum", true, plr_map(2, true, true), 2, PluginFeatureState::Ready},
        {"contract above the minimum", true, plr_map(3, true, true), 2, PluginFeatureState::Ready},
        {"unavailable", true, plr_map(1, false, false), 1, PluginFeatureState::Absent},
        {"unavailable but switched on", true, plr_map(1, false, true), 1,
         PluginFeatureState::Absent},
        {"switched off", true, plr_map(1, true, false), 1, PluginFeatureState::Disabled},
        {"old contract switched off", true, plr_map(1, true, false), 2, PluginFeatureState::Absent},
        {"ready", true, plr_map(1, true, true), 1, PluginFeatureState::Ready},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        CHECK(plugin_feature_state(row.plugin_available, row.features, "plr", row.min_version) ==
              row.expected);
    }

    INFO("feature not in the map");
    CHECK(plugin_feature_state(true, plr_map(1, true, true), "file_analysis", 1) ==
          PluginFeatureState::Absent);
}

TEST_CASE("parse_plugin_features reads the status features map", "[plugin_feature]") {
    const json status = {
        {"enabled", true},
        {"api", 2},
        {"features",
         {{"plr", {{"version", 2}, {"available", true}, {"enabled", false}, {"reason", nullptr}}},
          {"event_journal",
           {{"version", 1}, {"available", false}, {"enabled", false}, {"reason", "moonraker<0.9"}}},
          {"bogus", "not an object"}}},
    };

    const PluginFeatureMap features = parse_plugin_features(status);

    REQUIRE(features.size() == 2);
    const PluginFeatureEntry& plr = features.at("plr");
    CHECK(plr.version == 2);
    CHECK(plr.available);
    CHECK_FALSE(plr.enabled);
    CHECK(plr.reason.empty());
    const PluginFeatureEntry& journal = features.at("event_journal");
    CHECK_FALSE(journal.available);
    CHECK(journal.reason == "moonraker<0.9");
    CHECK(features.count("bogus") == 0);
}

TEST_CASE("parse_plugin_features tolerates old and malformed replies", "[plugin_feature]") {
    CHECK(parse_plugin_features(json{{"enabled", true}, {"version", "1.2.0"}}).empty());
    CHECK(parse_plugin_features(json{{"features", json::array({1, 2})}}).empty());
    CHECK(parse_plugin_features(json{{"features", nullptr}}).empty());
    CHECK(parse_plugin_features(json::array()).empty());

    const PluginFeatureMap features = parse_plugin_features(
        json{{"features", {{"plr", {{"version", "two"}, {"available", "yes"}}}}}});
    REQUIRE(features.count("plr") == 1);
    CHECK(features.at("plr").version == 0);
    CHECK_FALSE(features.at("plr").enabled);
    CHECK(plugin_feature_state(true, features, "plr", 1) == PluginFeatureState::Absent);
}

TEST_CASE("plugin_reason_text says what a reason code means", "[plugin_feature]") {
    CHECK(plugin_reason_text("no_virtual_sdcard") == "Klipper has no [virtual_sdcard] section.");
    CHECK(plugin_reason_text("not_listed") ==
          "Power-loss recovery isn't supported on this printer yet.");
    // A code with a detail suffix maps on the code before the colon.
    CHECK(plugin_reason_text("snapshot_unreachable: /webcam/?action=snapshot") ==
          "The plugin can't reach the camera, so this screen takes the photos.");
    CHECK(plugin_reason_text("load_failed: SyntaxError: bad input") ==
          "Part of the plugin failed to load. Reinstall the plugin.");
    // Anything else reads as the plugin wrote it.
    CHECK(plugin_reason_text("moonraker<0.9") == "moonraker<0.9");
    CHECK(plugin_reason_text("unsafe gcodes root: it holds the config directory") ==
          "unsafe gcodes root: it holds the config directory");
    CHECK(plugin_reason_text("").empty());
}

TEST_CASE("every plugin reason code is listed once", "[plugin_feature]") {
    for (size_t i = 0; i < std::size(kPluginReasonTexts); ++i) {
        for (size_t j = i + 1; j < std::size(kPluginReasonTexts); ++j) {
            CHECK(std::string(kPluginReasonTexts[i].code) != kPluginReasonTexts[j].code);
        }
    }
}
