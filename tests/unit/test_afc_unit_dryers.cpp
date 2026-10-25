// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_unit_dryers.cpp
 * @brief AFC unit heaters (Bambu AMS on a BridgeBox, AFC-driven ACE) reach the
 *        dryer model, and a unit AFC gains at runtime joins the layout.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "afc_unit_dryer.h"
#include "ams_backend_afc.h"
#include "fake_moonraker_client.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcDryerHelper : public AmsBackendAfc {
  public:
    explicit AfcDryerHelper(const std::vector<std::string>& names,
                            IMoonrakerClient* client = nullptr)
        : AmsBackendAfc(nullptr, client) {
        AfcTestAccess::initialize_slots(*this, names);
    }

    void feed(const nlohmann::json& params) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    std::vector<std::string> captured;
};

} // namespace helix

namespace {

std::vector<std::string> lanes(int first, int count) {
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) {
        out.push_back("lane" + std::to_string(first + i));
    }
    return out;
}

nlohmann::json one_ams2() {
    return {{"AFC", {{"units", {"BambuAMS AMS_1"}}}},
            {"AFC_BambuAMS AMS_1",
             {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
              {"has_heater", true},
              {"ams_model", "ams2"},
              {"dry_max_temp", 65},
              {"bridge_online", true},
              {"drying", false},
              {"temperature", nullptr},
              {"humidity", 41.0}}}};
}

} // namespace

TEST_CASE("AFC dryer vendor comes from the unit object prefix", "[afc][dryer]") {
    using helix::afc::DryerVendor;
    CHECK(helix::afc::dryer_vendor_for_object("AFC_BambuAMS AMS_1") == DryerVendor::Bambu);
    CHECK(helix::afc::dryer_vendor_for_object("AFC_ACE ace_1") == DryerVendor::Ace);
    CHECK(helix::afc::dryer_vendor_for_object("AFC_ACE2 ace_2") == DryerVendor::Ace2);
    CHECK(helix::afc::dryer_vendor_for_object("AFC_BoxTurtle Turtle_1") == DryerVendor::None);
    // The bridge and buffer helpers share the prefix but are not units.
    CHECK(helix::afc::dryer_vendor_for_object("AFC_BambuAMS_bridge chain1") == DryerVendor::None);
}

TEST_CASE("Heater units subscribe to their dryer fields", "[afc][dryer]") {
    const auto objects = AmsBackendAfc::required_status_objects(std::vector<std::string>{
        "AFC", "AFC_BambuAMS AMS_1", "AFC_ACE ace_1", "AFC_BoxTurtle Turtle_1"});
    auto has = [&](const char* object, const char* field) {
        const auto& f = objects.at(object);
        return std::find(f.begin(), f.end(), field) != f.end();
    };
    CHECK(has("AFC_BambuAMS AMS_1", "lanes"));
    CHECK(has("AFC_BambuAMS AMS_1", "dry_remaining"));
    CHECK(has("AFC_ACE ace_1", "ace_dryer"));
    CHECK_FALSE(has("AFC_BoxTurtle Turtle_1", "drying"));
}

