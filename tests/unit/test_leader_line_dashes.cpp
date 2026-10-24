// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The dashes a leader draws along its legs, including the diagonal leg LVGL
// does not dash itself.

#include "ui_leader_line.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using helix::ui::leader_dashes;
using helix::ui::LeaderPoint;

namespace {
float dash_length(const helix::ui::LeaderDash& d) {
    return std::hypot(d.b.x - d.a.x, d.b.y - d.a.y);
}
} // namespace

TEST_CASE("a diagonal leg is dashed", "[leader_line]") {
    // 50px at 45 degrees: dash 3, gap 5 gives dashes starting every 8px.
    const LeaderPoint pts[] = {{0, 0}, {50 / std::sqrt(2.0f), 50 / std::sqrt(2.0f)}};
    const auto d = leader_dashes(pts, 2, 3, 5);
    REQUIRE(d.size() == 7); // starts at 0, 8, ... 48
    for (size_t i = 0; i + 1 < d.size(); ++i)
        CHECK(dash_length(d[i]) == Catch::Approx(3).margin(0.01));
    CHECK(dash_length(d.back()) == Catch::Approx(2).margin(0.01)); // cut by the leg's end
    // Each dash runs along the diagonal.
    CHECK(d[1].a.x == Catch::Approx(d[1].a.y).margin(0.01));
    CHECK(std::hypot(d[1].a.x, d[1].a.y) == Catch::Approx(8).margin(0.01));
}

TEST_CASE("the dash pattern runs on across the elbow", "[leader_line]") {
    // 10px down, then 10px right: the second leg starts 2px into a gap.
    const LeaderPoint pts[] = {{0, 0}, {0, 10}, {10, 10}};
    const auto d = leader_dashes(pts, 3, 3, 5);
    // First leg: dashes 0-3, 8-10 (cut at the elbow, 2px of 3 drawn).
    REQUIRE(d.size() >= 3);
    CHECK(d[1].a.y == Catch::Approx(8));
    CHECK(d[1].b.y == Catch::Approx(10));
    // The cut dash finishes its last 1px on the second leg.
    CHECK(d[2].a.x == Catch::Approx(0));
    CHECK(d[2].b.x == Catch::Approx(1));
    // Then a 5px gap.
    REQUIRE(d.size() >= 4);
    CHECK(d[3].a.x == Catch::Approx(6));
}

TEST_CASE("a rounding remainder at a dash's end does not stall the walk", "[leader_line]") {
    // An ordinary 45-degree leader whose float phase lands a hair short of the
    // dash's end, where t + remainder rounds back to t.
    const LeaderPoint pts[] = {{22, 258}, {74.3258972f, 310.325897f}, {246.325897f, 310.325897f}};
    const auto d = leader_dashes(pts, 3, 2, 3);
    // About 246px of leader at a 5px period.
    CHECK(d.size() >= 45);
    CHECK(d.size() <= 52);
    CHECK(d.back().b.x == Catch::Approx(246.325897f).margin(2));
}

TEST_CASE("coordinates too large or infinite still give a bounded result", "[leader_line]") {
    // Far enough out that a 2px step does not change a float at all.
    const LeaderPoint huge[] = {{0, 0}, {6e7f, 0}};
    CHECK(leader_dashes(huge, 2, 2, 3).size() <= helix::ui::kMaxLeaderDashSteps);
    const LeaderPoint inf[] = {{0, 0}, {INFINITY, 0}};
    CHECK(leader_dashes(inf, 2, 2, 3).empty());
}

TEST_CASE("no dash style draws nothing here", "[leader_line]") {
    const LeaderPoint pts[] = {{0, 0}, {10, 10}};
    CHECK(leader_dashes(pts, 2, 0, 5).empty());
    CHECK(leader_dashes(pts, 1, 3, 5).empty());
}
