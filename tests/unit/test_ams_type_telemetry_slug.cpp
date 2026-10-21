// SPDX-License-Identifier: GPL-3.0-or-later

// ams.type in the telemetry hardware profile is a published schema the
// dashboard groups by: an existing value must never change.

#include "ams_types.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ams_type_telemetry_slug;
using helix::AmsType;

TEST_CASE("Telemetry ams.type slugs are stable", "[telemetry][ams]") {
    CHECK(std::string(ams_type_telemetry_slug(AmsType::NONE)) == "unknown");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::HAPPY_HARE)) == "happy_hare");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::AFC)) == "afc");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::ACE)) == "ace");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::TOOL_CHANGER)) == "tool_changer");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::AD5X_IFS)) == "ifs");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::CFS)) == "cfs");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::QIDI_BOX)) == "qidi_box");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::OPENAMS)) == "openams");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::SNAPMAKER)) == "snapmaker");
    CHECK(std::string(ams_type_telemetry_slug(AmsType::INDX)) == "indx");
}
