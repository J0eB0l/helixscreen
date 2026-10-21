// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_timer_guard.h"

#include "ams_subscription_backend.h"
#include "filament_slot_override_store.h"
#include "mount_feed_sequence.h"
#include "toolchanger_addon.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix {

class ToolChangerTestAccess;

/**
 * @file ams_backend_toolchanger.h
 * @brief Physical tool changer backend implementation
 *
 * Implements the AmsBackend interface for physical tool changers using
 * viesturz/klipper-toolchanger. Unlike filament-switching systems (Happy Hare,
 * AFC), tool changers have multiple physical toolheads that are swapped.
 *
 * Key differences from filament systems:
 * - Each "slot" is a complete toolhead with its own extruder
 * - No hub/selector - path topology is PARALLEL
 * - "Loading" means mounting the tool to the carriage
 * - No bypass mode (each tool IS the path)
 *
 * Klipper Objects (viesturz/klipper-toolchanger):
 * - toolchanger.status     (string): "ready", "changing", "error", "uninitialized"
 * - toolchanger.tool       (string): Current tool name ("T0") or null
 * - toolchanger.tool_number (int): Current tool number (-1 if none)
 * - toolchanger.tool_numbers (array[int]): All tool numbers [0, 1, 2]
 * - toolchanger.tool_names  (array[string]): All tool names ["T0", "T1", "T2"]
 *
 * Per-tool Objects:
 * - tool T0.active         (bool): Is this tool selected?
 * - tool T0.mounted        (bool): Is this tool mounted on carriage?
 * - tool T0.gcode_x_offset (float): X offset for tool
 * - tool T0.gcode_y_offset (float): Y offset for tool
 * - tool T0.gcode_z_offset (float): Z offset for tool
 * - tool T0.extruder       (string): Associated extruder name
 * - tool T0.fan            (string): Associated fan name
 *
 * G-code Commands:
 * - SELECT_TOOL TOOL=T{n}  - Mount specified tool
 * - UNSELECT_TOOL          - Unmount current tool (park it)
 * - T{n}                   - Tool change macro (same as SELECT_TOOL)
 */
class AmsBackendToolChanger : public AmsSubscriptionBackend {
  public:
    /**
     * @brief Construct tool changer backend
     *
     * @param api Pointer to IMoonrakerAPI (for sending G-code commands)
     * @param client Pointer to helix::IMoonrakerClient (for subscribing to updates)
     *
     * @note Pointers must remain valid for the lifetime of this backend.
     * @note Call set_discovered_tools() before start() to set tool names.
     */
    AmsBackendToolChanger(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /**
     * @brief Set discovered tool names from PrinterCapabilities
     *
     * Must be called before start() to initialize tool structures.
     * Tool names are extracted from printer.objects.list (e.g., "T0", "T1").
     *
     * @param tool_names Vector of tool names
     */
    void set_discovered_tools(std::vector<std::string> tool_names);

    /// Tool names plus the feeder, tool sensor, swap commands and material
    /// source resolved from discovery.
    void set_discovery(const helix::PrinterDiscovery& discovery) override;

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] AmsType get_type() const override;

    /// The material list the firmware itself validates against, once a frame
    /// has carried it. nullopt until then, or when the firmware states no such
    /// list - which keeps the edit dropdown unrestricted, like every other
    /// backend that treats material as a free-form label.
    [[nodiscard]] std::optional<std::vector<std::string>> get_supported_materials() const override;

    /// True once a frame has carried the firmware's slots AND palette: only
    /// then can an edit be snapped into the palette and normalized into the
    /// types before it is sent. Before that, an edit stays local as on any
    /// tool changer.
    [[nodiscard]] bool firmware_stores_color_and_material(int slot_index) const override;

    /// Constant capability answers; see BackendTraits.
    static constexpr BackendTraits kTraits = [] {
        BackendTraits t;
        // Tool changers give each tool its own independent toolhead with no shared
        // physical tray/housing, so the AMS detail view's tray graphic is hidden.
        t.has_physical_tray = false;
        // A slot's status is where its tool sits (carriage, dock, removed); nothing
        // here senses filament.
        t.slot_status_tracks_filament = false;
        // The per-slot tool badge ("T0", "T1", ...) is redundant with the toolhead
        // label shown below each slot on a tool changer.
        t.should_hide_slot_tool_badge = true;
        // Reset re-runs the changer's initialisation, which the toolchanger's own
        // recovery already offers.
        t.supports_reset = false;
        return t;
    }();
    /// With a firmware feed command (Z-Mod's afclite shim), Load feeds filament
    /// and the firmware heats the head it feeds.
    static constexpr BackendTraits kTraitsWithFeed = [] {
        BackendTraits t = kTraits;
        t.load_feeds_filament = true;
        t.supports_auto_heat_on_load = true;
        return t;
    }();
    /// One extruder drive serving every tool (MedusaHC): a load or unload
    /// mounts the tool, heats it and moves filament through it itself, so the
    /// UI neither preheats nor treats a paused print as a time to do it, and a
    /// head that reads loaded still accepts Load.
    static constexpr BackendTraits kTraitsMountFeed = [] {
        BackendTraits t = kTraits;
        t.load_feeds_filament = true;
        t.supports_auto_heat_on_load = true;
        t.supports_auto_heat_on_unload = true;
        t.filament_ops_self_home = true;
        return t;
    }();
    [[nodiscard]] BackendTraits traits() const override {
        if (tool_feed_.present) {
            return kTraitsWithFeed;
        }
        return mounts_to_feed_ ? kTraitsMountFeed : kTraits;
    }

