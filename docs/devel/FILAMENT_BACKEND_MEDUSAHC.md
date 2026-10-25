# MedusaHC (Tool-Changer Add-On)

MedusaHC is a budget **hotend changer**: it swaps only the hot end (heater, thermistor,
fan), not a whole toolhead. It is not its own `AmsType` and has no backend of its own.
It is a klipper-toolchanger printer, so `AmsBackendToolChanger` drives it, and everything
MedusaHC adds on top lives in one module.

- Hardware / config: [Irbis3D/MedusaHC](https://github.com/Irbis3D/MedusaHC)
- Experimental Python controller: [Irbis3D/MedusaHC-Python-Controller](https://github.com/Irbis3D/MedusaHC-Python-Controller)

## Why it is not a backend

Upstream `printer.cfg` has an uncommented `[include toolchanger.cfg]`, and that file
declares `[toolchanger]` plus `[tool T0..T3]`. Those are exactly the objects
`AmsBackendToolChanger` already reads, so a stock MedusaHC is detected and driven with no
MedusaHC-specific code at all. Adding a ninth `AmsType` would have forked a working
backend for no gain.

What a hotend changer genuinely adds is two things klipper-toolchanger cannot answer, and
those are the whole content of `include/toolchanger_addon.h` +
`src/printer/toolchanger_addon.cpp`.

## The two add-ons

### 1. Dock sensors are the authority on which tool is mounted

`Macros/toolchanger.cfg` ships `verify_tool_pickup: False` and
`require_tool_present: False`, so klipper-toolchanger never verifies a pickup.
`toolchanger.tool_number` is **only ever "what `SELECT_TOOL` last set"** - after a failed
or partial pickup it names a tool that is not on the head.

The physical answer comes off dock sensors through MedusaHC's own pin_watch Klipper extra
(Scripts/pin_watch.py upstream). It is not a stock Klipper module - it is absent from all
136 files in Klipper3d/klipper's klippy/extras - so the object name is a real signature.
The official controller agrees: its `_current_tool()` returns `pin_watch.current_tool`.

`AmsBackendToolChanger::apply_tool_sensor_locked()` therefore runs **after**
`parse_toolchanger_state()` in the same frame, so the sensor answer wins.

`current_tool == -2` is a third state, distinct from "no tool": the sensors cannot tell.
Reporting it as `-1` would invite a tool change against an unknown carriage state, so the
backend holds the last known tool and raises `AmsAction::ERROR` instead. A swap in flight
reads `-2` for a moment between the head sensor and the dock sensor changing, so an
unreported `-2` is a fault only outside one (`toolchanger_addon::sensor_error_is_fault()`);
a mount-to-feed run's Mount and Restore stages count as a swap. A `sensor_error` the
firmware itself reports is a fault during a swap, and at rest unless a definite
`current_tool` (-1 or a tool) sits beside it. Irbis3D's flag latches the last operation's
failure until the next operation begins, while `current_tool` follows the dock pins live,
so a definite reading at rest, or one arriving after the flag, means the docks agree. The controller's `last_error` names the failed operation: the fault dialog shows it while
the fault is up, and a failure the screen is not showing as a fault (withdrawn, or left from
before this connection) toasts once as "Tool change failed: <last_error>", until the next
operation clears it. Irbis3D's `operation` opens a swap with `changing` before `dropping` and
`picking`; that frame lights the bar's first step.

### 1a. And which tools are in their docks

The same sensors answer a second question the tool number cannot: what is in each dock.
Upstream - the shape to design against - publishes it as `sensors`: `e` for the toolhead,
`t0`, `t1`, ... per dock, `1` meaning occupied. The fork flattens the same answer to
`tool<N>_docked` plus `head_loaded`.
`ToolReading::docks` carries both spellings, indexed by tool number.

A dock reading vacant for a tool that is not on the head means that hot end has been taken
out of the machine, which is why `refresh_slot_statuses_locked()` can now stamp
`SlotStatus::EMPTY` at all. The carriage slot always wins over its own dock reading: the
mounted tool's dock reads vacant because that is where the tool came from.

`docks` is merged across frames, never replaced. Moonraker republishes only the fields that
CHANGED, so a frame naming two docks says nothing about the third - a per-frame overwrite
would blank a tool on every partial update. Empty `docks` means "this frame said nothing",
which is not the same answer as "every dock is vacant".

### 2. A filament feeder

Only the hot end travels, so the filament is held by a servo gripper on the frame
(`[servo my_servo]`) that has to be released around a swap. There is no analogue in
klipper-toolchanger, because a toolhead changer carries its own extruder.

Surfaced as two `DeviceAction`s (`open_feeder` / `close_feeder`) in the AMS Device
Operations overlay. They are gated by
`check_preconditions(/*requires_toolhead_motion=*/true)`: the gripper is the only thing
holding the filament, so opening it mid-print drops the strand and kills the job, and the
overlay itself has no print gate.

## Two shipping configurations

One machine, two Python extras. `[medusahc]` lives outside the hardware repo
([Irbis3D/MedusaHC](https://github.com/Irbis3D/MedusaHC), which is `Macros/`, STLs and STEP
files) and there are two independent implementations of it. **Every MedusaHC runs one of
them** - there is no macros-only deployment, so `[medusahc]` is always present.

| | Source | Objects | Status schema | Feeder macros |
|---|---|---|---|---|
| **(a) Irbis3D Python controller** | [Irbis3D/MedusaHC-Python-Controller](https://github.com/Irbis3D/MedusaHC-Python-Controller) `klippy/extras/medusahc.py` | `[pin_watch io]`, `[medusahc]`; `[toolchanger]` + `[tool Tn]` only when klipper-toolchanger is installed (`sync_toolchanger: 1`), otherwise `T<n>` macros run `SET T=<n>` | `operation`, `current_tool`, `target_tool`, `last_error`, `feeder_open`, `layer`, `sensor_error`, `tool_count`, `sensors` | `MHC_OPEN` / `MHC_CLOSE` (native commands, absent from `printer.objects.list`), plus the `OPEN` / `CLOSE` macros HelixScreen sends |
| **(b) topi314's fork** | [topi314/MedusaHC](https://github.com/topi314/MedusaHC) `scripts/medusahc.py` | `[medusahc]` **alone** - drops both `[pin_watch]` and `[toolchanger]` | `state`, `current_tool`, `target_tool`, `tool_count`, `feeder_open`, `error` (bool), `layer`, `head_loaded`, flat `toolN_docked`, plus calibration fields | `OPEN` / `CLOSE` |

Object presence cannot separate the two - both register `[medusahc]` - so
`read_medusahc()` discriminates on **field names**, reading the Irbis3D names first. The
fork's names are a fallback, never a default, and that ordering is the rule everywhere in
this module: upstream first, fork second.

### `state` is not another spelling of `operation`

This is the trap, and it is worth stating flatly because the two keys look interchangeable
and are not. Read from the two sources, not inferred:

| | key | values |
|---|---|---|
| (a) Irbis3D | `operation` | `idle`, `changing` (MHC_SET homing before it reads the docks), `dropping`, `picking` |
| (b) topi314 | `state` | `uninitialized`, `ready`, `changing`, `error` |

Only `operation` names the swap **direction**. `state` is the same coarse resolution as
klipper-toolchanger's own `status`: a swap is one undifferentiated `changing`.
`ToolReading::phase_names_direction` records which key answered, because callers need to
tell them apart *before* a swap starts - the phase words themselves only appear once one is
already running.

`changing` must be handled explicitly. It matched none of the branches in
`apply_tool_sensor_locked()` until it was added, and since the fork also drops `[toolchanger]`
there was nothing else setting the action either: a swap on that fork ran with only
HelixScreen's own optimistic dispatch driving the UI.

**`feeder_open` is published by BOTH controllers** (`medusahc.py` in the fork,
`get_status()` in Irbis3D's), so the gripper phases are drivable on every MedusaHC.

The fork is the shape that used to break detection.
[topi314/MedusaHC](https://github.com/topi314/MedusaHC) absorbed the dock sensing into
medusahc.py itself - the switch pins moved to `dock_pin:` on each `[medusahc_tool N]`, and
klippy logs `medusahc: configured 7 switch pin(s): e=..., t0..t5` at startup - and dropped
klipper-toolchanger with it. `printer.objects.list` on that machine carries `medusahc` and
`medusahc_calibrate` and neither of the two objects `present()` requires. Its
`get_status()` is the fork schema `read_medusahc()` already falls back to, so detection was
the only thing in the way. `[pin_watch io]` is staying upstream, so a machine with only
`[medusahc]` stays the exception - but it is an exception someone is running today, and the
object alone is now enough to claim it.

The feeder buttons send the plain `OPEN` / `CLOSE` on every configuration. Irbis3D's
controller ships them as aliases forwarding to its `MHC_*` commands, the stock config and
topi314's fork register only those names, and they work on a parked tool. Irbis3D's
controller is the one HelixScreen builds against; the fork stays supported as a fallback.

## Loading and unloading filament

MedusaHC has one extruder drive serving every tool, so filament moves only through the tool
on the carriage. `toolchanger_addon::feeds_through_mount()` (the provider row's
`feeds_through_mount`) says so, `AmsBackendToolChanger::set_discovery()` sets
`mounts_to_feed_`, and the backend answers `head_filament_ops()` Automatic in both
directions with `needs_mount_to_feed()` true. To every surface it is a plain Load and
Unload; the mounting happens inside the operation.

`include/mount_feed_sequence.h` builds the stages; `AmsBackendToolChanger::start_mount_feed()`
sends them one at a time, each on the previous one's ack, as ONE `LOADING` or `UNLOADING`
operation whose step bar is the stage list:

| Load | Gcode | Unload | Gcode |
|------|-------|--------|-------|
| Insert filament | an action prompt: "Insert filament into the hot end of Tool N while it is docked, then tap Continue", with Continue and Cancel. It comes first, while the tool is still in its dock: once the hot end is on the carriage, the extruder's gears stand between the operator and the nozzle. A tool already on the carriage gets "Insert filament into Tool N, then tap Continue", and a Load of several with the first one mounted names both cases in one sentence | Mount tool | `SELECT_TOOL T=<n>` (or the machine's own `T<n>`); skipped when already mounted |
| Mount tool | same as Unload's; it closes the prompt | Heat nozzle | `SET_HEATER_TEMPERATURE HEATER=<tool's extruder> TARGET=<t>`, then `TEMPERATURE_WAIT SENSOR=<tool's extruder> MINIMUM=<t-2>` (`t` is the target slot's material temperature, else 220) |
| Heat nozzle | same as Unload's | Retract filament | `M83`, `G1 E-25 F1800`, `G1 E-150 F1200` (until the strand is fully out), `SET_HEATER_TEMPERATURE HEATER=<tool's extruder> TARGET=0` |
| Purge | `M83`, `G1 E5 F60` (slow draw), `G1 E50 F300` (tip to the nozzle), `G1 E<purge> F300` (the `purge_length` device action, 0-200mm, default 50, omitted at 0; persisted per printer as `toolchanger/purge_length_mm`), `G1 E-10 F1800`, `SET_HEATER_TEMPERATURE HEATER=<tool's extruder> TARGET=0` | Restore tool | remount the tool that was on the carriage, or park if none was |
| Restore tool | same as Unload's | | |

A tool already on the carriage skips Mount and Restore: Insert, Heat, Purge. A completed
Unload leaves the strand in the tube, so it ends on a success toast naming every tool it
unloaded: "Unload complete. Pull the filament out of Tool 2, Tool 4." (`toast_unload_complete()`).

Neither direction sends `OPEN`: the feeder buttons stay a separate control.

Every temperature command names the tool's own heater by name (`extruder` / `extruder<n>`).
The swap does not run `ACTIVATE_EXTRUDER`, so Klipper's active extruder stays where it was
and a bare `M109` or `M104` would act on that one; a printer's own `M109` override may also
wait on a different heater than it set.

Each stage goes out through `ensure_homed_then(..., bypass_busy_gate=true)`, which only this
run passes: every other backend send keeps IMoonrakerAPI's busy gate. A swap leaves
`idle_timeout` reading `Printing` for a while, and the gate sends a script made only of
heater commands (a cleanup with nothing to restore) fire-and-forget then, dropping its
answer: the run would wait forever. No stage carries a bare `G0`/`G1`, so skipping the
gate's move refusal changes nothing for the run (the purge and retract open with `M83`).
A stage that times out (`kAmsOperationTimeoutMs`) or errors unwinds the run like any
failed stage.

`TEMPERATURE_WAIT` holds Klipper's gcode queue until the temperature is reached. A Cancel
during Heat takes effect only after the wait ends: its cleanup queues behind it. If
another client zeroes that heater mid-wait, the run's 300 s timeout fires and the run
unwinds, but Klipper itself stays in the wait until `M112` or a restart.

**The dock sensors confirm every swap.** A Mount or planned Restore that acks is not done
until the sensors place the expected tool on the carriage (`-1` after a park). The answer
can lead the frame that reports the finished swap, so the run waits up to
`kSwapConfirmGraceMs` for that frame. A reading still `-2`, a fault the controller reports,
or a different tool stops the run WITHOUT a restore - the carriage is unknown, and no swap
goes out against it - with heaters off by name and the toast "Load stopped. Dock sensors
cannot tell which tool is mounted". Outside a swap, any sensor fault under a live run stops
it the same way, and once the run is over the fault shows as the backend's `ERROR`. A
cleanup counts as a swap only when it actually puts the carriage back. The operator
prompt tolerates a transient unreported `-2` too, since pushing a strand into a docked hot
end can jostle a dock switch; one that outlasts the prompt fails the Mount's confirmation.

Every other way a run stops - a failed stage, Cancel, a lost connection - asks
`carriage_trusted_for_restore()` before sending the restore: with dock sensors, a swap in
flight or waiting on its confirmation, or a fault they report, leaves the carriage
unknown, and the cleanup turns the heaters off without it. A machine with no dock sensors
always restores.

The prompt is Klipper's own action prompt, raised with `RESPOND TYPE=command` lines, so it
shows on this screen and on Mainsail/Fluidd alike, and it needs `[respond]`. Its payload is
**English whatever this screen's language is** - the RESPOND lines are console text every
client shows verbatim, so no locale of ours may decide what Mainsail displays. Its buttons
answer with `RESPOND TYPE=command MSG=HELIX_MOUNT_FEED_CONTINUE` / `..._CANCEL`; the backend
watches the console for that exact echo (`mount_feed::parse_console()`). The sidebar's Cancel
(`AmsBackendToolChanger::cancel()`), the prompt's Cancel, and a prompt **dismissed** anywhere
(the screen's own dismiss sends `action:prompt_end`, whose echo is the only trace) all turn
the rest of the run into the cleanup: prompt closed and, once anything past the prompt has
gone out, the plan's heaters turned off BY NAME (`SET_HEATER_TEMPERATURE ... TARGET=0`,
never a blanket `M104 S0` - the active extruder before the first mount is the PREVIOUS
tool's, and its user-set target must survive) and the carriage restored. A stop at the
prompt itself, before anything moved or heated, only closes the prompt. A stage that fails does the same; Klipper's own error line reports
the failure. A cleanup that cannot be sent at all ends the run and says
`Could not turn the heaters off`.

A lost connection or a klippy restart under a live run strands it: no ack can arrive for
the stage in flight. `abandon_feed_run_after_disconnect()` runs from both `on_started()`
(the reconnect) and `on_stopped()`: it sends the whole cleanup best effort and resets the
run without waiting for an ack. Both run without `mutex_` held, since the unwind takes it
and emits; `on_stopping()` runs under it.

**The wait has a ceiling.** Nobody inserting the filament for three minutes
(`mount_feed::kPromptTimeoutMs`) unwinds the run the same way and raises the error toast
"No filament was inserted in time": a run nobody is attending must not go on to mount and
heat a tool. A `LvglTimerGuard` watches the wait while LVGL is up; unit tests drive
`handle_wait_elapsed()` through `ToolChangerTestAccess` instead, and the swap confirmation's
grace through `handle_swap_check_elapsed()` the same way.

**Load all / Unload all.** `max_heads_per_filament_op()` answers the tool count, so the
sidebar's picker accepts several heads on this backend. A Load of several asks **once**, up
front, exactly as a Load of one does ("Insert filament into the hot ends of Tools 1, 2, 4
while they are docked, as far as it goes, then tap Continue"), then works tool by tool
(Mount, Heat, Purge): each tool's opening stage heats the **next** one to its full material
temperature (`SET_HEATER_TEMPERATURE HEATER=extruder<n>`, the Klipper multi-extruder
naming - `extruder` for tool 0), so its own heat stage has nothing left to wait for. A preheat
never LOWERS a target: a next tool already set at least that hot is left alone
(`HeadRequest::heater_target_c`, read off the live extruder targets at dispatch). An Unload of several
runs the same sequence with no prompt. At the end the carriage is restored to the tool the
run found on it (or parked). A stop mid-sequence - cancel, timeout, failed stage - turns
every heater in the plan off, restores the carriage, and toasts the tools that did complete
(`mount_feed::Run::completed_heads()`).

**The printer's filament macros are never called.** The feed and retract stages always send
the built-in moves, and a batch of every tool is the same sequenced run as any other. A
`LOAD_FILAMENT` / `UNLOAD_FILAMENT` / `LOAD_ALL_FILAMENT` / `UNLOAD_ALL_FILAMENT` on a
MedusaHC printer is the owner's own customization (stock MedusaHC defines none), with no
shared contract for its parameters or what it does around the moves.

While a run is active its stage owns the published action and step: the swap frames its
Mount and Restore stages raise (`operation`, the gripper) do not end it. The step bar is
the stage list, one stride per tool for a multi-tool run ("Tool 2 of 3: Purge"), built by
`get_operation_step_model()` from the live run; `Run::phase_index()` publishes the index
into it. The backend claims
`filament_ops_self_home`, so a load or unload is refused while a print is running or
paused, and `supports_auto_heat_on_load/unload`, so the UI does not preheat the active tool
before the backend mounts another.

`HELIX_MOCK_AMS=medusahc` runs all of it against mock hardware: the mock client echoes
`RESPOND` lines, so the prompt appears and its buttons drive the run. Tests:
`tests/unit/test_toolchanger_mount_feed.cpp`. **Unverified on hardware.**

## Overriding the feeder macros

The command names forked upstream, and a machine mid-migration can have either set,
both, or legacy aliases repointed at something custom. So the macro each button sends is
user-selectable, not hardcoded.

`AmsBackendToolChanger::get_device_actions()` adds two `DROPDOWN` actions
(`feeder_open_macro` / `feeder_close_macro`) whose options come from
`feeder_macro_candidates()`: the macros the printer actually reports, filtered to `MHC_*`
plus anything containing `OPEN`, `CLOSE`, `FEEDER` or `GRIP`. A raw macro list runs to
hundreds of entries and is unusable as a picker; a free-text field cannot be typo-checked.

The first option is the `auto` sentinel, which restores whatever detection chose (`OPEN` /
`CLOSE`).

`Feeder` carries all of it - `open_gcode` (what is actually sent), `detected_open` (what
detection chose, kept so `auto` can be restored), `open_choice` (the stored pick) and
`macro_options`.

Persisted per-printer via `SettingsManager::get_feeder_open_macro()` /
`set_feeder_open_macro()` (and the `close` pair), under
`wizard::FEEDER_OPEN_MACRO` / `FEEDER_CLOSE_MACRO`. Per-printer rather than global because
two MedusaHC machines on one network can be at different points in the migration.

## The operation step bar

The sidebar's step bar used to render the legacy Heat/Feed/Purge stepper on every tool
changer, because `AmsBackendToolChanger` overrode neither `toolchange_phase_template()` nor
`get_operation_step_model()` and fell through to the coarse `AmsAction` fallback. On a
hotend changer none of those three steps happen: nothing heats, no filament feeds and
nothing purges. It swaps a hot end.

The steps a machine gets are now derived from what it actually reports, by
`tc_step_sequence()` in `ams_backend_toolchanger.cpp`:

| reports | swap | fresh load | unload |
|---|---|---|---|
| `operation` + `feeder_open` (Irbis3D) | Release / Dock / Pick up / Grip | Release / Pick up / Grip | Release / Dock / Grip |
| `state` + `feeder_open` (topi314) | Release / Change tool / Grip | same | same |
| neither (plain klipper-toolchanger) | *no bar* | | |

Two rules produce all of it. The gripper brackets the operation, because a swap releases
the filament before anything moves and re-grips once the new hot end is seated. The middle
is the direction when the controller names it, and one undifferentiated "Change tool" when
it does not.

`tc_step_sequence()` is the single source for both the model and the phase-to-index
mapping, so a step added to it appears in the bar and becomes addressable at the same index
in one edit. That alone is not enough to keep the two in sync at runtime, though: the model
is built once, when the sidebar starts an operation, while the index is recomputed from
`tc_step_sequence()` again on every frame after that. Moonraker republishes only the fields
that changed, so the feeder/direction latches the sequence depends on can flip in between -
deriving both from the same function does not stop them from being derived from *different*
snapshots of it.

What actually prevents drift: `get_operation_step_model()` snapshots the latches it built the
model from, and `step_index_for_phase_locked()` resolves every frame's index against that same
snapshot rather than the live latches. Once a model exists, its index is pinned to the exact
sequence it was built from until the next model build refreshes the snapshot - the two cannot
drift apart in between.

**Mapping an idle frame.** Both ends of a swap read `idle`/`ready`, and the gripper is the
only thing separating them: open is the release that OPENS the operation, closed is the
grip that CLOSES it. A machine that does not report `feeder_open` cannot tell them apart
and gets no step for an idle frame rather than a guessed one.

That same ambiguity is why `apply_tool_sensor_locked()` does not report `AmsAction::IDLE`
for an idle-with-the-gripper-open frame while an operation is in flight. Doing so ended the
operation in the UI the instant it began - the bar was torn down on step 0 and the action
buttons came back mid-swap. It is gated on an operation actually running, because the user
can open the gripper by hand from the feeder panel while genuinely idle and that must not
read as busy.

**Suppression.** A machine with no phase source returns
`OperationStepModel{.suppressed = true}`, which is distinct from an empty model: empty means
"no model, use the legacy bar", suppressed means "no phases, and the legacy bar describes a
different kind of machine." The sidebar renders no step bar at all, and the status line
above it already names what the printer is doing. On MedusaHC that case never arises - both
controllers report phases - so in practice it is plain klipper-toolchanger, whose only
signal is `toolchanger.status == "changing"`.

**Which actions count.** The bar's visibility and its operation-start detection both ask
`AmsBackend::action_tracks_step_operation()`. They used to be two hardcoded action lists
that disagreed, and a tool changer fell through both: it reports `SELECTING` for a swap and
never heats, feeds or purges, so no start was detected, and once a bar did exist by accident
it vanished for the half of the swap spent in `SELECTING`. The default is the
filament-system vocabulary; `AmsBackendToolChanger` overrides it with
`SELECTING || UNLOADING`. Distinct from `ams_action_is_busy()`, the broader
"is anything happening" question the slot pulse asks.

## Detection

`toolchanger_addon::present(hw)` is `has_pin_watch() && has_tool_changer()`.

Both halves are required. `pin_watch` alone is just the extra; `[toolchanger]` alone is
any of the many klipper-toolchanger builds.

`has_medusahc()` is the third way in, and it needs no second half: nothing else in Klipper
registers `[medusahc]`. It is matched exactly, because `[medusahc_calibrate]` is a sibling
object and not this one.

That third path is a compatibility fallback, not a second mainline. It exists because
the fork has neither of the first two objects - and it is also what would catch upstream
if Sergei follows through on dropping the klipper-toolchanger dependency. Debug bundle
`6QWNVZY5` (HelixScreen 0.99.116, hostname `ducr10`, 123 objects: `medusahc`,
`medusahc_calibrate`, `gcode_macro T0..T5`, `extruder`..`extruder5`, no `pin_watch`, no
`toolchanger`) came in as `type: "Unknown"` with six plain extruders: `present()` was false,
`medusa_status_objects()` never subscribed, `PrinterDiscovery` registered no
`AmsType::TOOL_CHANGER`, and `ToolState::init_tools()` fell through to multi-extruder
enumeration - no docks, no feeder.

Two things follow from a changer with no klipper-toolchanger, and both live in this module
rather than in the backend. Neither is reached on the reference config, where
klipper-toolchanger supplies both:

- **No `[tool N]` objects to name the tools.** `PrinterDiscovery` enumerates the extruder
  heaters instead - one hot end per extruder is what a hotend changer is - and keeps the
  G-code tool numbers as the names, `T0`..`T5`. Real `tool N` objects always win: a
  klipper-toolchanger name is arbitrary and `ASSIGN_TOOL` can remap it.
- **No `SELECT_TOOL`.** `resolve_tool_commands()` answers with the machine's own commands
  (`T<n>` to mount, `DROP_TOOL` to unmount) and an absent capability when `[toolchanger]` is
  there to own the swap. `DROP_TOOL` is a bare registered command, not a `[gcode_macro]`, so
  unlike the feeder macros it cannot be capability-checked - naming it in the provider table
  is the point of the table.

**It deliberately does not touch `has_mmu_` / `mmu_type_`.** Those pick which AMS backend
gets built. An unguarded write there replaces a working backend on any printer that
happens to run `pin_watch`, and the same applies to detecting on `T<n>` macros - every
multi-tool Klipper setup has those. `tests/unit/test_medusahc_detection.cpp` carries the
regression matrix (Snapmaker U1, Happy Hare, AD5X IFS, QIDI Box, plain IDEX), each of
which was measured resolving to MedusaHC when the signal was wired as an MMU type.

`PrinterDiscovery` records only the plain object facts (`has_pin_watch()`,
`pin_watch_object_name()`). Deciding that pin_watch + toolchanger *means* MedusaHC is the
add-on module's business - see the vendor-abstraction rule in the root `CLAUDE.md`.

## Tool mapping

Which remap route a MedusaHC takes follows from the same fact as the tool names above:
whether `[toolchanger]` is there.

| Configuration | `[toolchanger]` | Route | How a remap lands |
|---------------|-----------------|-------|-------------------|
| (a) Irbis3D Python controller | Yes | `RemapStrategy::Native` | `ASSIGN_TOOL TOOL=T{n} N={tool}`; the file is untouched |
| (b) topi314's fork | No | `RemapStrategy::GcodeRewrite` | The job file is rewritten and printed through the HelixPrint plugin |

`AmsBackendToolChanger::get_remap_strategy()` asks `tool_commands_.present`, which
`resolve_tool_commands()` sets exactly when `[toolchanger]` is absent. The provider row
does not decide it: any changer without klipper-toolchanger rewrites.

On the rewrite route the file is the only thing that moves, so a start macro that takes
per-tool values (`EXTRUDER_TEMP`, `EXTRUDER1_TEMP`, ...) keeps heating the tools the slicer
chose while the rewritten body selects others. The rewrite handles `INITIAL_TOOL=` and
`TOOL=`, and the Filament Mapping dialog warns about the rest while the picks move a tool; see
[the remapper](FILAMENT_MANAGEMENT.md#gcode-tool-remapper). The recommended slicer start
G-code for these machines is in the user guide,
[Printers that map filament by rewriting the file](../user/guide/filament.md#printers-that-map-filament-by-rewriting-the-file).

`HELIX_MOCK_AMS=medusahc-fork` runs the rewrite route end to end in `--test`; set
`helix_plugin_installed` to `1` with `ctl set` to get past the plugin gate.

## Adding another hotend changer

Add one `Provider` row to the table in `toolchanger_addon.cpp`: a detection predicate,
which status objects to subscribe, and the feeder gcode. No call site changes.

## Printer database

Four entries in `assets/config/printer_database.json` - two frames, each with and without
the changer. Both frames share the same art (`duender.png`, `ducr10.png`), rendered from
the project images.

- `duender_medusahc` - `pin_watch` (92) and `medusahc` (92), hostname `medusa` (85),
  `toolchanger` corroborating at 20. `tool_count_4` and `kinematics_match corexy` are
  deliberately absent: they describe any 4-tool CoreXY toolchanger, and since
  `has_pattern()` is a case-insensitive substring match they would claim machines with no
  `pin_watch` at all.
- `duender` - hostname only. [Irbis3D/Duender](https://github.com/Irbis3D/Duender) is a
  mechanical Ender-3 to CoreXY conversion that ships **no Klipper config**, so a bare
  Duender has nothing in `printer.objects.list` distinguishing it from any other CoreXY.
  A `corexy` heuristic here would compete with the Voron and RatRig entries on every one
  of their machines. The wizard's manual-pick path covers it instead.
- `ducr10_medusahc` - the same MedusaHC signals on the DuCR10 frame (Andizzle's "DUO CR10"
  remix of the Duender, [Printables 1263532](https://www.printables.com/model/1263532-duender-remix-duo-cr10-corexy-conversion-gridfinit)).
- `ducr10` - hostname only, same reasoning as `duender`.

The bare-frame hostname deliberately does not appear on either MedusaHC entry, or the bare
entry could never outscore it (scoring is highest-match + 3 per extra match). The `ducr10`
hostname is the one exception: it sits at 40 on `ducr10_medusahc`, low enough to lose to
the bare `ducr10` entry (80) on a changer-less machine but still worth its +3 bonus on top
of a MedusaHC object match.

**The two MedusaHC entries cannot be told apart by hardware.** Same extra, same objects,
same schema - only the hostname says which frame it is, so each entry carries a
`hostname_exclude` for the other's name. A MedusaHC machine named neither is a genuine
tie, and whichever entry the detector reaches first wins; both name a MedusaHC CoreXY, so
the miss is the frame picture, not the capabilities.

## Known gaps

Not yet reconciled, and worth knowing before extending this:

(Per-tool spool metadata used to be listed here. It is now persisted - see
[FILAMENT_SLOT_METADATA.md](FILAMENT_SLOT_METADATA.md#tool-changer-is-the-odd-one-out).)

- **Tool offsets have two stores**, and upstream has now said which wins: `save_variables`
  (`t{N}_gcode_{x,y,z}_offset`) is the persistent source, loaded into `TOOL_OFFSET` at
  startup, and MedusaHC works off the runtime `TOOL_OFFSET` values. A temporary nudge means
  writing `TOOL_OFFSET`; a permanent one means writing both. klipper-toolchanger's own
  offset model is not the authority. The Z half is handled: the `TOOL_OFFSET macro` row in
  `src/printer/tool_offsets.cpp` reads `t{N}_off_z` and writes both stores. X and Y are
  currently **declined** (`supports_axis()` is false for them), because only the Z runtime
  variable names are confirmed against a machine — not because MedusaHC lacks the axes: the
  durable store already names `t{N}_gcode_{x,y}_offset`. Verifying the X/Y runtime names
  (presumably `t{N}_off_x` / `t{N}_off_y`) is all that `supports_axis_tool_offset_macro()`
  needs to widen; the rest of the row is already axis-agnostic. Until then the tool
  object's `gcode_x_offset` is not consulted on this printer either.
- **`layer`, `PRIME_FLAGS_*`, `MHC_CLEAN`** - per-tool priming and cleaning scheduled
  against layer number. No analogue in the toolchanger model, unused today.

The Python controller is marked experimental and is moving; prefer widening the provider
table over hardcoding against any single revision of it.

## What upstream has committed to (Sergei / Irbis3D, 2026-08-24)

Answers to the questions this document raised. Treat as intent, not contract.

- **`[pin_watch io]` is staying.** It remains the primary source for sensor states and for
  which tool is physically installed. The detection predicate is safe upstream; forks are
  the exposure.
- **`operation`, `current_tool`, `sensor_error`, `tool_count` are stable** and will not be
  renamed without notice. There is no schema version field today; he intends to add one,
  plus compatibility flags, so external interfaces can adapt.
- **The `sensors` dict is per-dock, and we should be using it.** `e` is the toolhead
  sensor, `t0`, `t1`, ... the docks. `1` means occupied: `e: 1` is a tool on the head,
  `tN: 1` is tool N seated in its dock. Example: `{"e":1,"t5":1,"t4":1,"t3":0,"t2":1,
  "t1":1,"t0":1}` is T3 on the head, everything else docked. That is enough to show real
  per-dock state instead of only the mounted tool number.
- **`verify_tool_pickup: False` is intentional.** klipper-toolchanger is there for offset
  calibration and for syncing the active tool into Mainsail/Fluidd; MedusaHC performs the
  swap and the sensors through pin_watch are the physical truth. Read
  `medusahc.current_tool` and `medusahc.sensors`, never `toolchanger.tool_number` - which
  is what `apply_tool_sensor_locked()` already does.
- **The klipper-toolchanger dependency is meant to go away** eventually, so MedusaHC runs
  standalone. No major behaviour changes are planned in the meantime, and he will give
  advance notice.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the
shared architecture and [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md)
for the backend that drives it.
