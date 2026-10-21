// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_mapper.h"

#include <functional>
#include <lvgl.h>
#include <vector>

namespace helix {
class AmsBackend;
} // namespace helix

namespace helix::ui {

/**
 * @brief The slots to offer when the user must say which lane is in the toolhead.
 *
 * @param candidates 0-based slot indices from AmsBackend::unload_lane_question()
 * @param slots      every slot the primary backend publishes
 * @return the entries of @p slots on the primary backend whose index is a
 *         candidate, in candidate order
 */
std::vector<helix::AvailableSlot>
unload_lane_choices(const std::vector<int>& candidates,
                    const std::vector<helix::AvailableSlot>& slots);

/**
 * @brief Ask which lane is in the toolhead before a slotless unload, when the
 *        backend cannot name it.
 *
 * One candidate lane asks the user to confirm it in a dialog; several show the
 * lane picker anchored to @p anchor. @p on_chosen receives the chosen 0-based
 * slot, or kClearToolheadSlot for "Not sure" when the backend offers it.
 * Dismissing either cancels the unload.
 *
 * @return true when the picker took over the unload, false (showing nothing)
 *         when the backend resolves the lane itself
 */
bool choose_unload_lane(helix::AmsBackend* backend, lv_obj_t* anchor,
                        std::function<void(int)> on_chosen);

} // namespace helix::ui
