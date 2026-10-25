// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_unit_dryer.h"

#include "json_utils.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace helix::afc {

namespace {

constexpr std::string_view kBambuPrefix = "AFC_BambuAMS ";
constexpr std::string_view kAcePrefix = "AFC_ACE ";
constexpr std::string_view kAce2Prefix = "AFC_ACE2 ";

// Ceilings each unit class falls back to when the unit does not publish its own:
// AFC_BambuAMS _AMS_MODELS (AMS 2 Pro 65, HT 85) and AFC_ACE / AFC_ACE2's
// max_dryer_temperature defaults.
constexpr float kBambuDefaultMaxC = 65.0f;
constexpr float kBambuHtDefaultMaxC = 85.0f;
constexpr float kAceDefaultMaxC = 55.0f;
constexpr float kAce2DefaultMaxC = 70.0f;

// The lowest set-point afc_dryer offers either vendor, and the ACE floor the
// native ACE backend uses.
constexpr float kBambuMinC = 40.0f;
constexpr float kAceMinC = 35.0f;

// AFC_BAMBU_HEATER_START takes TIME up to 65535 min; the AMS's own UI stops at 24 h.
constexpr int kBambuMaxDurationMin = 24 * 60;
constexpr int kAceMaxDurationMin = 12 * 60;

bool starts_with(const std::string& s, std::string_view prefix) {
    return s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

/// A sensor channel, or nullopt. Every one of these sensors reports 0.0 for a
/// channel it has not read yet, so 0 is "no reading", not 0 °C / 0 %.
std::optional<float> reading(const nlohmann::json& v) {
    if (!v.is_number()) {
        return std::nullopt;
    }
    const float f = v.get<float>();
    if (!std::isfinite(f) || f == 0.0f) {
        return std::nullopt;
    }
    return f;
}

/// A whole number of seconds or minutes; 0 is "no cycle on record".
std::optional<int> positive_int(const nlohmann::json& v) {
    if (!v.is_number()) {
        return std::nullopt;
    }
    const double d = v.get<double>();
    if (!std::isfinite(d) || d <= 0.0) {
        return std::nullopt;
    }
    return static_cast<int>(d);
}

std::string string_value(const nlohmann::json& v) {
    return helix::json_util::as_string(v);
}

bool flag(const nlohmann::json& v) {
    return v.is_boolean() && v.get<bool>();
}

/// bridge_online goes stale rather than false; a unit that never published one
/// is assumed reachable.
bool online_flag(const nlohmann::json& v) {
    return !v.is_boolean() || v.get<bool>();
}

float number_or_zero(const nlohmann::json& v) {
    return v.is_number() ? v.get<float>() : 0.0f;
}

std::optional<int> minutes_as_seconds(const nlohmann::json& v) {
    const auto m = positive_int(v);
    return m ? std::optional<int>(*m * 60) : std::nullopt;
}

bool ace_dryer_running(const nlohmann::json& v) {
    // ace_dryer values that mean "not running"; anything else is a live cycle.
    static constexpr std::array<std::string_view, 7> kIdle = {"",     "stop", "stopped", "stopping",
                                                              "idle", "off",  "none"};
    const std::string lowered = helix::text_io::to_lower(string_value(v));
    for (const auto idle : kIdle) {
        if (lowered == idle) {
            return false;
        }
    }
    return true;
}

/// Assign @p key's value through @p parse when the frame carries it. A key that
/// arrives as null clears the value: Bambu publishes None for "not drying".
template <typename T>
bool take(const nlohmann::json& data, const char* key, T& out, T (*parse)(const nlohmann::json&)) {
    const auto it = data.find(key);
    if (it == data.end()) {
        return false;
    }
    out = parse(*it);
    return true;
}

bool apply_bambu(const nlohmann::json& data, UnitDryerState& st) {
    bool any = false;
    any |= take(data, "has_heater", st.has_heater, flag);
    any |= take(data, "ams_model", st.model, string_value);
    any |= take(data, "dry_max_temp", st.max_temp_c, number_or_zero);
    // A frame without bridge_online keeps the last answer.
    any |= take(data, "bridge_online", st.online, online_flag);
    any |= take(data, "drying", st.drying, flag);
    any |= take(data, "temperature", st.temperature_c, reading);
    any |= take(data, "humidity", st.humidity_pct, reading);
    any |= take(data, "dry_target", st.target_c, reading);
    any |= take(data, "dry_remaining", st.remaining_s, positive_int);
    any |= take(data, "dry_minutes", st.duration_s, minutes_as_seconds);
    any |= take(data, "dry_error", st.error, string_value);
    any |= take(data, "dry_blocked", st.blocked, string_value);
    return any;
}

bool apply_ace(const nlohmann::json& data, UnitDryerState& st) {
    bool any = false;
    any |= take(data, "ace_connected", st.online, flag);
    any |= take(data, "ace_dryer", st.drying, ace_dryer_running);
    any |= take(data, "ace_temp", st.temperature_c, reading);
    any |= take(data, "ace_humidity", st.humidity_pct, reading);
    any |= take(data, "ace_dryer_target", st.target_c, reading);
    any |= take(data, "ace_dryer_remain", st.remaining_s, positive_int);
    // V1 echoes the minutes it was asked for; the ACE 2 converts to seconds.
    any |= take(data, "ace_dryer_duration", st.duration_s,
                st.vendor == DryerVendor::Ace2 ? positive_int : minutes_as_seconds);
    return any;
}

int ceil_minutes(int seconds) {
    return (seconds + 59) / 60;
}

} // namespace

DryerVendor dryer_vendor_for_object(const std::string& klipper_key) {
    if (starts_with(klipper_key, kBambuPrefix)) {
        return DryerVendor::Bambu;
    }
    if (starts_with(klipper_key, kAce2Prefix)) {
        return DryerVendor::Ace2;
    }
    if (starts_with(klipper_key, kAcePrefix)) {
        return DryerVendor::Ace;
    }
    return DryerVendor::None;
}

const nlohmann::json& unit_dryer_status_fields() {
    static constexpr const char* kFields[] = {
        // AFC_BambuAMS
        "has_heater", "ams_model", "dry_max_temp", "bridge_online", "drying", "temperature",
        "humidity", "dry_target", "dry_remaining", "dry_minutes", "dry_error", "dry_blocked",
        // AFC_ACE / AFC_ACE2
        "ace_connected", "ace_temp", "ace_humidity", "ace_dryer", "ace_dryer_target",
        "ace_dryer_remain", "ace_dryer_duration"};
    static const nlohmann::json fields = [] {
        nlohmann::json out = nlohmann::json::array();
        for (const char* field : kFields) {
            out.push_back(field);
        }
        return out;
    }();
    return fields;
}

UnitDryerState make_unit_dryer_state(const std::string& klipper_key) {
    UnitDryerState st;
    st.vendor = dryer_vendor_for_object(klipper_key);
    const auto space = klipper_key.find(' ');
    if (space != std::string::npos) {
        st.unit_name = klipper_key.substr(space + 1);
    }
    // Every ACE dries unless max_dryer_temperature: 0 opts it out, which no
    // status field says; a Bambu unit says so itself in has_heater.
    st.has_heater = st.vendor == DryerVendor::Ace || st.vendor == DryerVendor::Ace2;
    return st;
}

bool apply_unit_dryer_status(const nlohmann::json& data, UnitDryerState& state) {
    if (!data.is_object()) {
        return false;
    }
    switch (state.vendor) {
    case DryerVendor::Bambu:
        return apply_bambu(data, state);
    case DryerVendor::Ace:
    case DryerVendor::Ace2:
        return apply_ace(data, state);
    case DryerVendor::None:
        break;
    }
    return false;
}

DryerInfo to_dryer_info(const UnitDryerState& st) {
    DryerInfo dryer;
    if (st.vendor == DryerVendor::None || !st.has_heater) {
        dryer.supported = false;
        return dryer;
    }
    dryer.supported = true;
    dryer.active = st.drying;
    // AFC takes both vendors' commands mid-print.
    dryer.allows_during_print = true;
    dryer.current_temp_c = st.temperature_c.value_or(0.0f);
    dryer.target_temp_c = st.drying ? st.target_c.value_or(0.0f) : 0.0f;
    if (st.drying) {
        dryer.duration_min = st.duration_s ? ceil_minutes(*st.duration_s) : 0;
        dryer.remaining_min = st.remaining_s ? ceil_minutes(*st.remaining_s) : 0;
    }

    const bool bambu = st.vendor == DryerVendor::Bambu;
    float fallback_max = kAceDefaultMaxC;
    if (bambu) {
        fallback_max =
            helix::text_io::to_lower(st.model) == "ht" ? kBambuHtDefaultMaxC : kBambuDefaultMaxC;
    } else if (st.vendor == DryerVendor::Ace2) {
        fallback_max = kAce2DefaultMaxC;
    }
    dryer.max_temp_c = st.max_temp_c > 0.0f ? st.max_temp_c : fallback_max;
    dryer.min_temp_c = std::min(bambu ? kBambuMinC : kAceMinC, dryer.max_temp_c);
    dryer.max_duration_min = bambu ? kBambuMaxDurationMin : kAceMaxDurationMin;
    dryer.duration_step_min = 1;
    dryer.supports_fan_control = false;
    // Both vendors take a new cycle only; a change is stop + start.
    dryer.supports_live_temp = false;
    dryer.supports_live_duration = false;
    return dryer;
}

std::optional<EnvironmentData> to_environment(const UnitDryerState& st) {
    if (st.vendor == DryerVendor::None || (!st.temperature_c && !st.humidity_pct)) {
        return std::nullopt;
    }
    EnvironmentData env;
    env.temperature_c = st.temperature_c.value_or(0.0f);
    if (st.humidity_pct) {
        env.humidity_pct = *st.humidity_pct;
        env.has_humidity = true;
    }
    return env;
}

std::string start_drying_gcode(const UnitDryerState& st, int temp_c, int duration_min) {
    if (st.vendor == DryerVendor::None) {
        return {};
    }
    const bool bambu = st.vendor == DryerVendor::Bambu;
    return fmt::format("{} UNIT={} TEMP={} {}={}", bambu ? "AFC_BAMBU_HEATER_START" : "AFC_ACE_DRY",
                       st.unit_name, temp_c, bambu ? "TIME" : "DURATION", duration_min);
}

std::string stop_drying_gcode(const UnitDryerState& st) {
    if (st.vendor == DryerVendor::None) {
        return {};
    }
    return (st.vendor == DryerVendor::Bambu ? "AFC_BAMBU_HEATER_STOP UNIT="
                                            : "AFC_ACE_DRY_STOP UNIT=") +
           st.unit_name;
}

} // namespace helix::afc