    /// Whether this machine moves filament only through the mounted tool
    /// (toolchanger_addon::feeds_through_mount()). Set from discovery.
    void set_mounts_to_feed(bool mounts) {
        mounts_to_feed_ = mounts;
    }

    /// A load or unload running its mount-to-feed sequence can be cancelled:
    /// the heater goes off and the carriage goes back.
    [[nodiscard]] bool can_cancel_operation() const override;

    /// Every tool docks: the carriage can hold none.
    [[nodiscard]] bool carriage_can_be_empty() const override {
        return true;
    }

    /**
     * @brief What each head can do with filament.
     *
     * Nothing on a klipper-toolchanger moves filament unless the firmware
     * publishes a feed for that head: then Load is that feed. No feed reverses,
     * so Unload is always the user's job. Mounting and parking are never a
     * filament op: the toolhead tap and park_active_tool() do those.
     */
    [[nodiscard]] HeadFilamentOps head_filament_ops(int head) const override;

    /// The answers head_filament_ops() gives, shared with the mock so its
    /// tool-changer persona cannot drift from this backend.
    /// @param feeds_head    The firmware publishes a feed for the head.
    /// @param cut_to_unload The machine's unload is a cut at the head (unload_is_manual()).
    [[nodiscard]] static HeadFilamentOps head_ops_for(bool feeds_head, bool cut_to_unload);

    /// One head per tool, fed from that tool's own slot.
    [[nodiscard]] int filament_head_count() const override {
        return get_system_info().total_slots;
    }
    /// A machine that moves filament through the mounted tool can work every
    /// tool in one run: Load all and Unload all pick several heads at once.
    [[nodiscard]] int max_heads_per_filament_op() const override {
        return mounts_to_feed_ ? get_system_info().total_slots : 1;
    }
    [[nodiscard]] std::optional<int> head_for_slot(int slot) const override {
        if (slot < 0 || slot >= get_system_info().total_slots) {
            return std::nullopt;
        }
        return slot;
    }
    [[nodiscard]] std::optional<int> head_source_slot(int head,
                                                      const AmsSystemInfo&) const override {
        return head_for_slot(head);
    }
    /// Nothing here senses filament in a head.
    [[nodiscard]] std::optional<bool> head_has_filament(int) const override {
        return std::nullopt;
    }

    /// Z-Mod's Creator 5 has no automatic unload: the user cuts the filament at
    /// the head and pulls it out by hand.
    [[nodiscard]] bool unload_is_manual() const override {
        return zmod_c5_row_active();
    }

    // Marker for tool-changer expected-hardware recording during wizard setup.
    [[nodiscard]] const char* get_klipper_object_name() const override {
        return "toolchanger";
    }
    [[nodiscard]] helix::ui::LaneNoun lane_noun() const override {
        return helix::ui::LaneNoun::Tool;
    }

