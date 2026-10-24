// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The print status panel's printer tile and job tile, driven through printer
// state: what each chip and checklist subject publishes, and how the callout
// layout drops, glows and relayouts across a phase switch.

#include "ui_print_status_tiles.h"
#include "ui_update_queue.h"

#include "../test_helpers/print_status_panel_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_geometry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_controller.h"
#include "print_start_phase.h"
#include "print_status_chips.h"
#include "print_status_layout_fitter.h"
#include "print_status_tile_layout.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::PrintStatusTiles;
using helix::ui::UpdateQueue;
using print_status_panel_test::PrintStatusPanelFixture;

namespace {

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

std::string subject_str(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    const char* v = lv_subject_get_string(s);
    return v ? v : "";
}

/// A printer picture tagged with every region, so no chip docks for want of a point.
const ImageRegions& tagged_regions() {
    static const ImageRegions r = [] {
        ImageRegions x;
        x.src_w = 400;
        x.src_h = 400;
        x.nozzle = {0.5f, 0.35f};
        x.bed_left = {0.3f, 0.7f};
        x.bed_right = {0.7f, 0.7f};
        x.part_fan = NormPoint{0.55f, 0.4f};
        x.chamber = NormPoint{0.5f, 0.2f};
        x.light = NormPoint{0.2f, 0.1f};
        return x;
    }();
    return r;
}

/// A panel built at a geometry, with a printer tile of the given size under
/// its content box: the tile layout replaces the panel's own XML later, so
/// these cases attach the component themselves.
class TileFixture : public PrintStatusPanelFixture {
  public:
    TileFixture(int32_t screen_w, int32_t screen_h, int32_t tile_w, int32_t tile_h)
        : PrintStatusPanelFixture([screen_w, screen_h]() -> std::shared_ptr<void> {
              // The chips read the light and tool-badge subjects their owners publish.
              helix::led::LedController::instance().init(nullptr, nullptr);
              ToolState::instance().init_subjects(false);
              return std::make_shared<helix::test::ScopedGeometry>(screen_w, screen_h);
          }) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        holder_ = lv_obj_create(lv_obj_find_by_name(root_, "overlay_content"));
        lv_obj_add_flag(holder_, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(holder_, tile_w, tile_h);
        lv_obj_set_style_pad_all(holder_, 0, 0);
        tile_ =
            static_cast<lv_obj_t*>(lv_xml_create(holder_, "print_status_printer_tile", nullptr));
        REQUIRE(tile_ != nullptr);
        tiles().set_image_source([]() {
            PrintStatusTiles::Image img;
            img.path = "A:test-printer.png";
            img.natural_w = 400;
            img.natural_h = 400;
            img.regions = &tagged_regions();
            return img;
        });
        lv_obj_update_layout(root_);
    }

    PrintStatusTiles& tiles() {
        return PrintStatusPanelTestAccess::tiles(panel());
    }

    /// Deliver queued observer fires, then run the update they scheduled.
    void settle() {
        UpdateQueue::instance().drain();
        lv_obj_update_layout(root_);
        tiles().update_now();
    }

    lv_obj_t* holder_ = nullptr;
    lv_obj_t* tile_ = nullptr;
};

struct Landscape800 : TileFixture {
    Landscape800() : TileFixture(800, 480, 360, 300) {}
};
/// The printer tile the arrangement gives a portrait content box.
helix::PrintStatusTileLayout portrait_tiles(int32_t content_w, int32_t content_h) {
    return helix::decide_print_status_tile_layout(
        content_w, content_h, theme_manager_get_spacing("space_md"),
        helix::ui::PrintStatusLayoutFitter::strip_row_height(),
        helix::ui::PrintStatusLayoutFitter::strip_stacked_height());
}

// Content boxes measured from the running app (the spec's size table).
struct Portrait320 : TileFixture {
    Portrait320()
        : TileFixture(320, 480, portrait_tiles(304, 346).printer_w,
                      portrait_tiles(304, 346).printer_h) {}
};
struct Portrait272 : TileFixture {
    Portrait272()
        : TileFixture(272, 480, portrait_tiles(256, 346).printer_w,
                      portrait_tiles(256, 346).printer_h) {}
};

void set_int(lv_subject_t* s, int v) {
    lv_subject_set_int(s, v);
}

} // namespace

