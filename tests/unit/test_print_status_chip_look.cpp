// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_chip_look.cpp
 * @brief What the printer tile's chips look like
 */

#include "ui_panel_print_status.h"

#include "../test_helpers/print_status_panel_fixture.h"
#include "callout_layout.h"

#include "../catch_amalgamated.hpp"

using print_status_panel_test::PrintStatusPanelFixture;

TEST_CASE_METHOD(PrintStatusPanelFixture, "Print status chips pinned on the photo cover more of it",
                 "[print_status][print_status_chips]") {
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_speed_flow");
    REQUIRE(chip != nullptr);
    lv_subject_t* mode = lv_xml_get_subject(nullptr, "ps_callout_mode");
    REQUIRE(mode != nullptr);

    lv_subject_set_int(mode, static_cast<int>(helix::CalloutMode::Pinned));
    CHECK(lv_obj_get_style_bg_opa(chip, LV_PART_MAIN) == LV_OPA_COVER * 90 / 100);

    lv_subject_set_int(mode, static_cast<int>(helix::CalloutMode::OneSide));
    CHECK(lv_obj_get_style_bg_opa(chip, LV_PART_MAIN) == LV_OPA_COVER * 65 / 100);
}

TEST_CASE_METHOD(PrintStatusPanelFixture,
                 "Print status speed chip shows the speed alone, no flow readout",
                 "[print_status][print_status_chips]") {
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_speed_flow");
    REQUIRE(chip != nullptr);
    CHECK(lv_obj_find_by_name(chip, "ps_chip_speed_text") == nullptr);
    CHECK(lv_obj_find_by_name(chip, "chip_text") != nullptr);
}
