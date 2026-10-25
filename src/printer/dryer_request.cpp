// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dryer_request.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>

#include <string>

namespace helix {

std::optional<AmsError> check_dryer_request(const DryerInfo& dryer, float temp_c,
                                            int duration_min) {
    if (temp_c < dryer.min_temp_c || temp_c > dryer.max_temp_c) {
        return AmsError(
            AmsResult::COMMAND_FAILED, "Temperature out of range: " + std::to_string(temp_c),
            lv_tr("Invalid temperature"),
            fmt::format(lv_tr("Set temperature between {}°C and {}°C"),
                        static_cast<int>(dryer.min_temp_c), static_cast<int>(dryer.max_temp_c)));
    }
    if (duration_min <= 0 || duration_min > dryer.max_duration_min) {
        return AmsError(
            AmsResult::COMMAND_FAILED, "Duration out of range: " + std::to_string(duration_min),
            lv_tr("Invalid duration"),
            fmt::format(lv_tr("Set duration between 1 and {} minutes"), dryer.max_duration_min));
    }
    return std::nullopt;
}

std::pair<float, int> dryer_restart_request(const DryerInfo& dryer, float temp_c,
                                            int duration_min) {
    const float temp = temp_c < 0 ? dryer.target_temp_c : temp_c;
    // A temperature change restarts the cycle, so it must restart on what is
    // left. Reusing the session length would silently hand back time served.
    const int duration = duration_min >= 0
                             ? duration_min
                             : (dryer.remaining_min > 0 ? dryer.remaining_min : dryer.duration_min);
    return {temp, duration};
}

} // namespace helix