TEST_CASE_METHOD(Landscape800, "preparing shows checklist rows from collector input",
                 "[print_status]") {
    tiles().set_checklist_source([]() -> std::optional<PreprintChecklistInput> {
        PreprintChecklistInput in;
        in.done_in_order = {PrintStartPhase::HOMING};
        in.actual_seconds[static_cast<int>(PrintStartPhase::HOMING)] = 12;
        in.current = PrintStartPhase::HEATING_BED;
        in.current_elapsed_seconds = 30;
        in.expected_order = {PrintStartPhase::HOMING, PrintStartPhase::HEATING_BED,
                             PrintStartPhase::HEATING_NOZZLE, PrintStartPhase::BED_MESH};
        in.predicted_seconds[static_cast<int>(PrintStartPhase::HEATING_BED)] = 90;
        in.predicted_seconds[static_cast<int>(PrintStartPhase::BED_MESH)] = 120;
        return in;
    });
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_BED));
    UpdateQueue::instance().drain();

    CHECK(subject_int("ps_step_count") == 4);
    CHECK(subject_int("ps_step_0_status") == static_cast<int>(PreprintStepStatus::Done));
    CHECK(subject_str("ps_step_0_time") == "12s");
    CHECK(subject_int("ps_step_1_status") == static_cast<int>(PreprintStepStatus::Current));
    CHECK(subject_str("ps_step_1_time") == "1m");
    CHECK(subject_int("ps_step_2_status") == static_cast<int>(PreprintStepStatus::Upcoming));
    CHECK(subject_str("ps_step_2_time").empty()); // nothing predicts it
    CHECK(subject_str("ps_step_3_time") == "2m");
    CHECK(subject_str("ps_step_4_name").empty()); // past the count
    CHECK(subject_str("ps_step_position_text") == "Step 2 of 4");
    CHECK(subject_str("ps_step_current_name") == subject_str("ps_step_1_name"));
    CHECK_FALSE(subject_str("ps_step_current_name").empty());
}

TEST_CASE_METHOD(Landscape800, "printing hides checklist and shows preview", "[print_status]") {
    tiles().set_checklist_source([]() -> std::optional<PreprintChecklistInput> {
        PreprintChecklistInput in;
        in.current = PrintStartPhase::HOMING;
        return in;
    });
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HOMING));
    UpdateQueue::instance().drain();
    process_lvgl(900); // the preparing view shows after its debounce
    REQUIRE(subject_int("preparing_visible") == 1);
    REQUIRE(subject_int("ps_step_count") == 1);

    tiles().set_checklist_source(
        []() -> std::optional<PreprintChecklistInput> { return PreprintChecklistInput{}; });
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
    CHECK(subject_int("preparing_visible") == 0);
    CHECK(subject_int("ps_step_count") == 0);
    CHECK(subject_str("ps_step_position_text").empty());
}

TEST_CASE_METHOD(Landscape800, "phase switch relayouts and clears pre-print glows",
                 "[print_status]") {
    auto& temps = state().temperature_state();
    set_int(temps.get_active_extruder_temp_subject(), 1000);
    set_int(temps.get_active_extruder_target_subject(), 2200);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    settle();
    REQUIRE(subject_int("ps_chip_nozzle_shown") == 1);
    REQUIRE(subject_int("ps_glow_nozzle") == 1);
    REQUIRE(subject_int("ps_chip_nozzle_tone") == static_cast<int>(ChipTone::Active));
    const int layouts = tiles().layout_count();

    // The start sequence ends and its macro drops the preheat in the same batch.
    set_int(temps.get_active_extruder_target_subject(), 0);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
    process_lvgl(50); // the coalesced update runs on its own timer

    CHECK(tiles().layout_count() == layouts + 1);
    CHECK(subject_int("ps_glow_nozzle") == 0);
    CHECK(subject_int("ps_chip_nozzle_progress") == -1);
}

TEST_CASE_METHOD(Landscape800, "speed 104 sets speed_flow tone Warn", "[print_status]") {
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    settle();
    REQUIRE(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Normal));

    set_int(state().motion_state().get_speed_factor_subject(), 104);
    settle();
    CHECK(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Warn));
    CHECK(subject_int("ps_chip_speed_flow_shown") == 1);
    CHECK(subject_str("ps_chip_speed_flow_text").find("104") != std::string::npos);
}

namespace {

/// Spin the part fan, stop the aux fan, and lay the tile out.
void fans_part_spinning_aux_stopped(TileFixture& fx) {
    auto& fans = PrinterStateTestAccess::get_fan_state(fx.state());
    fans.init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux"});
    fans.update_fan_speed("fan", 0.5);
    fans.update_fan_speed("fan_generic aux", 0.0);
    fx.settle();
    fx.settle(); // a mode change reschedules once
}

} // namespace

