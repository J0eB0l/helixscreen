// SPDX-License-Identifier: GPL-3.0-or-later
// Vendor state pushed by the HelixPrint plugin: the push-vs-poll rule over
// every plugin state, the tracker's resync and fallback, and the AD5X IFS and
// ACE backends applying a push through the same function as their poll.

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ace_backend_test_helper.h"
#include "../test_helpers/ace_test_access.h"
#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "app_globals.h"
#include "helix_plugin_features.h"
#include "printer_state.h"
#include "vendor_state_push.h"

#include <chrono>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

PluginFeatureEntry entry(int version, bool available, bool enabled) {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    e.reason = available ? "" : "no_vendor";
    return e;
}

/// The source a backend picks for feature `vendor_x` at contract 1.
VendorStateSource source_for(bool plugin, const PluginFeatureMap& features) {
    return vendor_state_source(plugin_feature_state(plugin, features, "vendor_x", 1));
}

json notification(const json& message) {
    return json{{"jsonrpc", "2.0"},
                {"method", kVendorStateNotifyMethod},
                {"params", json::array({message})}};
}

/// The plugin as PrinterState holds it, applied the way a probe applies it.
void set_plugin(bool installed, PluginFeatureMap features) {
    auto& plugin = get_printer_state().plugin_status_state();
    plugin.set_installed(installed);
    plugin.set_features(std::move(features));
}

struct PluginStateFixture : LVGLUITestFixture {
    ~PluginStateFixture() override {
        set_plugin(false, {});
    }
};

constexpr const char* kJsonA = R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})";
constexpr const char* kJsonB = R"({"FFMInfo":{"ffmColor1":"#00FF00","ffmType1":"PETG"}})";

json ace_slots(const char* material) {
    return json{{"slots",
                 {{{"index", 0}, {"color", "#FF0000"}, {"material", material}, {"status", "ready"}},
                  {{"index", 1}, {"color", "#00FF00"}, {"material", "PETG"}, {"status", "ready"}},
                  {{"index", 2}, {"color", "#0000FF"}, {"material", "ABS"}, {"status", "ready"}},
                  {{"index", 3}, {"color", "#FFFFFF"}, {"material", "TPU"}, {"status", "ready"}}}}};
}

} // namespace

// ============================================================================
// The rule
// ============================================================================

TEST_CASE("vendor state: push only while the plugin's feature is ready", "[plugin_vendor]") {
    SECTION("no plugin") {
        CHECK(source_for(false, {{"vendor_x", entry(1, true, true)}}) == VendorStateSource::Poll);
    }
    SECTION("a plugin without the features map") {
        CHECK(source_for(true, {}) == VendorStateSource::Poll);
    }
    SECTION("disabled") {
        CHECK(source_for(true, {{"vendor_x", entry(1, true, false)}}) == VendorStateSource::Poll);
    }
    SECTION("unavailable on this printer") {
        CHECK(source_for(true, {{"vendor_x", entry(1, false, false)}}) == VendorStateSource::Poll);
    }
    SECTION("a contract below the minimum") {
        CHECK(source_for(true, {{"vendor_x", entry(0, true, true)}}) == VendorStateSource::Poll);
    }
    SECTION("ready") {
        CHECK(source_for(true, {{"vendor_x", entry(1, true, true)}}) == VendorStateSource::Push);
    }
}

TEST_CASE("vendor state: a notification yields only its own vendor's payload", "[plugin_vendor]") {
    const json payload = {{"content", "x"}};
    CHECK(vendor_state_payload(notification({{"vendor", "ad5x"}, {"payload", payload}}), "ad5x") ==
          payload);
    CHECK_FALSE(
        vendor_state_payload(notification({{"vendor", "ace"}, {"payload", payload}}), "ad5x"));
    CHECK_FALSE(vendor_state_payload(notification({{"vendor", "ad5x"}}), "ad5x"));
    CHECK_FALSE(vendor_state_payload(notification(json::array()), "ad5x"));
    CHECK_FALSE(vendor_state_payload(json{{"params", json::array()}}, "ad5x"));
    CHECK_FALSE(vendor_state_payload(json::object(), "ad5x"));
}

TEST_CASE("vendor state: the tracker owes a read on each start of a push", "[plugin_vendor]") {
    VendorStateTracker tracker;
    CHECK_FALSE(tracker.pushing());

    CHECK_FALSE(tracker.update(PluginFeatureState::Absent, 1));
    CHECK_FALSE(tracker.pushing());

    // The push starts: one read now, then none while nothing changes
    CHECK(tracker.update(PluginFeatureState::Ready, 1));
    CHECK(tracker.pushing());
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 1));

    // A later probe (a reconnect, a restarted plugin) may have missed a change
    CHECK(tracker.update(PluginFeatureState::Ready, 2));
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 2));

    // The plugin is gone: back to polling, nothing owed
    CHECK_FALSE(tracker.update(PluginFeatureState::Absent, 3));
    CHECK_FALSE(tracker.pushing());
    CHECK_FALSE(tracker.update(PluginFeatureState::Disabled, 3));
    CHECK_FALSE(tracker.pushing());

    CHECK(tracker.update(PluginFeatureState::Ready, 3));
    CHECK(tracker.pushing());
}

