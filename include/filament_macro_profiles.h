// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_macro_profiles.h
 * @brief What a printer's stock filament macros do for themselves.
 *
 * Most load/unload macros expect the hotend to already be at temperature, so
 * every dispatch surface heats first and waits. Some firmwares ship macros that
 * drive the hotend themselves, and preheating in front of one of those makes the
 * user wait twice and can settle on a different temperature than the macro then
 * commands.
 *
 * This module is the ONLY place that names which firmware's macros behave that
 * way. Generic code (the filament panel, the AMS sidebar, the shared dispatch
 * ladder) asks the capability question and never names a firmware.
 *
 * Adding a firmware means adding one row to the table in
 * filament_macro_profiles.cpp - no call site changes.
 *
 * The backend tier has its own answer to the same question,
 * AmsBackend::supports_auto_heat_on_load(). Both feed
 * helix::ui::preheat_skip_reason(), which is where a caller should ask.
 */

#pragma once

#include <string>

namespace helix::filament_macros {

/**
 * @brief Does @p macro_name bring the hotend to temperature on its own?
 *
 * Answered from the macro NAME alone, which is narrower than it looks: the
 * names in the table are firmware-specific spellings that carry no meaning in
 * stock Klipper, so a printer that defines one is running that firmware. A
 * generic name a user could plausibly write themselves (LOAD_FILAMENT,
 * UNLOAD_FILAMENT, M701, M702) is never in the table, because there the name
 * proves nothing about the body - those users are served by the
 * "Allow cold load/unload" setting instead.
 *
 * Klipper macro names are case-insensitive; @p macro_name may be in any case.
 *
 * @param macro_name Resolved macro name, e.g. StandardMacroInfo::get_macro()
 * @return true when a UI preheat in front of this macro would be redundant
 */
[[nodiscard]] bool macro_heats_hotend(const std::string& macro_name);

/**
 * @brief Must the printer be homed before @p macro_name runs?
 *
 * True only for a known macro whose body moves the toolhead with no home of its
 * own. Any macro not in the table answers false and runs as it is: a generic
 * LOAD_FILAMENT may do nothing but extrude, and a G28 in front of it parks the
 * head wherever homing leaves it (#1819). A known macro that carries its own
 * conditional home (QIDI's M604/M603 open with `_CG28`) answers false too.
 *
 * Answered from the name for the same reason macro_heats_hotend() is: only
 * firmware-specific spellings whose bodies are known belong in the table.
 *
 * The backend tier has its own answer, AmsBackend::delegates_homing_to_printer().
 * Both feed helix::ui::needs_prerequisite_home(), which is where a caller should ask.
 *
 * @param macro_name Resolved macro name, e.g. StandardMacroInfo::get_macro()
 * @return true when a caller must home before sending the macro
 */
[[nodiscard]] bool macro_needs_home_first(const std::string& macro_name);

} // namespace helix::filament_macros
