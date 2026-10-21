// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_indx.h"

#include "ams_error.h"
#include "ams_fault_event.h"
#include "app_constants.h"
#include "display_numbering.h"
#include "filament_op_slot_resolver.h"
#include "i_moonraker_api.h"
#include "klipper_extruder_naming.h"
#include "lane_source_store.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "toolchanger_addon.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix {

AmsBackendIndx::AmsBackendIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsBackendToolChanger(api, client) {
    system_info_.type = AmsType::INDX;
    system_info_.type_name = "INDX"; // i18n: do not translate - product name
}

nlohmann::json AmsBackendIndx::required_status_objects(const helix::PrinterDiscovery& hw) {
    return indx::required_status_objects(hw);
}

void AmsBackendIndx::set_discovery(const helix::PrinterDiscovery& discovery) {
    contract_ = indx::check_contract(discovery);
    macros_set_active_spool_ = indx::macros_set_active_spool(discovery);
    change_gcodes_.clear();
    for (int i = 0; i < indx::kMaxTools; ++i) {
        change_gcodes_.push_back(indx::change_tool_gcode(discovery, i));
    }

    // The swap is INDX's own T<n>/CHANGE_TOOL, never SELECT_TOOL: marking the
    // commands present gives the base the G-code-rewrite remap and park.
    toolchanger_addon::ToolCommands commands;
    commands.present = true;
    commands.provider_name = "INDX"; // i18n: do not translate - product name
    commands.select_prefix = "T";
    commands.unselect = indx::park_gcode();
    set_tool_commands(std::move(commands));

    if (!contract_.recognized) {
        spdlog::warn("{} INDX macros not recognized (missing: {}); nothing will be sent",
                     backend_log_tag(), indx::missing_text(contract_));
        size_tools(0);
        return;
    }
    spdlog::info("{} INDX macros recognized: {} tools from config, load={}, own spool macros={}",
                 backend_log_tag(), indx::config_tool_count(), contract_.has_load_filament,
                 macros_set_active_spool_);
    size_tools(indx::config_tool_count());
}

void AmsBackendIndx::size_tools(int count) {
    count = std::clamp(count, 0, indx::kMaxTools);
    if (count == sized_tool_count_) {
        return;
    }
    sized_tool_count_ = count;
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        names.push_back(helix::ui::tool_label(i));
    }
    set_discovered_tools(std::move(names));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!system_info_.units.empty()) {
        system_info_.units[0].name = "INDX"; // i18n: do not translate - product name
        // Every tool prints through the Smart Head's one extruder.
        for (auto& slot : system_info_.units[0].slots) {
            slot.extruder_name = helix::extruder_name_for_tool(0);
        }
    }
}

AmsError AmsBackendIndx::additional_start_checks() {
    // Starts with no tools on purpose: an unrecognized macro set renders its
    // error, and a config without variable_tool_count sizes on the first frame.
    return AmsErrorHelper::success();
}

void AmsBackendIndx::handle_status(const nlohmann::json& status) {
    auto update = indx::read_status(status);
    if (!update) {
        return;
    }

    std::optional<int> resize;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const indx::Reading before = state_;
        indx::merge(state_, *update);
        changed = !(state_ == before);
        if (contract_.recognized && update->tool_count &&
            *update->tool_count != sized_tool_count_) {
            resize = *update->tool_count;
        }
    }
    if (!changed && !resize) {
        // Moonraker resends unchanged macro state; nothing on screen moves.
        return;
    }
    if (resize) {
        size_tools(*resize);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.active_tool) {
            const int tool = *state_.active_tool;
            if (tool < -1 || tool >= sized_tool_count_) {
                // A tool the printer has not configured: not a reading to act on.
                if (!warned_out_of_range_) {
                    spdlog::warn("{} active_tool {} is outside the {} configured tools; "
                                 "treating the mounted tool as unknown",
                                 backend_log_tag(), tool, sized_tool_count_);
                    warned_out_of_range_ = true;
                }
            } else {
                warned_out_of_range_ = false;
                toolchanger_addon::ToolReading reading;
                reading.current_tool = tool;
                apply_tool_sensor_locked(reading);
            }
        }

        // T<n>.spool_id is a spool the printer's own macros track per tool.
        for (const auto& [tool, id] : update->tool_spool_ids) {
            if (tool < 0 || tool >= system_info_.total_slots) {
                continue;
            }
            ams::Observation stated(ams::ObservationSource::VendorCache);
            stated.spoolman_id = id > 0 ? id : 0;
            ams::ingest(lane_id(tool), stated);
        }

        if (!system_info_.units.empty()) {
            auto& slots = system_info_.units[0].slots;
            for (size_t i = 0; i < slots.size(); ++i) {
                apply_resolved_lane(slots[i], static_cast<int>(i));
            }
        }
    }
    emit_event(EVENT_STATE_CHANGED);
}

std::string AmsBackendIndx::nozzle_heat_blocked_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active_tool || *state_.active_tool >= sized_tool_count_) {
        // No usable reading: the macros have not said what is on the head, or
        // named a tool this printer does not have.
        return {};
    }
    return indx::heat_blocked_reason(*state_.active_tool,
                                     state_.no_heat_tools.value_or(std::set<int>{}));
}

bool AmsBackendIndx::print_paused() const {
    return api_ && api_->printer_state().print_state().get_print_lifecycle() == PrintState::Paused;
}

