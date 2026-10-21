// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The spool's front flange is vented: its slots show the filament wound behind
// them, or the dark inside of an empty spool, and a spool too small to read
// them keeps a solid flange. Pixels are read straight from the canvas buffer.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "spool_vents.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

namespace {

lv_color32_t px(lv_obj_t* canvas, int32_t x, int32_t y) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    return reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride)[x];
}

bool reddish(lv_color32_t c) {
    return c.red > c.green + 40 && c.red > c.blue + 40;
}

bool grey(lv_color32_t c) {
    return std::abs(c.red - c.green) < 12 && std::abs(c.green - c.blue) < 12;
}

int brightness(lv_color32_t c) {
    return c.red + c.green + c.blue;
}

// An 80 px spool draws slots but no dots. Its front flange is centred at
// x = 54, y = 40; the top slot's centre is 0.25 * 80 = 20 px above that, and
// (58, 20) is solid flange on the same row, clear of every slot.
constexpr int32_t kSpool = 80;
constexpr int32_t kSlotX = 54;
constexpr int32_t kSlotY = 20;
constexpr int32_t kFlangeX = 58;

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: a full spool shows its filament through the slots",
                 "[spool_canvas][spool_vents]") {
    REQUIRE(helix::ui::spool_vent_detail(kSpool) == helix::ui::SpoolVentDetail::Slots);
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 1.0f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    CHECK(reddish(px(canvas, kSlotX, kSlotY)));
    CHECK(grey(px(canvas, kFlangeX, kSlotY)));
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: an empty spool's slots show the dark inside",
                 "[spool_canvas][spool_vents]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 0.02f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    const lv_color32_t slot = px(canvas, kSlotX, kSlotY);
    const lv_color32_t flange = px(canvas, kFlangeX, kSlotY);
    CHECK(grey(slot));
    CHECK(brightness(slot) + 60 < brightness(flange));
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: a small spool keeps a solid flange",
                 "[spool_canvas][spool_vents]") {
    // 28 px: the front flange is centred at x = 18, y = 14; the top slot would
    // sit 7 px above that.
    REQUIRE(helix::ui::spool_vent_detail(28) == helix::ui::SpoolVentDetail::None);
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 28);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 1.0f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    CHECK(grey(px(canvas, 18, 7)));
}
