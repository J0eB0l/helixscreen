// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bondtech INDX: up to ten passive nozzles parked in a dock, picked up one at a
// time by a Smart Head that carries the only extruder and the only (induction)
// heater. A tool changer to the UI: one slot per tool, each with its own
// filament and spool, a change is a swap.
//
// Built on AmsBackendToolChanger, which already owns the tool slots, the slot
// store, the optimistic swap dispatch and park. INDX publishes no changer
// status object, so the tool on the head comes from the macro contract in
// indx_contract.h, fed to the base as a tool reading.

#include "ams_backend_toolchanger.h"
#include "indx_contract.h"

#include <string>
#include <vector>

namespace helix {

class AmsBackendIndx : public AmsBackendToolChanger {
  public:
    AmsBackendIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    void set_discovery(const helix::PrinterDiscovery& discovery) override;

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::INDX;
    }

    /// Status objects this backend reads; empty for any other printer.
    [[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);

    static constexpr BackendTraits kIndxTraits = [] {
        BackendTraits t = kTraits;
        // LOAD_FILAMENT picks the tool up, heats it and feeds: the UI neither
        // preheats nor needs the head loaded first.
        t.load_feeds_filament = true;
        t.supports_auto_heat_on_load = true;
        // CHANGE_TOOL homes an unhomed printer itself, fetching T0 to probe Z.
        t.filament_ops_self_home = true;
        return t;
    }();
    [[nodiscard]] BackendTraits traits() const override {
        return kIndxTraits;
    }

    /// True when the printer's own T<n> or CHANGE_TOOL macros switch the
    /// Spoolman active spool; HelixScreen then posts nothing.
    [[nodiscard]] bool manages_active_spool() const override {
        return macros_set_active_spool_;
    }

    /// Every tool prints through the one extruder, so per-extruder usage says
    /// nothing about which tool used it; the tracker charges the mounted slot.
    [[nodiscard]] std::optional<int> slot_for_extruder(int /*extruder_idx*/) const override {
        return std::nullopt;
    }

    [[nodiscard]] bool heads_share_heater() const override {
        return true;
    }
    [[nodiscard]] std::string nozzle_heat_blocked_reason() const override;

    /// The macro homes; HelixScreen sends no G28 of its own.
    [[nodiscard]] bool delegates_homing_to_printer() const override {
        return true;
    }

    [[nodiscard]] HeadFilamentOps head_filament_ops(int head) const override;
    [[nodiscard]] bool unload_is_manual() const override {
        return true;
    }
    /// Each LOAD_FILAMENT leaves its tool mounted and hot.
    [[nodiscard]] int max_heads_per_filament_op() const override {
        return 1;
    }

    [[nodiscard]] const char* get_klipper_object_name() const override {
        return "indx";
    }

    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    /// Why a change or park would be refused now, or empty. The same answer
    /// do_change_tool() and do_park_active_tool() refuse with.
    [[nodiscard]] std::string motion_blocked_reason() const;

  protected:
    AmsError additional_start_checks() override;
    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[AMS INDX]";
    }
    AmsError do_change_tool(int tool_number) override;
    AmsError do_park_active_tool() override;
    AmsError do_load_filament(int slot_index) override;

    /// Size the tool slots to @p count, keeping the unit's INDX identity.
    void size_tools(int count);

    helix::indx::Contract contract_;
    helix::indx::Reading state_;
    /// The change command per tool index, from discovery.
    std::vector<std::string> change_gcodes_;
    bool macros_set_active_spool_ = false;
    int sized_tool_count_ = -1;
};

} // namespace helix