namespace {

/// Why a swap is refused while a print is paused: the print resumes with
/// whatever nozzle is mounted.
std::string paused_swap_reason() {
    return lv_tr("Resuming would print with a different nozzle. Change tools after the print.");
}

} // namespace

std::string AmsBackendIndx::motion_blocked_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return indx::motion_blocked_reason(contract_, state_);
}

std::optional<helix::ErrorEvent> AmsBackendIndx::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!contract_.recognized) {
        return helix::make_ams_fault_event(
            helix::ErrorSource::TOOLCHANGER, lv_tr("INDX macros not recognized"),
            fmt::format(fmt::runtime(lv_tr("This printer's INDX macros are missing: {}")),
                        indx::missing_text(contract_)),
            {});
    }
    if (state_.seat_suspect.value_or(-1) >= 0) {
        // The macro's own recovery is hands-on (MANUAL_TOOL_REMOVE opens the
        // latch), so the dialog explains and offers no button.
        return helix::make_ams_fault_event(helix::ErrorSource::TOOLCHANGER,
                                           lv_tr("Tool may be half seated"),
                                           indx::motion_blocked_reason(contract_, state_), {});
    }
    return std::nullopt;
}

HeadFilamentOps AmsBackendIndx::head_filament_ops(int head) const {
    HeadFilamentOps ops;
    ops.unload = FilamentOpSupport::Manual;
    ops.unload_reason = lv_tr("Pick up the tool, heat it, and pull the filament out");
    bool no_heat = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        no_heat = state_.no_heat_tools && state_.no_heat_tools->count(head) > 0;
    }
    if (no_heat) {
        const std::string reason = indx::heat_blocked_reason(head, {head});
        ops.load = FilamentOpSupport::Unavailable;
        ops.load_reason = reason;
        ops.unload = FilamentOpSupport::Unavailable;
        ops.unload_reason = reason;
    } else if (!contract_.has_load_filament) {
        ops.load = FilamentOpSupport::Manual;
        ops.load_reason = lv_tr("This printer has no INDX LOAD_FILAMENT macro");
    }
    return ops;
}

AmsError AmsBackendIndx::do_change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsError slot_valid = validate_slot_index_locked(tool_number);
        if (!slot_valid) {
            return slot_valid;
        }
        const std::string blocked = indx::motion_blocked_reason(contract_, state_);
        if (!blocked.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "INDX tool change refused", blocked, "");
        }
        if (system_info_.current_slot == tool_number) {
            return AmsErrorHelper::success();
        }
    }
    if (print_paused()) {
        return AmsError(AmsResult::WRONG_STATE, "INDX tool change refused while paused",
                        paused_swap_reason(), "");
    }
    if (tool_number < 0 || tool_number >= static_cast<int>(change_gcodes_.size())) {
        return AmsErrorHelper::not_supported("INDX tool " + std::to_string(tool_number));
    }
    std::string cmd = change_gcodes_[static_cast<size_t>(tool_number)];
    spdlog::info("{} Changing to tool {}: {}", backend_log_tag(), tool_number, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::SELECTING);
}

AmsError AmsBackendIndx::do_park_active_tool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string blocked = indx::motion_blocked_reason(contract_, state_);
        if (!blocked.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "INDX park refused", blocked, "");
        }
        if (system_info_.current_tool < 0) {
            return AmsErrorHelper::not_loaded();
        }
    }
    if (print_paused()) {
        return AmsError(AmsResult::WRONG_STATE, "INDX park refused while paused",
                        paused_swap_reason(), "");
    }
    spdlog::info("{} Parking the current tool: {}", backend_log_tag(), indx::park_gcode());
    return dispatch_operation(indx::park_gcode(), AmsAction::UNLOADING);
}

AmsError AmsBackendIndx::do_load_filament(int slot_index) {
    const HeadFilamentOps ops = head_filament_ops(slot_index);
    SlotInfo slot;
    std::map<std::string, indx::FilamentPreset> presets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsError slot_valid = validate_slot_index_locked(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }
        const std::string blocked = indx::motion_blocked_reason(contract_, state_);
        if (!blocked.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "INDX load refused", blocked, "");
        }
        if (ops.load != FilamentOpSupport::Automatic) {
            return AmsError(AmsResult::NOT_SUPPORTED, "INDX load unavailable", ops.load_reason, "");
        }
        if (const SlotInfo* lane = system_info_.get_slot_global(slot_index)) {
            slot = *lane;
        }
        // LOAD_FILAMENT picks its tool up first: only the mounted one can be
        // loaded without swapping nozzles under a paused print.
        if (slot_index != system_info_.current_slot && print_paused()) {
            return AmsError(AmsResult::WRONG_STATE, "INDX load of a docked tool while paused",
                            paused_swap_reason(), "");
        }
        presets = state_.presets.value_or(presets);
    }
    const int temp_c =
        helix::ui::resolve_load_preheat_material(slot_index, &slot, nullptr)
            .value_or(helix::ui::PreheatTarget{app_constants::ams::kDefaultLoadPreheatTemp, ""})
            .temp_c;
    std::string cmd = indx::load_gcode(slot_index, slot.material, temp_c, presets);
    if (cmd.empty()) {
        return AmsErrorHelper::invalid_parameter("material '" + slot.material + "'");
    }
    spdlog::info("{} Loading tool {}: {}", backend_log_tag(), slot_index, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::LOADING);
}

} // namespace helix
