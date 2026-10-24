// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "enclosure_top.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
using namespace helix::enclosure_top;

namespace {

PrinterDiscovery hardware_with_objects(const std::vector<std::string>& names) {
    PrinterDiscovery hw;
    json objects = json::array();
    for (const auto& n : names) {
        objects.push_back(n);
    }
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("enclosure top: detected by its status object", "[enclosure_top]") {
    PrinterDiscovery hw = hardware_with_objects({"purifier", "extruder"});
    CHECK(firmware_reports_enclosure_top(hw));
    // Only the one field is subscribed: the object's fan readings change every tick.
    CHECK(required_status_objects(hw) == json{{"purifier", {"power_detected"}}});
}

TEST_CASE("enclosure top: a printer without the object subscribes nothing", "[enclosure_top]") {
    PrinterDiscovery hw = hardware_with_objects({"extruder", "fan_generic purifier_fan"});
    CHECK_FALSE(firmware_reports_enclosure_top(hw));
    CHECK(required_status_objects(hw).empty());
}

TEST_CASE("enclosure top: the reading follows the firmware's flag", "[enclosure_top]") {
    // The shape a U1 with its top fitted reports.
    json s = {{"purifier",
               {{"power_detected", true},
                {"power_det_value", 2.5},
                {"mode", 0},
                {"inner_fan_rpm", 0.0},
                {"exhaust_fan", json::object()}}}};
    REQUIRE(fitted_from_status(s) == std::optional<bool>(true));
    s["purifier"]["power_detected"] = false;
    REQUIRE(fitted_from_status(s) == std::optional<bool>(false));
}

TEST_CASE("enclosure top: a frame that does not carry the flag says nothing", "[enclosure_top]") {
    // Delta frames carry only what changed; silence is not a removal.
    CHECK_FALSE(fitted_from_status(json::object()).has_value());
    CHECK_FALSE(fitted_from_status(json{{"extruder", {{"temperature", 200.0}}}}).has_value());
    CHECK_FALSE(fitted_from_status(json{{"purifier", {{"inner_fan_rpm", 1200.0}}}}).has_value());
    CHECK_FALSE(fitted_from_status(json::array()).has_value());
}

TEST_CASE("enclosure top: a flag of the wrong type says nothing", "[enclosure_top]") {
    CHECK_FALSE(fitted_from_status(json{{"purifier", {{"power_detected", 1}}}}).has_value());
    CHECK_FALSE(fitted_from_status(json{{"purifier", {{"power_detected", "true"}}}}).has_value());
    CHECK_FALSE(fitted_from_status(json{{"purifier", {{"power_detected", nullptr}}}}).has_value());
    CHECK_FALSE(fitted_from_status(json{{"purifier", true}}).has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "enclosure top: the U1 mock reports its top fitted",
                 "[enclosure_top][mock]") {
    unsetenv("HELIX_MOCK_ENCLOSURE_TOP");
    MoonrakerClientMock u1(MoonrakerClientMock::PrinterType::SNAPMAKER_U1);
    const json status{{"purifier", u1.purifier_status()}};
    CHECK(fitted_from_status(status) == std::optional<bool>(true));

    setenv("HELIX_MOCK_ENCLOSURE_TOP", "0", 1);
    CHECK(fitted_from_status(json{{"purifier", u1.purifier_status()}}) ==
          std::optional<bool>(false));
    unsetenv("HELIX_MOCK_ENCLOSURE_TOP");

    MoonrakerClientMock voron(MoonrakerClientMock::PrinterType::VORON_24);
    CHECK(voron.purifier_status().empty());
}