// ============================================================================
// AD5X IFS
// ============================================================================

TEST_CASE("AD5X IFS ignores a push while it polls", "[plugin_vendor][ad5x_ifs]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    Ad5xIfsTestAccess::on_vendor_state(backend,
                                       {{"file", "Adventurer5M.json"}, {"content", kJsonA}});
    // Still unseen: the push was not applied
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));
}

TEST_CASE_METHOD(PluginStateFixture,
                 "AD5X IFS takes the plugin's push while ready and polls again without it",
                 "[plugin_vendor][ad5x_ifs]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);
    const auto long_ago = std::chrono::steady_clock::now() - std::chrono::minutes(5);
    const json frame = {{"extruder", {{"temperature", 25.0}}}};

    set_plugin(true, {{"vendor_ad5x", entry(1, true, true)}});
    Ad5xIfsTestAccess::set_last_json_poll_kick(backend, long_ago);
    Ad5xIfsTestAccess::handle_status(backend, frame);
    REQUIRE(Ad5xIfsTestAccess::vendor_pushing(backend));
    // The periodic poll stood down although its interval had long elapsed
    CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) == long_ago);

    SECTION("a push lands through the poll's apply: parse, then the GET_ZCOLOR follow-up") {
        const uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonA}});
        CHECK(backend.get_slot_info(0).color_rgb == 0xFF0000);
        CHECK(backend.get_slot_info(0).material == "PLA");
        CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);

        // The same body again is no change, as for a poll
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonA}});
        CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
        CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));

        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonB}});
        CHECK(backend.get_slot_info(0).color_rgb == 0x00FF00);
        CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 2);
    }

    SECTION("a payload without content is ignored") {
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"file", "Adventurer5M.json"}});
        CHECK(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));
    }

    SECTION("losing the plugin brings the poll back on the next frame") {
        set_plugin(false, {});
        Ad5xIfsTestAccess::handle_status(backend, frame);
        CHECK_FALSE(Ad5xIfsTestAccess::vendor_pushing(backend));
        CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) > long_ago);

        // A push arriving late is ignored once the backend polls
        Ad5xIfsTestAccess::on_vendor_state(backend, {{"content", kJsonA}});
        CHECK(Ad5xIfsTestAccess::note_json_content(backend, kJsonA));
    }

    SECTION("a plugin that reconnects without the feature falls back too") {
        set_plugin(true, {});
        Ad5xIfsTestAccess::handle_status(backend, frame);
        CHECK_FALSE(Ad5xIfsTestAccess::vendor_pushing(backend));
        CHECK(Ad5xIfsTestAccess::last_json_poll_kick(backend) > long_ago);
    }
}

// ============================================================================
// ACE
// ============================================================================

TEST_CASE_METHOD(PluginStateFixture,
                 "ACE applies the plugin's pushed bridge answers as its REST poll would",
                 "[plugin_vendor][ace]") {
    AmsBackendAceTestHelper backend;
    backend.test_parse_info_response(json{{"slot_count", 4}});
    AceTestAccess::set_rest_fallback(backend, true);
    const json frame = {{"extruder", {{"temperature", 25.0}}}};

    SECTION("ignored while the backend polls") {
        AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
        CHECK(backend.get_slot_info(0).material.empty());
    }

    SECTION("pushing") {
        set_plugin(true, {{"vendor_ace", entry(1, true, true)}});
        AceTestAccess::handle_status(backend, frame);
        REQUIRE(AceTestAccess::vendor_pushing(backend));

        AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
        CHECK(backend.get_slot_info(0).material == "PLA");
        CHECK(backend.get_slot_info(0).color_rgb == 0xFF0000);

        SECTION("an error answer is not applied") {
            AceTestAccess::on_vendor_state(backend,
                                           {{"slots", {{"error", "instance 0 is unavailable"}}}});
            CHECK(backend.get_slot_info(0).material == "PLA");
        }

        SECTION("losing the plugin brings the REST poll back") {
            set_plugin(false, {});
            AceTestAccess::handle_status(backend, frame);
            CHECK_FALSE(AceTestAccess::vendor_pushing(backend));

            AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("ASA")}});
            CHECK(backend.get_slot_info(0).material == "PLA");
        }
    }

    SECTION("a backend reading the ACE objects takes no push") {
        set_plugin(true, {{"vendor_ace", entry(1, true, true)}});
        AceTestAccess::set_rest_fallback(backend, false);
        AceTestAccess::handle_status(backend, frame);
        CHECK_FALSE(AceTestAccess::vendor_pushing(backend));
        AceTestAccess::on_vendor_state(backend, {{"slots", ace_slots("PLA")}});
        CHECK(backend.get_slot_info(0).material.empty());
    }
}
