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
#include "printer_image_manager.h"
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

/// The panel's own XML built at a screen size, with a tagged printer picture.
class TileFixture : public PrintStatusPanelFixture {
  public:
    TileFixture(int32_t screen_w, int32_t screen_h)
        : PrintStatusPanelFixture([screen_w, screen_h]() -> std::shared_ptr<void> {
              // The chips read the light and tool-badge subjects their owners publish.
              helix::led::LedController::instance().init(nullptr, nullptr);
              ToolState::instance().init_subjects(false);
              // The tile follows the image-changed subject, which exists
              // before any panel, as in the app.
              helix::PrinterImageManager::instance().init_subjects();
              return std::make_shared<helix::test::ScopedGeometry>(screen_w, screen_h);
          }) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        tile_ = lv_obj_find_by_name(root_, "ps_printer_tile");
        REQUIRE(tile_ != nullptr);
        tiles().set_image_source([]() {
            PrintStatusTiles::Image img;
            img.path = "A:test-printer.png";
            img.natural_w = 400;
            img.natural_h = 400;
            img.regions = &tagged_regions();
            return img;
        });
        settle();
    }

    PrintStatusTiles& tiles() {
        return PrintStatusPanelTestAccess::tiles(panel());
    }

    /// Deliver queued observer fires, then run the update they scheduled.
    void settle() {
        UpdateQueue::instance().drain();
        lv_obj_update_layout(root_);
        UpdateQueue::instance().drain();
        lv_obj_update_layout(root_);
        tiles().update_now();
    }

    lv_obj_t* tile_ = nullptr;
};

struct Landscape800 : TileFixture {
    Landscape800() : TileFixture(800, 480) {}
};
struct Portrait320 : TileFixture {
    Portrait320() : TileFixture(320, 480) {}
};
struct Portrait272 : TileFixture {
    Portrait272() : TileFixture(272, 480) {}
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

    // One pass, and a second when the mode it picks differs from the last one.
    CHECK(tiles().layout_count() > layouts);
    CHECK(tiles().layout_count() <= layouts + 2);
    CHECK(subject_int("ps_glow_nozzle") == 0);
    CHECK(subject_int("ps_chip_nozzle_progress") == -1);
}

TEST_CASE_METHOD(Landscape800, "a heater at its target shows the check", "[print_status]") {
    auto& temps = state().temperature_state();
    set_int(temps.get_bed_target_subject(), 600);
    set_int(temps.get_bed_temp_subject(), 600);
    settle();
    REQUIRE(subject_int("ps_chip_bed_shown") == 1);
    CHECK(subject_int("ps_chip_bed_state") ==
          static_cast<int>(helix::ui::temperature::HeaterStatusState::Ready));
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_bed");
    REQUIRE(chip != nullptr);
    lv_obj_t* check = lv_obj_find_by_name(chip, "status_ready");
    lv_obj_t* flame = lv_obj_find_by_name(chip, "status_heating");
    REQUIRE(check != nullptr);
    REQUIRE(flame != nullptr);
    CHECK_FALSE(lv_obj_has_flag(check, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(flame, LV_OBJ_FLAG_HIDDEN));
    // The duty is in the chip's own text; the status row adds only the glyph,
    // which is all the chip's width reserves for it.
    lv_obj_t* duty = lv_obj_find_by_name(chip, "status_duty");
    REQUIRE(duty != nullptr);
    CHECK(std::string(lv_label_get_text(duty)).empty());
    // A chip with no heater shows no glyph.
    lv_obj_t* fan = lv_obj_find_by_name(root_, "ps_chip_part_fan");
    REQUIRE(fan != nullptr);
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(fan, "status_ready"), LV_OBJ_FLAG_HIDDEN));
    set_int(temps.get_bed_target_subject(), 0);
    set_int(temps.get_bed_temp_subject(), 250);
}

TEST_CASE_METHOD(Landscape800, "the printer tile follows a change of printer image",
                 "[print_status]") {
    auto& images = helix::PrinterImageManager::instance();
    tiles().use_displayed_image();
    settle();
    const std::string before = subject_str("ps_printer_image_src");
    images.set_active_image("shipped:qidi-q2");
    settle();
    CHECK(subject_str("ps_printer_image_src").find("qidi-q2") != std::string::npos);
    images.set_active_image("");
    settle();
    CHECK(subject_str("ps_printer_image_src") == before);
}