    /// ASSIGN_TOOL belongs to klipper-toolchanger. A machine that drives swaps
    /// with its own T<n> commands instead (tool_commands_.present) has no
    /// tool-number indirection to rewrite: those macros name the physical tool
    /// outright. Klipper answers an unknown command by logging it and carrying
    /// on, so a remap sent to such a machine is accepted and never applied.
    ///
    /// The file's own tool numbers are the only thing left to change, so such a
    /// machine rewrites the job instead. That needs no firmware cooperation but
    /// costs a download and an upload of the whole G-code, and it needs the
    /// HelixPrint plugin and a transport that keeps a local copy - open_remap_modal()
    /// gates on remap_block(), which asks PrintPreparationManager::gcode_rewrite_block(),
    /// the same answer the pre-print options use, so the picker is never offered
    /// when the rewrite cannot land.
    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return tool_commands_.present ? RemapStrategy::GcodeRewrite : RemapStrategy::Native;
    }

    /// A tool changer has one extruder per tool, so its table is identity — but it
    /// is a real table the firmware owns and ASSIGN_TOOL rewrites, and
    /// get_tool_mapping() returns it.
    [[nodiscard]] bool owns_tool_mapping_table() const override {
        return true;
    }

    /// Klipper tool-changers have one extruder per tool. Tool N sources slot N
    /// directly — identity mapping — which activates per-extruder consumption
    /// tracking in FilamentConsumptionTracker.
    [[nodiscard]] std::optional<int> slot_for_extruder(int extruder_idx) const override {
        if (extruder_idx < 0 || extruder_idx >= static_cast<int>(get_system_info().total_slots)) {
            return std::nullopt;
        }
        return extruder_idx;
    }

    // Path visualization (PARALLEL topology for tool changers)
    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;

    /// Z-Mod's Creator 5 carries the same firmware-managed "endless spool
    /// mode" the AD5X IFS backend reports for stock zMod: on runout the
    /// firmware switches to the next head whose colour AND type both match,
    /// with no flag for HelixScreen to write. Every other tool changer
    /// answers the base default (Unsupported).
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;

    /// A dock-sensor fault is not a loading failure: nothing is being loaded and
    /// a hot end changer has no filament path for the user to check. Publishing
    /// it here routes it to AmsErrorBridge with this backend's own wording,
    /// instead of the generic load dialog whose Retry would mount the very tool
    /// the fault says nobody can vouch for.
    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

  protected:
    // Operations. Gated by AmsSubscriptionBackend's NVI wrapper.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    /// Load all / Unload all on a mount-to-feed changer: the printer's own
    /// LOAD_ALL_FILAMENT / UNLOAD_ALL_FILAMENT when the selection is every
    /// tool and the macro exists, else one sequenced run over the tools.
    AmsError do_filament_batch(const std::vector<int>& slots, bool load) override;
    AmsError do_select_slot(int slot_index) override;

    /// On a tool changer, selecting a slot means mounting that toolhead —
    /// do_select_slot() forwards to do_change_tool(), which emits SELECT_TOOL
    /// and swaps what is on the carriage.
    [[nodiscard]] bool select_slot_moves_toolhead() const override {
        return true;
    }

    /**
     * @brief Mount a physical toolhead. The argument is a SLOT INDEX.
     *
     * Named `tool_number` only because the base signature is, and validated as a
     * slot: the implementation emits `SELECT_TOOL T={n}`, which resolves through
     * the toolchanger's own list and therefore BYPASSES ASSIGN_TOOL remapping.
     * That is deliberate — a tap on lane 2 must mount the toolhead the user
     * tapped, not whichever physical tool the slicer's T2 was reassigned to.
     *
     * So on this backend a tool number and a slot index are NOT
     * interchangeable, and anything that has to cross between them goes through
     * the tool map (AmsSystemInfo::tool_to_slot_map / SlotInfo::mapped_tool),
     * never by assuming they are equal.
     */
    AmsError do_change_tool(int tool_number) override;

    /// Whatever is on the carriage goes back to its dock: UNSELECT_TOOL, or the
    /// machine's own unmount where its extra runs the swap.
    AmsError do_park_active_tool() override;

  public:
    /// A machine whose own swap commands have no unmount can only trade one
    /// tool for another.
    [[nodiscard]] bool supports_tool_park() const override {
        return !tool_commands_.present || !tool_commands_.unselect.empty();
    }

    /// Sends the feeder's own open or close command, refused while a print or
    /// a swap holds the machine: the gripper is all that holds the filament.
    AmsError set_feeder_open(bool open) override;
    [[nodiscard]] bool supports_feeder_control() const override {
        return feeder_.present;
    }
    [[nodiscard]] bool feeder_open() const override;

    /**
     * @brief Offer Unload only for the tool currently on the carriage.
     *
     * The base rule's PARALLEL arm keys on is_present(), which holds for every
     * slot on this backend forever — a slot here is a physical toolhead, never
     * EMPTY or UNKNOWN. That read true everywhere, so the context menu offered
     * Unload (UNSELECT_TOOL) on tools parked in their docks and, through
     * decide_can_load()'s inverted `!toolhead_unload` factor, kept Load disabled
     * on all of them (prestonbrown/helixscreen#1199).
     *
     * A klipper-toolchanger carries exactly one tool at a time, and unload_filament()
     * is an unmount rather than a filament retraction, so `toolchanger.tool_number`
     * is the whole answer. Load stays offered on every docked tool, which is what
     * mounting one means here.
     */
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

    /**
     * @brief "Seated and loaded" is "mounted on the carriage" here — same answer
     *        as can_unload_from_toolhead().
     *
     * The base rule (current_slot + filament_loaded) would agree on this backend
     * in the settled case, because both fields are written from one tool number,
     * but it cannot see a dock-sensor fault: apply_tool_sensor_locked() holds the
     * last known tool through one on purpose, so the pair keeps naming a
     * perfectly plausible carriage tool while the sensors say they cannot tell.
     *
     * That matters because the affordance gate is an OR. AmsContextMenu combines
     * the snapshot it took at open (can_unload_from_toolhead) with the live
     * accessors — `pending_is_loaded_ || slot_is_actively_loaded(i) ||
     * slot_has_filament_at_toolhead(i)` — so a withdrawal by one of them is
     * restored by any other that still reads true. Both questions therefore
     * resolve through the same private rule, and the menu needs no knowledge of
     * dock sensors to get the right answer.
     */
    [[nodiscard]] bool slot_is_actively_loaded(int slot_index) const override;

    /// A swap is SELECTING for its whole duration on a controller that names no
    /// direction, and UNLOADING then SELECTING on one that does. A
    /// mount-to-feed run walks the filament vocabulary on top of that:
    /// HEATING, LOADING, PURGING and UNLOADING are its stages.
    [[nodiscard]] bool action_tracks_step_operation(AmsAction action) const override {
        // HEATING is here despite a swap never heating: it is the optimistic
        // marker AmsOperationSidebar::start_operation() sets the moment a user
        // starts an operation, before any frame arrives. Omitting it made the
        // sidebar hide the bar and clear target_load_slot_ on our OWN dispatch,
        // after which the rest of a user-initiated swap read as externally
        // started.
        if (mounts_to_feed_) {
            return action == AmsAction::SELECTING || ams_action_is_filament_operation(action);
        }
        return action == AmsAction::SELECTING || action == AmsAction::UNLOADING ||
               action == AmsAction::HEATING;
    }

    /// Explains the one refusal a user can do something about: the dock sensors
    /// cannot say which tool is on the head, so every slot's Unmount is
    /// disabled and none of them look like the reason.
    [[nodiscard]] std::string unload_blocked_reason(int slot_index) const override;

    /// Step bar for a swap, built from what THIS machine actually reports.
    ///
    /// A hotend changer's phases are the add-on's `operation` (dropping/picking)
    /// and, where the controller publishes it, the frame-side gripper opening
    /// and closing around them. A changer with neither - plain
    /// klipper-toolchanger, or MedusaHC stock, whose only signal is
    /// `toolchanger.status == "changing"` - returns a SUPPRESSED model: it has no
    /// phases to show, and the generic Heat/Feed/Purge fallback describes a
    /// filament system rather than a tool changer.
    [[nodiscard]] OperationStepModel get_operation_step_model(StepOperationType op) const override;

    /// The shared operation-phase subject. apply_tool_sensor_locked() writes the
    /// step index into system_info_.operation_phase, the same route the U1 uses.
    [[nodiscard]] lv_subject_t* get_operation_step_index_subject(StepOperationType op) override;

    // NOTE: has_per_slot_loaded_authority() is deliberately NOT overridden, and
    // this backend has a reason none of the others do: it carries no filament
    // signal at all. get_slot_filament_segment() returns NOZZLE unconditionally,
    // no per-tool switch is read, and is_filament_loaded() is just
    // `tool_number >= 0`. The only fact the parse can state is which tool is on
    // the carriage — single-valued, which is exactly what the aggregate
    // current_slot + filament_loaded pair encodes, derived from
    // klipper-toolchanger's own toolchanger.tool_number — through the forward
    // tool map, since that number is the ASSIGNED one and ASSIGN_TOOL can point
    // it at any physical tool. The per-slot LOADED
    // stamp is derived FROM that pair (refresh_slot_statuses_locked), so
    // believing it back would only add staleness — the same argument that keeps
    // Happy Hare on the aggregate rule (prestonbrown/helixscreen#1199).
    //
    // In particular `tool <name>.mounted` is NOT that authority: it arrives on a
    // separate Moonraker object, and an all-tools-mounted payload is a shape we
    // emit ourselves in mock mode.
    //
    // slot_is_actively_loaded() IS overridden, which is the other route the base
    // offers ("backends may still override outright where neither rule fits").
    // The aggregate rule is right about which tool is seated and blind to the
    // dock-sensor fault that says the seat cannot be read at all.

    // Recovery
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // Configuration
    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    void clear_slot_override(int slot_index) override;
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) override;
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    void persist_external_identity_impl(int slot_index,
                                        const helix::ams::Observation& spoolman) override;
    [[nodiscard]] AmsError can_set_tool_mapping(int tool_number, int slot_index) const override;
    /// can_set_tool_mapping() for a caller that holds mutex_.
    [[nodiscard]] AmsError can_set_tool_mapping_locked(int tool_number, int slot_index) const;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    /// Point G-code tool @p tool_number at the physical tool named
    /// @p physical_tool_name, or refuse when this machine has no such command.
    /// The one emitter, so the refusal cannot be reached around.
    AmsError assign_tool(const std::string& physical_tool_name, int tool_number);

    // Tool mapping via klipper-toolchanger ASSIGN_TOOL command
    AmsError reset_tool_mappings() override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // Bypass mode (not applicable for tool changers)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    /**
     * @brief Declare the filament feeder this machine exposes, if any
     *
     * Resolved from discovery by toolchanger_addon::resolve_feeder() and handed over at
     * construction. Absent by default, so a tool changer nobody told anything
     * exposes no device actions -- which is every klipper-toolchanger build
     * that swaps a whole toolhead.
     */
    void set_feeder(helix::toolchanger_addon::Feeder feeder) {
        feeder_ = std::move(feeder);
    }

    /// Dock-sensor reader. When set, its answer overrides toolchanger.tool_number.
    void set_tool_sensor(helix::toolchanger_addon::ToolSensor sensor) {
        tool_sensor_ = std::move(sensor);
        // The Z-Mod row's phase source is gcode-response markers, and its
        // markers name the direction by construction (Grab/_T_IN is a pick,
        // Unload/_T_OUT is a dock). The direction capability is therefore known
        // from the provider row itself, not only once a marker arrives — which
        // matters because the step bar snapshots this latch when it is built,
        // and a bar built at dispatch time (before the macro's first marker)
        // would otherwise pin a direction-less, suppressed sequence forever.
        if (zmod_c5_row_active()) {
            direction_reported_ = true;
        }
    }
    /// Swap commands for a machine without klipper-toolchanger. Absent leaves
    /// SELECT_TOOL/UNSELECT_TOOL in place.
    void set_tool_commands(helix::toolchanger_addon::ToolCommands commands) {
        tool_commands_ = std::move(commands);
    }

    /// Filament feed through the firmware's own shim. Absent leaves Load as a
    /// tool mount.
    void set_tool_feed(helix::toolchanger_addon::ToolFeed feed) {
        tool_feed_ = std::move(feed);
    }

    /// The firmware material store this machine keeps. Absent means
    /// HelixScreen's own store is the only one.
    void set_material_source(helix::toolchanger_addon::MaterialSource source) {
        std::lock_guard<std::mutex> lock(mutex_);
        material_source_ = std::move(source);
    }

    // Device Actions -- the feeder, when the machine has one.
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError do_execute_device_action(const std::string& action_id, const std::any& value) override;

  protected:
    // Allow test helper access to private members
    friend class ToolChangerCharHelper;
    friend class ToolChangerTestAccess;

    // --- AmsSubscriptionBackend hooks ---
    AmsError additional_start_checks() override;

    /// Post-start work. Loads the slot-override store here and NOT in
    /// additional_start_checks(), which start() calls with mutex_ held.
    void on_started() override;
    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[AMS ToolChanger]";
    }
    SlotInfo* cached_slot_locked(int slot_index) override;
    [[nodiscard]] std::optional<AmsAction> step_action_locked() const override {
        return step_action_;
    }
    void on_stopping() override;
    [[nodiscard]] bool needs_mount_to_feed(int head) const override;

    /// Apply an add-on dock-sensor reading over the toolchanger's own claim.
    /// Caller holds mutex_.
    void apply_tool_sensor_locked(const helix::toolchanger_addon::ToolReading& reading);

    /// Send a tool operation: set @p action optimistically, dispatch @p gcode
    /// through ensure_homed_then(), and resolve on the macro's ack. Caller must
    /// NOT hold mutex_ and must have passed check_preconditions().
    AmsError dispatch_operation(std::string gcode, AmsAction action);

  private:
    // --- Mount-to-feed (mount_feed_sequence.h) ---------------------------------
    /// Start a Load (@p load) or Unload of @p heads' filament through the
    /// mounted tool, one head or several. Main thread.
    AmsError start_mount_feed(const std::vector<int>& heads, bool load);
    /// Everything a run needs, read off the printer: per-head temp and mount,
    /// the carriage restore, the printer's own macro, the (English) prompt.
    [[nodiscard]] helix::mount_feed::Request build_feed_request(const std::vector<int>& heads,
                                                                bool load);
    /// Send the run's current stage, or end the run when none is left.
    void send_feed_stage();
    void on_feed_stage_acked(uint64_t generation);
    void on_feed_stage_failed(uint64_t generation, const std::string& message);
    void on_feed_reply(helix::mount_feed::Reply reply);
    /// The console watcher's signal, once a reply or a dismissal arrives.
    void on_feed_signal(helix::mount_feed::ConsoleSignal signal);
    /// A lost connection (or a klippy restart) under a live run: the full
    /// cleanup goes out, best effort, and the run resets without waiting for
    /// an ack that can no longer arrive. Shared by on_started() (reconnect)
    /// and on_stopping().
    void abandon_feed_run_after_disconnect();
    /// The operator wait outlived its timeout: unwind the run and say so.
    void handle_wait_elapsed(uint32_t elapsed_ms);
    /// Arm or drop the one-second timer that watches the operator wait. LVGL
    /// must be up for the timer; without it (unit tests) the wait is driven
    /// through handle_wait_elapsed() by hand.
    void set_wait_watchdog(bool armed);
    void feed_wait_tick();
    /// Why a run unwound early, for the toast that names what got done.
    enum class FeedStopReason : uint8_t { Cancelled, TimedOut, Failed };
    /// A run stopping early says what it did finish. A single tool's Cancel
    /// stays silent; everything else accounts for itself. @p completed was
    /// read off the run BEFORE its unwind: abort() collapses the stage list
    /// the answer is derived from.
    void toast_partial_run(FeedStopReason why, const std::vector<int>& completed);
    /// One error toast about a run, with the no-LVGL test guard in one place.
    void show_feed_toast(const std::string& message);
    /// Mirror the run onto the published action and step. Caller holds mutex_.
    void publish_feed_state_locked();
    void register_mount_feed_listener();
    void unregister_mount_feed_listener();
    /// The command that mounts @p slot_index, and the one that parks.
    [[nodiscard]] std::string mount_gcode(int slot_index) const;
    [[nodiscard]] std::string park_gcode() const;

    bool mounts_to_feed_ = false;
    /// The run in progress. Main thread only; feed_active_ mirrors it under mutex_.
    std::optional<helix::mount_feed::Run> feed_run_;
    uint64_t feed_generation_ = 0;
    bool feed_active_ = false;
    AmsAction feed_action_ = AmsAction::IDLE;
    int feed_phase_ = -1;
    std::optional<AmsAction> feed_step_action_;
    /// Watches the operator prompt while it is up. Main thread (lv_timer).
    helix::ui::LvglTimerGuard feed_wait_timer_;
    std::chrono::steady_clock::time_point feed_wait_started_{};
    /// The last toast a feed run raised, recorded before the LVGL guard so
    /// unit tests can pin the wording without a display.
    std::string feed_last_toast_;

    /// Z-Mod's Creator 5 row narrates its swaps through gcode-response markers
    /// (toolchanger_addon::parse_zmod_marker) rather than a status object, so
    /// on_started() subscribes notify_gcode_response for that row alone and
    /// on_stopping() tears the subscription down.
    void register_zmod_marker_listener();
    void unregister_zmod_marker_listener();
    /// Main-thread half of the marker listener: classifies the line and folds a
    /// matching marker into the same phase machinery a status frame drives.
    void on_zmod_marker_line(const std::string& line);
    /// Whether the active provider row is Z-Mod's Creator 5 changer.
    [[nodiscard]] bool zmod_c5_row_active() const {
        return tool_sensor_.present &&
               tool_sensor_.provider_name == toolchanger_addon::kZmodC5ProviderName;
    }
    /// Fold one afclite status frame in: fault flag to the error surface,
    /// activity word and lane map to the log.
    void apply_afc_state_locked(const helix::toolchanger_addon::AfcStateReading& afc);
    /// Ask Z-Mod where every tool sits (T_INFO is read-only). Its answer fixes
    /// the boot state on firmware whose active_tool_id only recomputes inside
    /// its own commands.
    void query_zmod_tool_state();

  private:
    /// Feeder this machine exposes; absent unless set_feeder() said otherwise.
    helix::toolchanger_addon::Feeder feeder_;
    /// Absent on every tool changer without dock sensors.
    helix::toolchanger_addon::ToolSensor tool_sensor_;
    /// Absent whenever klipper-toolchanger owns the swap.
    helix::toolchanger_addon::ToolCommands tool_commands_;
    helix::toolchanger_addon::ToolFeed tool_feed_;
    /// Firmware material store this machine keeps; absent unless
    /// set_material_source() said otherwise.
    helix::toolchanger_addon::MaterialSource material_source_;
    /// Latched from the material source. Each arrives only in frames that
    /// changed it, so a frame without it is not a clearing.
    std::optional<std::vector<std::string>> firmware_valid_types_;
    std::optional<std::vector<std::pair<int, std::uint32_t>>> firmware_palette_;
    bool firmware_slots_seen_ = false;
    /// Latest per-dock occupancy from the dock sensors, indexed by slot: true
    /// seated, false empty, nullopt never reported. Kept across frames, because
    /// Moonraker republishes only what CHANGED and a frame carrying just the
    /// tool number says nothing about the docks.
    std::vector<std::optional<bool>> dock_seated_;
    /// Per-head filament presence from the firmware's own slots (Z-Mod 1.7.4+
    /// hasFilament), nullopt never reported. Kept beside dock_seated_ because
    /// it answers a different question: docked says where the HEAD is, this
    /// says whether there is filament in it.
    std::vector<std::optional<bool>> head_filament_;
    /// The afclite shim's standing fault text, set while its error_state flag
    /// reads true. Empty when the shim reports healthy (or is absent).
    std::string afclite_fault_message_;
    /// Last afclite activity word logged, so a frame repeating it stays silent.
    std::string last_afc_state_logged_;
    /// Last lane→tool map logged verbatim; identity is the expected answer, and
    /// a firmware that remaps is exactly what a field capture needs to see.
    std::string last_lane_map_logged_;
    /// Whether the printer's macro list carries T_INFO (the read-only tool
    /// state query Z-Mod ships on the C5).
    bool tinfo_macro_present_ = false;
    /// Whether the T_INFO block currently in flight on the console has named a
    /// HEAD row; the block-end line is what turns "no HEAD row" into "the
    /// carriage is empty".
    bool tinfo_block_saw_head_ = false;
    /// Frame-side gripper, and whether this machine reports it AT ALL. The
    /// second flag is what get_operation_step_model() keys on: a controller that
    /// publishes feeder_open earns the release/grip steps, and a machine that
    /// never mentions it gets a bar with only the phases it can actually drive
    /// rather than two steps that would sit grey forever. Latched once true -
    /// Moonraker republishes only changed fields, so a later frame omitting
    /// feeder_open is not the machine retracting the capability.
    bool feeder_open_ = false;
    bool feeder_state_reported_ = false;
    /// Dock sensors cannot say what is on the head. Latched rather than inferred
    /// from current_slot, because the sensor-error path deliberately HOLDS the
    /// last known tool: current_slot stays >= 0 through the fault and names a
    /// tool that may not be there.
    bool sensor_error_ = false;
    /// The gripper has been open at some point during the operation currently
    /// running. Cleared when it ends.
    ///
    /// Without this, idle-with-the-gripper-closed is ambiguous in a THIRD way:
    /// it is the resting state, it is the closing grip at the end of a swap, and
    /// it is also every frame between dispatching SELECT_TOOL and the machine
    /// actually moving. Treating that last one as the closing grip paints the
    /// bar complete for the second before the swap starts. The closing grip is
    /// only recognisable once the gripper has actually opened.
    bool feeder_opened_this_operation_ = false;
    /// Whether this machine's phase word names the swap DIRECTION. Latched from
    /// ToolReading::phase_names_direction, which is answerable from the first
    /// status frame - the phase WORDS are not, since "picking" only appears once
    /// a swap is already running and the step bar has to be built before that.
    bool direction_reported_ = false;
    /// Whether a real non-idle operation frame ("dropping"/"picking"/"changing")
    /// has been seen since the idle-with-gripper-open hold in
    /// apply_tool_sensor_locked() last released. That hold exists because a
    /// swap's own FIRST frame is idle-with-the-gripper-open, but its condition
    /// is partly derived from the action the hold itself sets - so without a
    /// separate signal, a machine that settles into a LATER idle-with-open
    /// frame (e.g. an unmount that ends with the head empty and nothing to
    /// re-grip) would keep re-satisfying the same condition forever and never
    /// reach IDLE again. Bounding the hold with this flag means it can only
    /// ever catch the leading idle-with-open frame: once a real operation
    /// frame has confirmed the swap is actually running, a SUBSEQUENT idle
    /// frame means it has settled, not recurred, and must be believed.
    /// Reset to false when the hold releases into IDLE, and again at the start
    /// of every fresh dispatch (begin_dispatch_locked()) in case the prior
    /// swap reached IDLE through a path that bypassed the idle branch below
    /// (e.g. parse_toolchanger_state() alone, on a frame with no addon data).
    bool operation_confirmed_ = false;

    /// What the step at system_info_.operation_phase projects to, written with
    /// it. get_system_info() publishes it as the action while a swap runs.
    std::optional<AmsAction> step_action_;

    /// Snapshot of feeder_state_reported_ / direction_reported_ taken the last
    /// time get_operation_step_model() built a sequence. resolve_step_locked()
    /// resolves its index against this SAME snapshot, not whatever the latches
    /// read right now: the model is captured once (at operation start, by the
    /// sidebar) while the index is recomputed on every frame, and Moonraker
    /// republishes only the fields that CHANGED. A latch that flips mid-
    /// operation without the model being rebuilt would otherwise put the index
    /// computation against a longer/different sequence than the one actually
    /// on screen. get_operation_step_model() is const, so these are mutable.
    ///
    /// step_model_captured_ stays false until the first call: nothing has
    /// pinned a sequence yet, so resolve_step_locked() falls back to
    /// the LIVE latches rather than the (false, false) power-on default, which
    /// would otherwise build an always-empty sequence for any caller that asks
    /// for the phase without ever having asked for the model first.
    mutable bool step_model_captured_ = false;
    mutable bool step_model_feeder_reported_ = false;
    mutable bool step_model_direction_reported_ = false;

    /**
     * @brief Parse toolchanger state from Moonraker JSON
     *
     * Extracts toolchanger object and updates system_info_.
     *
     * @param tc_data JSON object containing toolchanger data
     */
    void parse_toolchanger_state(const nlohmann::json& tc_data);

    /// Latch one firmware material reading: the types and palette it carried,
    /// and each slot's material and colour filed as vendor-cache observations.
    /// Caller holds mutex_.
    void apply_material_reading_locked(const helix::toolchanger_addon::MaterialReading& reading);

    /// Is this slot's toolhead on the carriage, as far as anything can tell?
    /// The one rule behind can_unload_from_toolhead() and
    /// slot_is_actively_loaded(), which ask it for different reasons and must
    /// not be able to disagree. Caller holds mutex_.
    [[nodiscard]] bool slot_is_mounted_locked(int slot_index) const;

    /// The step `operation` maps to under the model this machine gets: its index
    /// (-1 when the phase maps to none) and the action it projects to.
    struct ResolvedStep {
        int index = -1;
        std::optional<AmsAction> action;
    };

    /// `mid_operation` says whether a swap was running when the frame arrived,
    /// which is the only thing separating the closing grip of one from the
    /// resting closed gripper. Caller holds mutex_.
    ResolvedStep resolve_step_locked(const std::string& operation, bool mid_operation) const;

    /// Layer the user's stored spool metadata over a slot. Caller holds mutex_.
    ///
    /// Unlike every other backend that does this, there is nothing underneath:
    /// parse_tool_state() reads `mounted` and `active` and nothing else, so
    /// klipper-toolchanger reports no material, colour, brand or weight at all.
    /// The store is the SOLE source of filament identity here, not a layer over
    /// a firmware reading, and initialize_tools() resets colour to default grey
    /// on every rediscovery - which is exactly what used to wipe the user's edit.

    /// The store is the sole source of filament identity here, so a clear
    /// blanks every identity field: nothing will restate them.
    void clear_override_fields(SlotInfo& slot) const override;

    /// The shared lane_data namespace this backend co-authors. request_resync()
    /// re-reads it only where firmware states no identity of its own.
    helix::ams::FilamentSlotOverrideStore* lane_record_store() override {
        return override_store_.get();
    }

    /// klipper-toolchanger reports no filament identity, so nothing here files
    /// a vendor-cache reading from a status frame and the persisted record is
    /// the lane's only account of what a slot holds.
    [[nodiscard]] bool firmware_publishes_lane_identity() const override {
        return false;
    }

    /**
     * @brief Parse individual tool state from Moonraker JSON
     *
     * Updates the slot corresponding to this tool.
     *
     * @param tool_name Tool name (e.g., "T0")
     * @param tool_data JSON object containing tool data
     */
    void parse_tool_state(const std::string& tool_name, const nlohmann::json& tool_data);

    /**
     * @brief Re-derive every slot's LOADED stamp from the carriage tool.
     *
     * Called from both parse paths so the per-slot status and the aggregate pair
     * are always computed from the same value and cannot disagree. Only
     * AVAILABLE and LOADED occur on this backend — a toolhead is always present.
     *
     * @note Must be called while holding mutex_.
     */
    void refresh_slot_statuses_locked();

    /**
     * @brief Convert toolchanger status string to AmsAction
     *
     * @param status Status string from toolchanger.status
     * @return Corresponding AmsAction enum value
     */
    static AmsAction status_to_action(const std::string& status);

    /**
     * @brief Initialize tool structures based on discovered tool names
     *
     * Creates SlotInfo entries for each tool.
     */
    void initialize_tools();

    /**
     * @brief Find slot index for a tool name
     *
     * @param tool_name Tool name (e.g., "T0", "T1")
     * @return Slot index or -1 if not found
     */
    [[nodiscard]] int find_slot_for_tool(const std::string& tool_name) const;

    // === Optimistic dispatch + macro-ack resolution =========================
    //
    // The only thing that can move the action off the value a dispatch sets is
    // a `toolchanger.status` frame, and Moonraker only pushes fields whose value
    // CHANGED. klipper-toolchanger short-circuits SELECT_TOOL on the tool already
    // on the carriage ("Tool tool T4 already selected") without touching
    // `status`, so a no-op produces no frame at all and the action never leaves
    // SELECTING — the filament panel spins and is_busy() locks out every later
    // operation until restart. Same defect and same fix as AFC (#1183): set the
    // action optimistically so there is a transition to complete, and resolve it
    // on the macro's own gcode ack when the toolchanger never claimed the op.

    /// Action the most recent dispatch set optimistically, cleared once that
    /// dispatch has been resolved — by its ack, by klipper-toolchanger taking
    /// the operation over, or by a newer dispatch superseding it.
    std::optional<AmsAction> pending_dispatch_action_;

    /// Monotonic dispatch counter. The ack carries the generation it was issued
    /// under, so an ack that lands after a newer dispatch resolves nothing.
    uint64_t dispatch_generation_{0};

    /// Set @p action optimistically for a dispatch that is about to go out and
    /// return the generation its ack must present. Caller holds mutex_.
    uint64_t begin_dispatch_locked(AmsAction action);

    /// Undo an optimistic set whose gcode never left the building. Caller must
    /// NOT hold mutex_.
    void abandon_dispatch(uint64_t generation);

    /// Resolve a dispatch to IDLE on its macro's gcode ack, but only when the
    /// toolchanger never took the operation over. Main thread; must NOT hold
    /// mutex_.
    void finalize_dispatch_after_macro(uint64_t generation);

    // Tool changer specific state
    std::vector<std::string> tool_names_; ///< Tool names from discovery

    // Cached toolchanger state
    bool tools_initialized_{false}; ///< Have we received initial state?

    // Per-tool mounted state (for quick lookup)
    std::vector<bool> tool_mounted_; ///< Which tools are mounted
};

} // namespace helix
