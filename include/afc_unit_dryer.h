// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <optional>
#include <string>

#include "hv/json.hpp"

/**
 * @file afc_unit_dryer.h
 * @brief Drying heaters on AFC units: Bambu AMS (BridgeBox) and AFC-driven ACE.
 *
 * AFC itself has no dryer model. Each unit class that carries a heater publishes
 * its own fields on its own unit object and takes its own G-code:
 *
 * - `AFC_BambuAMS` (AMS 2 Pro, AMS HT on a BridgeBox):
 *   `AFC_BAMBU_HEATER_START UNIT= TEMP= TIME=` / `AFC_BAMBU_HEATER_STOP UNIT=`
 * - `AFC_ACE` (ACE Pro) and `AFC_ACE2` (ACE 2 Pro):
 *   `ACE_DRY UNIT= TEMP= DURATION=` / `ACE_DRY_STOP UNIT=`
 *
 * Both commands take minutes. A Bambu unit reports the time left in seconds and the
 * commanded length in minutes; an ACE reports the time left in seconds, and the
 * length in minutes on a V1 but seconds on an ACE 2 (measured on both, see
 * afc_dryer.py in the AFC BridgeBox fork).
 *
 * Moonraker forwards only changed keys, so the state here is merged frame by frame
 * and a key that arrives as null clears its value.
 */

namespace helix::afc {

enum class DryerVendor { None, Bambu, Ace, Ace2 };

/// Which dryer vendor an AFC unit object belongs to, from its Klipper key
/// (e.g. "AFC_BambuAMS AMS_1"). None for any unit class without a heater.
[[nodiscard]] DryerVendor dryer_vendor_for_object(const std::string& klipper_key);

/// The unit-object fields a dryer needs, for the status subscription.
[[nodiscard]] const nlohmann::json& unit_dryer_status_fields();

struct UnitDryerState {
    DryerVendor vendor = DryerVendor::None;
    std::string unit_name; ///< AFC unit name, the UNIT= argument

    /// Bambu: the unit's has_heater (an AMS 1, AMS Lite or an unconfirmed `boxed`
    /// unit has none). ACE: true unless the configured ceiling is 0.
    bool has_heater = false;
    std::string model;       ///< Bambu ams_model ("ams2", "ht", ...)
    float max_temp_c = 0.0f; ///< 0 = not reported, use the vendor default
    bool online = true;
    bool drying = false;
    std::optional<float> temperature_c;
    std::optional<float> humidity_pct;
    std::optional<float> target_c;
    std::optional<int> remaining_s;
    std::optional<int> duration_s;
    std::string error;   ///< The unit declined the last cycle
    std::string blocked; ///< Why a start would be refused now
};

/// Seed a state for a unit object. Vendor defaults (an ACE has a heater unless
/// told otherwise) are set here so a frame that omits a key keeps them.
[[nodiscard]] UnitDryerState make_unit_dryer_state(const std::string& klipper_key);

/// Merge one status frame for the unit into @p state. Returns true when any
/// dryer field was present.
bool apply_unit_dryer_status(const nlohmann::json& data, UnitDryerState& state);

/// The DryerInfo surfaces read. supported=false for a unit with no heater.
[[nodiscard]] DryerInfo to_dryer_info(const UnitDryerState& state);

/// The unit's own temperature/humidity reading, when it has one.
[[nodiscard]] std::optional<EnvironmentData> to_environment(const UnitDryerState& state);

[[nodiscard]] std::string start_drying_gcode(const UnitDryerState& state, int temp_c,
                                             int duration_min);
[[nodiscard]] std::string stop_drying_gcode(const UnitDryerState& state);

} // namespace helix::afc
