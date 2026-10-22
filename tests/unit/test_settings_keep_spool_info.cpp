// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_keep_spool_info.cpp
 * @brief ams/keep_spool_info_on_eject setting: default, round-trip, persistence
 *
 * Mirrors the unload_after_print round-trip test
 * (test_ams_backend_afc.cpp "SettingsManager unload_after_print
 * round-trips and persists") — same per-printer ams/* config key family.
 */

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "ams keep_spool_info_on_eject setting", "[settings][ams]") {
    helix::Config* config = helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    const std::string path = config->df() + "ams/keep_spool_info_on_eject";

    SECTION("defaults to on (designed retention)") {
        CHECK(settings.get_ams_keep_spool_info_on_eject());
        REQUIRE(lv_subject_get_int(settings.subject_ams_keep_spool_info_on_eject()) == 1);
    }

    SECTION("set false -> get false -> persisted to per-printer config path") {
        settings.set_ams_keep_spool_info_on_eject(false);
        REQUIRE_FALSE(settings.get_ams_keep_spool_info_on_eject());
        REQUIRE(config->get<bool>(path, true) == false);
        REQUIRE(lv_subject_get_int(settings.subject_ams_keep_spool_info_on_eject()) == 0);
    }

    SECTION("set true after false restores the value") {
        settings.set_ams_keep_spool_info_on_eject(false);
        settings.set_ams_keep_spool_info_on_eject(true);
        REQUIRE(settings.get_ams_keep_spool_info_on_eject());
        REQUIRE(config->get<bool>(path, false) == true);
        REQUIRE(lv_subject_get_int(settings.subject_ams_keep_spool_info_on_eject()) == 1);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ams tag_links_spoolman setting", "[settings][ams][tag_link]") {
    helix::Config* config = helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    const std::string path = config->df() + "ams/tag_links_spoolman";

    SECTION("defaults to on") {
        CHECK(settings.get_ams_tag_links_spoolman());
        REQUIRE(lv_subject_get_int(settings.subject_ams_tag_links_spoolman()) == 1);
    }

    SECTION("off persists to the per-printer config path") {
        settings.set_ams_tag_links_spoolman(false);
        REQUIRE_FALSE(settings.get_ams_tag_links_spoolman());
        REQUIRE(config->get<bool>(path, true) == false);
        settings.set_ams_tag_links_spoolman(true);
        REQUIRE(config->get<bool>(path, false) == true);
    }
}
