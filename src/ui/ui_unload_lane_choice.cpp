// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unload_lane_choice.h"

#include "ui_filament_slot_picker.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

std::vector<helix::AvailableSlot>
unload_lane_choices(const std::vector<int>& candidates,
                    const std::vector<helix::AvailableSlot>& slots) {
    std::vector<helix::AvailableSlot> choices;
    for (int slot : candidates) {
        auto it = std::find_if(slots.begin(), slots.end(), [slot](const helix::AvailableSlot& s) {
            return s.backend_index == 0 && s.slot_index == slot;
        });
        if (it != slots.end()) {
            choices.push_back(*it);
        }
    }
    return choices;
}

bool choose_unload_lane(helix::AmsBackend* backend, lv_obj_t* anchor,
                        std::function<void(int)> on_chosen) {
    if (!backend || !anchor) {
        return false;
    }
    const helix::UnloadLaneQuestion question = backend->unload_lane_question();
    if (question.lanes.empty()) {
        return false;
    }
    const std::vector<helix::AvailableSlot> choices =
        unload_lane_choices(question.lanes, AmsState::instance().collect_available_slots());
    if (choices.empty()) {
        spdlog::warn("[UnloadLane] {} lane candidate(s) but none published as a slot",
                     question.lanes.size());
        return false;
    }

    if (choices.size() == 1) {
        // One lane holds filament, which makes it likely but not certain:
        // the user confirms it, and "Not sure" clears the toolhead instead.
        const int lane = choices.front().slot_index;
        const std::string message =
            question.offers_clear_toolhead
                ? fmt::format(fmt::runtime(lv_tr("Probably {}, the only lane holding filament. Not "
                                                 "sure clears the extruder without retracting "
                                                 "any lane.")),
                              helix::FilamentMapper::format_slot_label(choices.front()))
                : fmt::format(fmt::runtime(lv_tr("Probably {}, the only lane holding filament.")),
                              helix::FilamentMapper::format_slot_label(choices.front()));
        spdlog::info("[UnloadLane] Asking to confirm slot {} as the lane in the toolhead", lane);
        ConfirmOptions options;
        if (question.offers_clear_toolhead) {
            options.cancel_text = lv_tr("Not sure");
            options.on_cancel = [on_chosen]() {
                if (on_chosen) {
                    on_chosen(helix::kClearToolheadSlot);
                }
            };
        }
        options.on_dismiss = []() { spdlog::info("[UnloadLane] Lane question dismissed"); };
        modal_confirm(
            lv_tr("Which lane is in the toolhead?"), message.c_str(), ModalSeverity::Info,
            lv_tr("Unload"),
            [on_chosen, lane]() {
                if (on_chosen) {
                    on_chosen(lane);
                }
            },
            options);
        return true;
    }

    // One question at a time, so one picker serves every Unload button. Never
    // destroyed: its destructor deletes LVGL objects, and a static's destructor
    // runs after lv_deinit(), which already freed the screen holding them.
    static FilamentSlotPicker* picker = new FilamentSlotPicker();
    spdlog::info("[UnloadLane] Asking which of {} lane(s) is in the toolhead", choices.size());
    FilamentSlotPicker::Options options;
    options.title = lv_tr("Which lane is in the toolhead?");
    options.highlight_trigger = false;
    if (question.offers_clear_toolhead) {
        options.extra_choice = lv_tr("Not sure");
    }
    picker->show(
        lv_screen_active(), anchor, choices, FilamentSlotPicker::Selection{},
        [on_chosen = std::move(on_chosen)](const FilamentSlotPicker::Selection& sel) {
            if (!on_chosen) {
                return;
            }
            if (sel.is_extra) {
                on_chosen(helix::kClearToolheadSlot);
            } else if (sel.slot_index >= 0) {
                on_chosen(sel.slot_index);
            }
        },
        options);
    return true;
}

} // namespace helix::ui