TEST_CASE_METHOD(Portrait272, "fan at 0 hides on tight layout via dropped", "[print_status]") {
    fans_part_spinning_aux_stopped(*this);

    REQUIRE(subject_int("ps_callout_mode") == static_cast<int>(CalloutMode::Pinned));
    CHECK(subject_int("ps_chip_aux_fan_shown") == 0);
    // An expected value is what goes; the unusual stays.
    CHECK(subject_int("ps_chip_part_fan_shown") == 1);
    CHECK(subject_int("ps_chip_speed_flow_shown") == 1);
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Portrait320, "fan at 0 stays, dimmed, while the tile has room for it",
                 "[print_status]") {
    fans_part_spinning_aux_stopped(*this);

    REQUIRE(subject_int("ps_callout_mode") == static_cast<int>(CalloutMode::Pinned));
    CHECK(subject_int("ps_chip_aux_fan_shown") == 1);
    CHECK(subject_int("ps_chip_aux_fan_tone") == static_cast<int>(ChipTone::Dim));
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Landscape800, "outcome shows reprint and keeps preview", "[print_status]") {
    lv_obj_t* reprint = lv_obj_find_by_name(root_, "btn_reprint");
    lv_obj_t* cancel = lv_obj_find_by_name(root_, "btn_cancel");
    REQUIRE(reprint != nullptr);
    REQUIRE(cancel != nullptr);
    set_int(state().print_state().get_print_outcome_subject(),
            static_cast<int>(PrintOutcome::COMPLETE));
    UpdateQueue::instance().drain();

    CHECK_FALSE(lv_obj_has_flag(reprint, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(cancel, LV_OBJ_FLAG_HIDDEN));
    CHECK(subject_int("preparing_visible") == 0); // the job tile stays the preview
    set_int(state().print_state().get_print_outcome_subject(),
            static_cast<int>(PrintOutcome::NONE));
}

TEST_CASE_METHOD(Landscape800, "more menu lists only available actions", "[print_status]") {
    lv_subject_t* webcam = lv_xml_get_subject(nullptr, "printer_has_webcam");
    lv_subject_t* leds = lv_xml_get_subject(nullptr, "led_controllable");
    lv_subject_t* timelapse = lv_xml_get_subject(nullptr, "printer_has_timelapse");
    REQUIRE(webcam != nullptr);
    REQUIRE(leds != nullptr);
    REQUIRE(timelapse != nullptr);
    set_int(webcam, 0);
    set_int(timelapse, 0);
    set_int(leds, 1);

    lv_obj_t* anchor = lv_obj_create(test_screen());
    PrintStatusPanelTestAccess::open_more_menu(panel(), anchor);
    auto& menu = PrintStatusPanelTestAccess::more_menu(panel());
    REQUIRE(menu.is_visible());
    lv_obj_t* camera = lv_obj_find_by_name(test_screen(), "btn_more_camera");
    lv_obj_t* light = lv_obj_find_by_name(test_screen(), "btn_more_light");
    lv_obj_t* tl = lv_obj_find_by_name(test_screen(), "btn_more_timelapse");
    REQUIRE(camera != nullptr);
    REQUIRE(light != nullptr);
    REQUIRE(tl != nullptr);
    CHECK(lv_obj_has_flag(camera, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(tl, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(light, LV_OBJ_FLAG_HIDDEN));
    menu.hide();
    set_int(leds, 0);
}

TEST_CASE_METHOD(Landscape800, "nozzle chip shows active tool on a toolchanger", "[print_status]") {
    auto& tools = ToolState::instance();
    lv_subject_copy_string(tools.get_tool_badge_text_subject(), "T1");
    set_int(tools.get_show_tool_badge_subject(), 1);
    auto& temps = state().temperature_state();
    set_int(temps.get_active_extruder_temp_subject(), 2100);
    set_int(temps.get_active_extruder_target_subject(), 2100);
    settle();

    CHECK(subject_str("ps_chip_nozzle_text").rfind("T1 ", 0) == 0);

    set_int(tools.get_show_tool_badge_subject(), 0);
    lv_subject_copy_string(tools.get_tool_badge_text_subject(), "");
    settle();
    CHECK(subject_str("ps_chip_nozzle_text").rfind("T1", 0) == std::string::npos);
}