TEST_CASE_METHOD(LVGLTestFixture, "An AMS 2 Pro on a BridgeBox offers its dryer", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4)};
    AfcDryerHelper& afc = *reg;
    afc.feed(one_ams2());

    DryerInfo dryer = afc.get_dryer_info(0);
    REQUIRE(dryer.supported);
    CHECK_FALSE(dryer.active);
    CHECK(dryer.max_temp_c == Catch::Approx(65.0f));
    CHECK(dryer.allows_during_print);

    // Humidity is reported while idle; the chamber temperature only while drying.
    const auto env = afc.get_system_info().units.at(0).environment;
    REQUIRE(env.has_value());
    CHECK(env->has_humidity);
    CHECK(env->humidity_pct == Catch::Approx(41.0f));
    CHECK(afc.traits().has_environment_sensors);

    afc.feed({{"AFC_BambuAMS AMS_1",
               {{"drying", true},
                {"temperature", 52.5},
                {"dry_target", 55},
                {"dry_remaining", 3600},
                {"dry_minutes", 480}}}});
    dryer = afc.get_dryer_info(0);
    CHECK(dryer.active);
    CHECK(dryer.current_temp_c == Catch::Approx(52.5f));
    CHECK(dryer.target_temp_c == Catch::Approx(55.0f));
    CHECK(dryer.duration_min == 480);
    CHECK(dryer.remaining_min == 60);

    // The cycle ends: Bambu publishes None for the cycle fields.
    afc.feed({{"AFC_BambuAMS AMS_1",
               {{"drying", false},
                {"temperature", nullptr},
                {"dry_target", nullptr},
                {"dry_remaining", nullptr}}}});
    dryer = afc.get_dryer_info(0);
    CHECK_FALSE(dryer.active);
    CHECK(dryer.target_temp_c == Catch::Approx(0.0f));
    CHECK(dryer.remaining_min == 0);

    // A unit that stops reporting both channels does not keep its last reading.
    afc.feed({{"AFC_BambuAMS AMS_1", {{"humidity", nullptr}}}});
    CHECK_FALSE(afc.get_system_info().units.at(0).environment.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Bambu drying sends AFC_BAMBU_HEATER_START/STOP",
                 "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4)};
    AfcDryerHelper& afc = *reg;
    afc.feed(one_ams2());

    REQUIRE(afc.start_drying(55.0f, 240, -1, 0).success());
    REQUIRE(afc.stop_drying(0).success());
    REQUIRE(afc.captured.size() == 2);
    CHECK(afc.captured[0] == "AFC_BAMBU_HEATER_START UNIT=AMS_1 TEMP=55 TIME=240");
    CHECK(afc.captured[1] == "AFC_BAMBU_HEATER_STOP UNIT=AMS_1");

    afc.captured.clear();
    CHECK_FALSE(afc.start_drying(80.0f, 240, -1, 0).success()); // above the 65 °C ceiling
    CHECK_FALSE(afc.start_drying(55.0f, 0, -1, 0).success());
    CHECK(afc.captured.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "A BridgeBox refusal is shown before the start", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4)};
    AfcDryerHelper& afc = *reg;
    afc.feed(one_ams2());
    afc.feed({{"AFC_BambuAMS AMS_1", {{"dry_blocked", "AMS_2 is drying on bridge power"}}}});
    CHECK_FALSE(afc.start_drying(55.0f, 240, -1, 0).success());
    afc.feed({{"AFC_BambuAMS AMS_1", {{"bridge_online", false}, {"dry_blocked", nullptr}}}});
    CHECK_FALSE(afc.start_drying(55.0f, 240, -1, 0).success());
    CHECK(afc.captured.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "An AMS HT dries to 85 °C", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 1)};
    AfcDryerHelper& afc = *reg;
    afc.feed(
        {{"AFC", {{"units", {"BambuAMS HT_1"}}}},
         {"AFC_BambuAMS HT_1", {{"lanes", {"lane1"}}, {"has_heater", true}, {"ams_model", "ht"}}}});
    const DryerInfo dryer = afc.get_dryer_info(0);
    REQUIRE(dryer.supported);
    CHECK(dryer.max_temp_c == Catch::Approx(85.0f));
}

TEST_CASE_METHOD(LVGLTestFixture, "An AMS without a heater offers no dryer", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4)};
    AfcDryerHelper& afc = *reg;
    afc.feed({{"AFC", {{"units", {"BambuAMS AMS_1"}}}},
              {"AFC_BambuAMS AMS_1",
               {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                {"has_heater", false},
                {"ams_model", "ams1"}}}});
    CHECK_FALSE(afc.get_dryer_info(0).supported);
    CHECK_FALSE(afc.start_drying(55.0f, 240, -1, 0).success());
    CHECK(afc.captured.empty());

    // A `boxed` unit gains its heater once BridgeBox confirms an AMS 2 Pro.
    afc.feed({{"AFC_BambuAMS AMS_1", {{"has_heater", true}, {"ams_model", "ams2"}}}});
    CHECK(afc.get_dryer_info(0).supported);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC-driven ACE units dry through AFC_ACE_DRY", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 8)};
    AfcDryerHelper& afc = *reg;
    afc.feed({{"AFC", {{"units", {"ACE ace_1", "ACE2 ace_2"}}}},
              {"AFC_ACE ace_1",
               {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                {"ace_connected", true},
                {"ace_dryer", "stop"},
                {"ace_temp", 24.0}}},
              {"AFC_ACE2 ace_2",
               {{"lanes", {"lane5", "lane6", "lane7", "lane8"}},
                {"ace_connected", true},
                {"ace_dryer", "drying"},
                {"ace_temp", 48.0},
                {"ace_humidity", 22.0},
                {"ace_dryer_target", 60},
                {"ace_dryer_remain", 5400},
                {"ace_dryer_duration", 7200}}}});

    const DryerInfo v1 = afc.get_dryer_info(0);
    REQUIRE(v1.supported);
    CHECK_FALSE(v1.active);
    CHECK(v1.max_temp_c == Catch::Approx(55.0f));

    const DryerInfo v2 = afc.get_dryer_info(1);
    REQUIRE(v2.supported);
    CHECK(v2.active);
    CHECK(v2.max_temp_c == Catch::Approx(70.0f));
    CHECK(v2.target_temp_c == Catch::Approx(60.0f));
    CHECK(v2.duration_min == 120); // ACE 2 reports seconds
    CHECK(v2.remaining_min == 90);

    // A V1 echoes the minutes it was asked for.
    afc.feed({{"AFC_ACE ace_1", {{"ace_dryer", "drying"}, {"ace_dryer_duration", 480}}}});
    CHECK(afc.get_dryer_info(0).duration_min == 480);

    REQUIRE(afc.start_drying(50.0f, 240, -1, 0).success());
    REQUIRE(afc.stop_drying(1).success());
    REQUIRE(afc.captured.size() == 2);
    CHECK(afc.captured[0] == "AFC_ACE_DRY UNIT=ace_1 TEMP=50 DURATION=240");
    CHECK(afc.captured[1] == "AFC_ACE_DRY_STOP UNIT=ace_2");
}

