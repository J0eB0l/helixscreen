// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_printer_utils.h"

#include "../catch_amalgamated.hpp"

static helix::DiscoveredPrinter make_printer(const std::string& name,
                                             const std::string& hostname = "printer.local") {
    return {name, hostname, "192.168.1.100", 9100, ""};
}

TEST_CASE("label_printer_score Brother QL models", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("Brother QL-820NWB")) == 100);
    REQUIRE(helix::label_printer_score(make_printer("Brother QL-810W")) == 100);
    REQUIRE(helix::label_printer_score(make_printer("QL-800")) == 100);
}

TEST_CASE("label_printer_score Brother TD models", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("Brother TD-4550DNWB")) == 100);
}

TEST_CASE("label_printer_score DYMO printers", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("DYMO LabelWriter 450")) == 90);
}

TEST_CASE("label_printer_score Zebra printers", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("Zebra ZD421")) == 80);
}

TEST_CASE("label_printer_score generic label keyword", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("Some Label Maker")) == 70);
}

TEST_CASE("label_printer_score BRW hostname", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("Unknown Printer", "BRW123456.local")) == 50);
}

TEST_CASE("label_printer_score non-label printers score 0", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("HP LaserJet Pro")) == 0);
    REQUIRE(helix::label_printer_score(make_printer("HP OfficeJet 5255")) == 0);
    REQUIRE(helix::label_printer_score(make_printer("Canon PIXMA G7020")) == 0);
    REQUIRE(helix::label_printer_score(make_printer("EPSON ET-4800")) == 0);
    REQUIRE(helix::label_printer_score(make_printer("HP ENVY 6055")) == 0);
}

TEST_CASE("label_printer_score unknown printer", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("Random Device")) == 10);
}

TEST_CASE("label_printer_score case insensitive", "[label]") {
    REQUIRE(helix::label_printer_score(make_printer("brother ql-820nwb")) == 100);
    REQUIRE(helix::label_printer_score(make_printer("BROTHER QL-820NWB")) == 100);
    REQUIRE(helix::label_printer_score(make_printer("hp laserjet")) == 0);
}
