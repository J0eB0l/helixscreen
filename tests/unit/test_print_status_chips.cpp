// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_chips.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using ui::temperature::classify_heater_status;

namespace {
ChipContent heater(int cur, int tgt, bool printing = false, bool power = false, int pwr = 0,
                   const char* badge = "", int start = 300) {
    return heater_chip(true, cur, tgt, classify_heater_status(cur, tgt, pwr), printing, power,
                       badge, start);
}
} // namespace

TEST_CASE("heater climbing shows target, bar and Active", "[print_status_chips]") {
    auto c = heater(1840, 2200, false, false, 0, "", 300);
    REQUIRE(c.shown);
    CHECK(c.text == "184 / 220°");
    CHECK(c.progress_pct == 81);
    CHECK(c.tone == ChipTone::Active);
    CHECK(c.glow);
}

TEST_CASE("heater ready shows value only", "[print_status_chips]") {
    auto c = heater(2200, 2200);
    CHECK(c.text == "220°");
    CHECK(c.tone == ChipTone::Normal);
    CHECK(c.progress_pct == -1);
}

TEST_CASE("heater off target mid-print warns", "[print_status_chips]") {
    auto c = heater(2050, 2200, true);
    CHECK(c.tone == ChipTone::Warn);
    CHECK(c.text == "205 / 220°");
}

TEST_CASE("power appended only when asked", "[print_status_chips]") {
    CHECK(heater(1840, 2200, false, true, 5).text == "184 / 220° · 5%");
    CHECK(heater(1840, 2200, false, false, 5).text == "184 / 220°");
    CHECK(heater(1840, 2200, false, true, 0).text == "184 / 220°");
}

TEST_CASE("off and cold heater hidden", "[print_status_chips]") {
    CHECK_FALSE(heater(300, 0).shown);
    CHECK(heater(600, 0).shown);
    CHECK_FALSE(
        heater_chip(false, 2200, 2200, classify_heater_status(2200, 2200, 0), false, false, "", 300)
            .shown);
}

TEST_CASE("heater_chip carries tool badge text", "[print_status_chips]") {
    CHECK(heater(2200, 2200, false, false, 0, "T1").text == "T1 220°");
}

TEST_CASE("part fan spinning is Active and glows", "[print_status_chips]") {
    auto c = fan_chip(FanRole::Part, true, 30, false, true);
    CHECK(c.text == "Part 30%");
    CHECK(c.tone == ChipTone::Active);
    CHECK(c.glow);
    CHECK(fan_chip(FanRole::Part, true, 30, false, false).text == "30%");
    CHECK(fan_chip(FanRole::Hotend, true, 30, false, true).text == "HE 30%");
    CHECK(fan_chip(FanRole::Aux, true, 30, false, true).text == "Aux 30%");
    CHECK_FALSE(fan_chip(FanRole::Part, false, 30, false, true).shown);
}

TEST_CASE("fan at 0 is Dim and expendable", "[print_status_chips]") {
    auto c = fan_chip(FanRole::Part, true, 0, false, false);
    CHECK(c.tone == ChipTone::Dim);
    CHECK(c.expendable);
    CHECK_FALSE(c.glow);
}

TEST_CASE("hotend fan automatic is expendable", "[print_status_chips]") {
    CHECK(fan_chip(FanRole::Hotend, true, 100, true, false).expendable);
    CHECK_FALSE(fan_chip(FanRole::Hotend, true, 100, false, false).expendable);
}

TEST_CASE("speed 104 warns and is not expendable", "[print_status_chips]") {
    auto c = speed_flow_chip(104, 100, "104%", "100%");
    CHECK(c.tone == ChipTone::Warn);
    CHECK_FALSE(c.expendable);
    CHECK(c.text == "104% · 100%");
    CHECK(speed_flow_chip(100, 97, "100%", "97%").tone == ChipTone::Warn);
    CHECK(speed_flow_chip(90, 100, "90%", "100%").tone == ChipTone::Warn);
}

TEST_CASE("speed and flow 100 is expendable", "[print_status_chips]") {
    auto c = speed_flow_chip(100, 100, "100%", "100%");
    CHECK(c.tone == ChipTone::Normal);
    CHECK(c.expendable);
}

TEST_CASE("light off is expendable, on glows", "[print_status_chips]") {
    CHECK_FALSE(light_chip(false, true).shown);
    auto on = light_chip(true, true);
    CHECK(on.tone == ChipTone::Active);
    CHECK(on.glow);
    auto off = light_chip(true, false);
    CHECK(off.tone == ChipTone::Dim);
    CHECK(off.expendable);
}

TEST_CASE("heater duty shows only beside the picture and only while it fits the column",
          "[print_status_chips]") {
    CHECK(heater_power_fits(CalloutMode::BothSides, 90, 90));
    CHECK(heater_power_fits(CalloutMode::OneSide, 80, 90));
    CHECK_FALSE(heater_power_fits(CalloutMode::BothSides, 91, 90));
    CHECK_FALSE(heater_power_fits(CalloutMode::Pinned, 40, 90));
    CHECK_FALSE(heater_power_fits(CalloutMode::Docked, 40, 90));
    CHECK_FALSE(heater_power_fits(CalloutMode::ImageOnly, 40, 90));
    CHECK_FALSE(heater_power_fits(CalloutMode::BothSides, 0, 90));
}