TEST_CASE_METHOD(LVGLTestFixture, "A BoxTurtle has no dryer", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4)};
    AfcDryerHelper& afc = *reg;
    afc.feed({{"AFC", {{"units", {"Box_Turtle Turtle_1"}}}},
              {"AFC_BoxTurtle Turtle_1", {{"lanes", {"lane1", "lane2", "lane3", "lane4"}}}}});
    CHECK_FALSE(afc.get_dryer_info(0).supported);
    CHECK_FALSE(afc.stop_drying(0).success());
}

TEST_CASE_METHOD(LVGLTestFixture, "A unit claimed after PREP joins the layout", "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4)};
    AfcDryerHelper& afc = *reg;
    afc.feed({{"AFC", {{"units", {"Box_Turtle Turtle_1"}}, {"lanes", lanes(1, 4)}}},
              {"AFC_BoxTurtle Turtle_1", {{"lanes", lanes(1, 4)}}},
              // A pooled BridgeBox spare: subscribed from startup, no lanes yet.
              {"AFC_BambuAMS AMS_1", {{"lanes", nlohmann::json::array()}, {"has_heater", true}}}});
    REQUIRE(afc.get_system_info().total_slots == 4);

    // BridgeBox claims a live AMS: its lanes join afc.lanes and the unit joins
    // AFC.units. The BoxTurtle's object did not change, so it sends nothing.
    afc.feed(
        {{"AFC", {{"units", {"Box_Turtle Turtle_1", "BambuAMS AMS_1"}}, {"lanes", lanes(1, 8)}}},
         {"AFC_BambuAMS AMS_1", {{"lanes", lanes(5, 4)}, {"ams_model", "ams2"}}}});

    const AmsSystemInfo info = afc.get_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.total_slots == 8);
    CHECK(info.units[1].name == "BambuAMS AMS_1");
    CHECK(afc.get_dryer_info(1).supported);
    CHECK_FALSE(afc.get_dryer_info(0).supported);
}

TEST_CASE_METHOD(LVGLTestFixture, "A unit AFC.units names before its object arrives is fetched",
                 "[afc][dryer]") {
    helix::test::FakeMoonrakerClient client;
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 4), &client};
    AfcDryerHelper& afc = *reg;
    afc.feed({{"AFC", {{"units", {"Box_Turtle Turtle_1"}}}},
              {"AFC_BoxTurtle Turtle_1", {{"lanes", lanes(1, 4)}}}});
    REQUIRE(client.rpc_count("printer.objects.query") == 0);

    // The claimed unit's object went out in an earlier frame, before AFC.units
    // named it, and is unchanged since: only a query brings it back.
    afc.feed({{"AFC", {{"units", {"Box_Turtle Turtle_1", "BambuAMS AMS_1"}}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(client.rpc_count("printer.objects.query") == 1);
    const auto* query = client.last_rpc();
    REQUIRE(query->params["objects"].contains("AFC_BambuAMS AMS_1"));
    REQUIRE(query->success_cb);

    query->success_cb(
        {{"result", {{"status", {{"AFC_BambuAMS AMS_1", {{"lanes", lanes(5, 4)}}}}}}}});
    // One drain applies the answer; the follow-up query it queues needs another.
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();

    const AmsSystemInfo info = afc.get_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.total_slots == 8);
    // The new lanes' own objects were never fetched: a second query asks for them.
    REQUIRE(client.rpc_count("printer.objects.query") == 2);
    CHECK(client.last_rpc()->params["objects"].contains("AFC_stepper lane5"));
}

TEST_CASE_METHOD(LVGLTestFixture, "A frame carrying every unit object rebuilds the layout once",
                 "[afc][dryer]") {
    helix::test::RegisteredBackend<AfcDryerHelper> reg{lanes(1, 8)};
    AfcDryerHelper& afc = *reg;
    const nlohmann::json frame = {{"AFC", {{"units", {"Box_Turtle Turtle_1", "BambuAMS AMS_1"}}}},
                                  {"AFC_BoxTurtle Turtle_1", {{"lanes", lanes(1, 4)}}},
                                  {"AFC_BambuAMS AMS_1", {{"lanes", lanes(5, 4)}}}};
    afc.feed(frame);
    REQUIRE(afc.get_system_info().units.size() == 2);

    // Both units already hold their lanes, so each one's object passes the
    // "every unit has lanes" check on its own.
    LogCapture log;
    afc.feed(frame);
    CHECK(log.count_containing("units have lane data, triggering reorganize") == 1);
    CHECK(afc.get_system_info().total_slots == 8);
}
