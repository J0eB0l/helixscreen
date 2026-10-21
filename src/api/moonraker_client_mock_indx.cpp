// SPDX-License-Identifier: GPL-3.0-or-later

// Bondtech INDX mock hardware (HELIX_MOCK_AMS=indx): the objects, configfile
// sections and macro state Bondtech's upstream macros publish (BondtechAB/INDX
// macros/*.cfg), so real discovery builds the production AmsBackendIndx under
// --test. Tool changes walk park -> pick up over a few notifications the way
// save_variables changes on a real printer, and the heater follows INDX's own
// M104/M109 rule: nothing heats with no tool on the head or a no-hotend tool.

#include "env_knobs.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace {

/// Notifications each half of a change takes (one notification is ~1 s), so
/// the park and the pickup reach the screen as separate frames.
constexpr int kIndxPhaseNotifications = 1;

/// The value after "KEY=" on a gcode line, empty when absent.
std::string param(const std::string& gcode, const char* key) {
    const std::string needle = std::string(key) + "=";
    const size_t at = gcode.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + needle.size();
    const size_t end = gcode.find_first_of(" \t", start);
    return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/// The number after an M-code's letter parameter ("M104 S210" -> "210").
std::string mcode_param(const std::string& gcode, char letter) {
    for (size_t at = gcode.find(' '); at != std::string::npos; at = gcode.find(' ', at + 1)) {
        if (at + 1 < gcode.size() &&
            std::toupper(static_cast<unsigned char>(gcode[at + 1])) == letter) {
            const size_t start = at + 2;
            const size_t end = gcode.find_first_of(" \t", start);
            return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }
    return {};
}

/// "T<n>" -> n, else -1.
int tool_command(const std::string& cmd) {
    if (cmd.size() < 2 || cmd[0] != 'T' ||
        !std::all_of(cmd.begin() + 1, cmd.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return -1;
    }
    return helix::text_io::parse_leading<int>(cmd.substr(1)).value_or(-1);
}

} // namespace

bool MoonrakerClientMock::is_mock_indx() const {
    return helix::mock::effective_mock_ams(std::getenv("HELIX_MOCK_AMS"),
                                           std::getenv("HELIX_MOCK_PRINTER")) == "indx";
}

void MoonrakerClientMock::read_indx_knobs() {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    indx_ = IndxMockState{};
    indx_.tool_count =
        static_cast<int>(helix::env_float("HELIX_MOCK_INDX_TOOLS", 6.0f, 2.0f, 10.0f));
    const char* fault = std::getenv("HELIX_MOCK_INDX_FAULT");
    const std::string f = fault ? fault : "";
    indx_.seat_suspect = f == "seat_suspect" ? 1 : -1;
    indx_.docks_calibrated = f != "docks_uncalibrated";
    indx_.unrecognized = f == "unrecognized";
    indx_.kalico = f == "kalico";
    indx_.spool_macros = helix::env_flag("HELIX_MOCK_INDX_SPOOL_MACROS");
    indx_.no_heat_tools = {indx_.tool_count - 1};
    indx_.spool_ids.clear();
    for (int i = 0; i < indx_.tool_count; ++i) {
        indx_.spool_ids.push_back(i + 1);
    }
    spdlog::info("[MoonrakerClientMock] INDX mock: {} tools, fault={}, spool macros={}",
                 indx_.tool_count, f.empty() ? "none" : f, indx_.spool_macros);
}

void MoonrakerClientMock::append_indx_objects(nlohmann::json& objects) const {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    if (!indx_.kalico) {
        objects.push_back("indx");
    }
    objects.push_back("save_variables");
    objects.push_back("load_cell_probe");
    for (const char* macro :
         {"TOOL_POSITIONS", "_TOOLCHANGE_VARS", "PARK_TOOL", "LOAD_FILAMENT", "SET_TOOL_FILAMENT",
          "TOOL_STATUS", "MANUAL_TOOL_REMOVE", "MANUAL_TOOL_SEAT"}) {
        objects.push_back(std::string("gcode_macro ") + macro);
    }
    if (!indx_.unrecognized) {
        objects.push_back("gcode_macro CHANGE_TOOL");
    }
    for (int i = 0; i < indx_.tool_count; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
}

nlohmann::json MoonrakerClientMock::indx_config_sections() const {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    nlohmann::json config = nlohmann::json::object();
    config["indx"] = {{"mcu", "indxmcu"}};
    config["gcode_macro TOOL_POSITIONS"] = {
        {"variable_tool_count", std::to_string(indx_.tool_count)}};
    for (int i = 0; i < indx_.tool_count; ++i) {
        std::string gcode = "CHANGE_TOOL TOOL=" + std::to_string(i);
        if (indx_.spool_macros) {
            gcode = "SET_ACTIVE_SPOOL ID={printer['gcode_macro T" + std::to_string(i) +
                    "'].spool_id}\n" + gcode;
        }
        config["gcode_macro T" + std::to_string(i)] = {{"gcode", gcode}};
    }
    config["gcode_macro LOAD_FILAMENT"] = {
        {"gcode", "{% set t = params.TOOL|int %}\n{% set type = params.TYPE|default('') %}\n"
                  "{% set temp = params.TEMP|default(0) %}\nCHANGE_TOOL TOOL={t}"}};
    return config;
}

nlohmann::json MoonrakerClientMock::indx_status_json() const {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    nlohmann::json status = nlohmann::json::object();
    status["save_variables"] = {{"variables",
                                 {{"active_tool", indx_.active_tool},
                                  {"toolchange_count", indx_.toolchange_count},
                                  {"seat_suspect", indx_.seat_suspect},
                                  {"toolhead_state", indx_.toolhead_state},
                                  {"global_z_offset", 0.0}}}};
    nlohmann::json tools = nlohmann::json::array();
    for (int i = 0; i < indx_.tool_count; ++i) {
        tools.push_back({{"x", 45.0 * i}, {"dock_y", -26.0}});
    }
    status["gcode_macro TOOL_POSITIONS"] = {{"tool_count", indx_.tool_count},
                                            {"tools", tools},
                                            {"no_heat_tools", indx_.no_heat_tools},
                                            {"docks_calibrated", indx_.docks_calibrated}};
    status["gcode_macro _TOOLCHANGE_VARS"] = {{"toolhead_state", indx_.toolhead_state},
                                              {"mode", "normal"}};
    status["gcode_macro LOAD_FILAMENT"] = {
        {"filament_presets",
         {{"PLA", {{"temp", 210}, {"density", 1.24}, {"hc", 1.80}}},
          {"PETG", {{"temp", 240}, {"density", 1.27}, {"hc", 1.30}}},
          {"ABS", {{"temp", 245}, {"density", 1.04}, {"hc", 1.40}}},
          {"ASA", {{"temp", 250}, {"density", 1.07}, {"hc", 1.40}}},
          {"PA", {{"temp", 260}, {"density", 1.14}, {"hc", 1.70}}},
          {"PC", {{"temp", 270}, {"density", 1.20}, {"hc", 1.20}}},
          {"TPU", {{"temp", 230}, {"density", 1.21}, {"hc", 1.80}, {"speed", 2.0}}}}}};
    for (int i = 0; i < indx_.tool_count; ++i) {
        nlohmann::json t = {{"active", i == indx_.active_tool}};
        if (indx_.spool_macros) {
            t["spool_id"] = indx_.spool_ids[static_cast<size_t>(i)];
        }
        status["gcode_macro T" + std::to_string(i)] = t;
    }
    return status;
}

void MoonrakerClientMock::advance_indx_swap() {
    double heat_to = -1.0;
    {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        if (indx_.phase == 0 || --indx_.phase_ticks > 0) {
            return;
        }
        if (indx_.phase == 1) {
            // Parked: the head is empty and the latch is open.
            indx_.active_tool = -1;
            indx_.toolhead_state = 2;
            if (indx_.target_tool < 0) {
                indx_.phase = 0;
                spdlog::info("[MoonrakerClientMock] INDX park complete");
                return;
            }
            indx_.phase = 2;
            indx_.phase_ticks = kIndxPhaseNotifications;
            return;
        }
        // Picked up, locked and recorded; the macro then heats the new tool.
        indx_.active_tool = indx_.target_tool;
        indx_.target_tool = -1;
        indx_.toolhead_state = 0;
        indx_.phase = 0;
        ++indx_.toolchange_count;
        const bool no_heat = std::count(indx_.no_heat_tools.begin(), indx_.no_heat_tools.end(),
                                        indx_.active_tool) > 0;
        if (!no_heat && indx_.resume_target > 0) {
            heat_to = indx_.resume_target;
        }
        spdlog::info("[MoonrakerClientMock] INDX change complete: T{} on the head",
                     indx_.active_tool);
    }
    if (heat_to > 0) {
        set_extruder_target(heat_to);
    }
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_indx(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    auto refuse = [this](const std::string& message) {
        spdlog::warn("[MoonrakerClientMock] INDX gcode rejected: {}", message);
        dispatch_gcode_response("!! " + message);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = message;
        return 1;
    };

    // INDX's own M104/M109 heat only a mounted tool that has a hotend, and a
    // direct SET_HEATER_TEMPERATURE on the coil is refused the same way here,
    // so a missing gate in the screen shows as a target that never moves.
    const bool heat_cmd = cmd == "M104" || cmd == "M109" ||
                          (cmd == "SET_HEATER_TEMPERATURE" && param(gcode, "HEATER") == "extruder");
    if (heat_cmd) {
        const std::string value =
            cmd == "SET_HEATER_TEMPERATURE" ? param(gcode, "TARGET") : mcode_param(gcode, 'S');
        const double target = helix::text_io::parse_leading<double>(value).value_or(0.0);
        std::lock_guard<std::mutex> lock(indx_mutex_);
        const bool no_heat = std::count(indx_.no_heat_tools.begin(), indx_.no_heat_tools.end(),
                                        indx_.active_tool) > 0;
        if (target > 0 && (indx_.active_tool < 0 || no_heat)) {
            spdlog::info("[MoonrakerClientMock] INDX: {} {:.0f}: {}, not heating", cmd, target,
                         indx_.active_tool < 0 ? "no tool on the head" : "tool has no hotend");
            return 0;
        }
        return std::nullopt;
    }

    int tool = tool_command(cmd);
    if (cmd == "CHANGE_TOOL" || cmd == "LOAD_FILAMENT") {
        tool = helix::text_io::parse_leading<int>(param(gcode, "TOOL")).value_or(-1);
        if (tool < 0) {
            return refuse(cmd + " requires TOOL=<n>");
        }
    }
    const bool park = cmd == "PARK_TOOL";
    if (cmd == "MANUAL_TOOL_REMOVE" || cmd == "MANUAL_TOOL_SEAT") {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        indx_.seat_suspect = -1;
        if (cmd == "MANUAL_TOOL_REMOVE") {
            indx_.active_tool = -1;
        } else if (auto t = helix::text_io::parse_leading<int>(param(gcode, "TOOL"))) {
            indx_.active_tool = *t;
        }
        return 0;
    }
    if (tool < 0 && !park) {
        return std::nullopt;
    }

    double target = extruder_target_.load();
    if (cmd == "LOAD_FILAMENT") {
        // LOAD_FILAMENT heats to TEMP=, else its TYPE preset's load temperature.
        static const std::map<std::string, double> kPresetTemps = {
            {"PLA", 210}, {"PETG", 240}, {"ABS", 245}, {"ASA", 250},
            {"PA", 260},  {"PC", 270},   {"TPU", 230}};
        const auto preset = kPresetTemps.find(helix::text_io::to_upper(param(gcode, "TYPE")));
        target = helix::text_io::parse_leading<double>(param(gcode, "TEMP"))
                     .value_or(preset != kPresetTemps.end() ? preset->second : 0.0);
    }
    {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        if (cmd == "CHANGE_TOOL" && indx_.unrecognized) {
            return std::nullopt;
        }
        if (indx_.seat_suspect >= 0) {
            return refuse("T" + std::to_string(indx_.seat_suspect) +
                          " may be half seated on the head");
        }
        if (!indx_.docks_calibrated) {
            return refuse("INDX: the dock positions are not calibrated yet");
        }
        if (tool >= indx_.tool_count) {
            return refuse("T" + std::to_string(tool) + " is not configured");
        }
        if (park && indx_.active_tool < 0) {
            return 0;
        }
        if (!park && tool == indx_.active_tool && cmd != "LOAD_FILAMENT") {
            return 0;
        }
        indx_.target_tool = park ? -1 : tool;
        indx_.resume_target = target;
        indx_.phase = indx_.active_tool >= 0 ? 1 : 2;
        indx_.phase_ticks = kIndxPhaseNotifications;
    }
    // The coil goes off before the head drives into a dock.
    set_extruder_target(0.0);
    if (cmd == "LOAD_FILAMENT") {
        dispatch_gcode_response("LOAD_FILAMENT: T" + std::to_string(tool) + " loading");
    }
    spdlog::info("[MoonrakerClientMock] INDX {}: {}", cmd, park ? "parking" : "changing tool");
    return 0;
}