TEST_CASE_METHOD(Landscape800, "chips lay out again when the printer tile resizes",
                 "[print_status]") {
    settle();
    const int before = tiles().layout_count();
    lv_obj_set_height(tile_, lv_obj_get_height(tile_) - 30);
    lv_obj_update_layout(root_);
    UpdateQueue::instance().drain();
    process_lvgl(50); // the chip update is coalesced onto a timer
    CHECK(tiles().layout_count() > before);
}

TEST_CASE_METHOD(Landscape800, "speed 104 sets speed_flow tone Warn", "[print_status]") {
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    settle();
    REQUIRE(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Normal));

    set_int(state().motion_state().get_speed_factor_subject(), 104);
    settle();
    CHECK(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Warn));
    CHECK(subject_int("ps_chip_speed_flow_shown") == 1);
    // The factor is Tune's to show; the chip keeps the live speed.
    CHECK(subject_str("ps_chip_speed_flow_text") == "0mm/s");
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

TEST_CASE_METHOD(Portrait272, "the smallest tile keeps the unusual chips and dims a stopped fan",
                 "[print_status]") {
    fans_part_spinning_aux_stopped(*this);

    // Which expendable chip a tight tile drops first is the engine's rule
    // ([callout_layout] "hotend fan drops first when chips do not fit").
    REQUIRE(subject_int("ps_callout_mode") == static_cast<int>(CalloutMode::Pinned));
    CHECK(subject_int("ps_chip_part_fan_shown") == 1);
    CHECK(subject_int("ps_chip_speed_flow_shown") == 1);
    if (subject_int("ps_chip_aux_fan_shown") == 1) {
        CHECK(subject_int("ps_chip_aux_fan_tone") == static_cast<int>(ChipTone::Dim));
    }
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Portrait320, "fan at 0 stays, dimmed, while the tile has room for it",
                 "[print_status]") {
    fans_part_spinning_aux_stopped(*this);

    REQUIRE(subject_int("ps_callout_mode") != static_cast<int>(CalloutMode::ImageOnly));
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

namespace {

bool hidden(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Two steps done of five, the third under way.
std::optional<PreprintChecklistInput> five_steps() {
    PreprintChecklistInput in;
    in.done_in_order = {PrintStartPhase::HOMING, PrintStartPhase::HEATING_BED};
    in.current = PrintStartPhase::HEATING_NOZZLE;
    in.expected_order = {PrintStartPhase::HOMING, PrintStartPhase::HEATING_BED,
                         PrintStartPhase::HEATING_NOZZLE, PrintStartPhase::QGL,
                         PrintStartPhase::BED_MESH};
    return in;
}

} // namespace

TEST_CASE_METHOD(Landscape800, "the checklist shows a row per step and none past them",
                 "[print_status]") {
    tiles().set_checklist_source(five_steps);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    UpdateQueue::instance().drain();
    process_lvgl(900); // the preparing view shows after its debounce
    REQUIRE(subject_int("preparing_visible") == 1);
    REQUIRE(subject_int("ps_step_count") == 5);
    REQUIRE(subject_int("ps_checklist_compact") == 0);

    CHECK_FALSE(hidden(root_, "ps_job_checklist"));
    CHECK_FALSE(hidden(root_, "ps_checklist_list"));
    CHECK(hidden(root_, "ps_checklist_compact"));
    CHECK_FALSE(hidden(root_, "ps_step_row_4"));
    CHECK(hidden(root_, "ps_step_row_5"));
    CHECK(hidden(root_, "ps_step_row_7"));
    // The pre-print strip line replaces the printing one.
    CHECK_FALSE(hidden(root_, "ps_strip_prepare_line"));
    CHECK(hidden(root_, "ps_strip_print_line"));

    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
    REQUIRE(subject_int("preparing_visible") == 0);
    CHECK(hidden(root_, "ps_job_checklist"));
    CHECK_FALSE(hidden(root_, "ps_strip_print_line"));
}

TEST_CASE_METHOD(Portrait272, "a narrow job tile shows the current step compactly",
                 "[print_status]") {
    tiles().set_checklist_source(five_steps);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    UpdateQueue::instance().drain();
    process_lvgl(900);
    REQUIRE(subject_int("ps_checklist_compact") == 1);
    CHECK(hidden(root_, "ps_checklist_list"));
    CHECK_FALSE(hidden(root_, "ps_checklist_compact"));
    lv_obj_t* current = lv_obj_find_by_name(root_, "ps_checklist_current");
    REQUIRE(current != nullptr);
    CHECK(std::string(lv_label_get_text(current)) == subject_str("ps_step_current_name"));
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(Landscape800, "More hides when its menu has nothing to offer", "[print_status]") {
    lv_subject_t* webcam = lv_xml_get_subject(nullptr, "printer_has_webcam");
    lv_subject_t* leds = lv_xml_get_subject(nullptr, "led_controllable");
    lv_subject_t* timelapse = lv_xml_get_subject(nullptr, "printer_has_timelapse");
    REQUIRE(webcam != nullptr);
    REQUIRE(leds != nullptr);
    REQUIRE(timelapse != nullptr);
    set_int(webcam, 0);
    set_int(timelapse, 0);
    set_int(leds, 0);
    CHECK(hidden(root_, "btn_more"));
    set_int(timelapse, 1);
    CHECK_FALSE(hidden(root_, "btn_more"));
    set_int(timelapse, 0);
    set_int(leds, 1);
    CHECK_FALSE(hidden(root_, "btn_more"));
    set_int(leds, 0);
}

TEST_CASE_METHOD(Landscape800, "strip buttons are square beside the progress", "[print_status]") {
    REQUIRE(subject_int("ps_strip_stacked") == 0);
    lv_obj_t* strip = lv_obj_find_by_name(root_, "ps_strip");
    lv_obj_t* progress = lv_obj_find_by_name(root_, "ps_strip_progress");
    lv_obj_t* pause = lv_obj_find_by_name(root_, "btn_pause");
    REQUIRE(strip != nullptr);
    REQUIRE(progress != nullptr);
    REQUIRE(pause != nullptr);
    const int32_t side = theme_manager_get_spacing("ps_strip_button");
    // As tall as the strip, which is a square button's height at the least.
    CHECK(lv_obj_get_height(pause) >= side);
    CHECK(lv_obj_get_height(pause) == lv_obj_get_height(progress));
    CHECK(lv_obj_get_width(pause) <= side);
    CHECK(lv_obj_get_width(pause) >= side - theme_manager_get_spacing("space_sm"));
    // The progress keeps at least 45% of the strip.
    CHECK(lv_obj_get_width(progress) * 100 >= lv_obj_get_content_width(strip) * 40);
    CHECK(lv_obj_get_height(strip) ==
          helix::ui::PrintStatusLayoutFitter::strip_row_height(progress));
}

TEST_CASE_METHOD(Portrait320, "a narrow strip stacks progress over a full row of buttons",
                 "[print_status]") {
    REQUIRE(subject_int("ps_strip_stacked") == 1);
    lv_obj_t* buttons = lv_obj_find_by_name(root_, "ps_strip_buttons");
    lv_obj_t* progress = lv_obj_find_by_name(root_, "ps_strip_progress");
    lv_obj_t* strip = lv_obj_find_by_name(root_, "ps_strip");
    REQUIRE(buttons != nullptr);
    REQUIRE(progress != nullptr);
    CHECK(lv_obj_get_width(buttons) == lv_obj_get_content_width(strip));
    CHECK(lv_obj_get_y(buttons) > lv_obj_get_y(progress));
    CHECK(lv_obj_get_height(buttons) == theme_manager_get_spacing("button_height"));
}

TEST_CASE_METHOD(Landscape800, "a tuned speed turns the speed/flow chip amber", "[print_status]") {
    set_int(state().motion_state().get_speed_factor_subject(), 104);
    settle();
    REQUIRE(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Warn));
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_speed_flow");
    REQUIRE(chip != nullptr);
    const lv_color_t warn = theme_manager_get_color("warning");
    CHECK(lv_color_eq(lv_obj_get_style_border_color(chip, LV_PART_MAIN), warn));
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    settle();
    CHECK_FALSE(lv_color_eq(lv_obj_get_style_border_color(chip, LV_PART_MAIN), warn));
}
