// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_error.h"
#include "ams_types.h"

#include <optional>
#include <utility>

/**
 * @file dryer_request.h
 * @brief Vendor-independent checks on a drying request against a DryerInfo.
 *
 * Shared by every backend whose dryer takes a temperature and a duration and
 * has no live retarget (ACE, multiACE, and the AFC unit heaters), so the range
 * refusal and the stop-and-restart arithmetic cannot drift between them. Pure:
 * no I/O, no locks.
 */
namespace helix {

/// The refusal for a drying request outside @p dryer's range, or nullopt when
/// @p temp_c and @p duration_min are both in range.
[[nodiscard]] std::optional<AmsError> check_dryer_request(const DryerInfo& dryer, float temp_c,
                                                          int duration_min);

/// The (temperature, duration) a stop-and-restart adjustment of a running
/// cycle starts with: a negative @p temp_c keeps the cycle's target, a negative
/// @p duration_min keeps what is left of it (the session length when the dryer
/// reports no remaining time).
[[nodiscard]] std::pair<float, int> dryer_restart_request(const DryerInfo& dryer, float temp_c,
                                                          int duration_min);

} // namespace helix
