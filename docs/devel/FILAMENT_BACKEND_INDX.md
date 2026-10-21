# Bondtech INDX Filament Backend

Bondtech INDX is a tool changer built around one **Smart Head** on the gantry and up to ten
**passive tools** parked in a dock (prestonbrown/helixscreen#1333). A passive tool is a
nozzle and a filament path, nothing else: the Smart Head carries the only extruder (the DX
drive), the only heater (an induction coil read by an IR sensor) and a load cell. Each tool
carries its own filament from its own spool, and no purge is needed between tools.

To the UI it is a tool changer: `AmsType::INDX`, `is_tool_changer()` true, one slot per
tool, a change is a swap, each tool owns its filament and spool. To Klipper it is a printer
with one `[extruder]`: `heater_pin: indx:heater`, `sensor_type: indx`.

| Piece | Where |
|---|---|
| Macro contract (what is read and sent) | `include/indx_contract.h`, `src/printer/indx_contract.cpp` |
| Backend | `include/ams_backend_indx.h`, `src/printer/ams_backend_indx.cpp` (derives `AmsBackendToolChanger`) |
| Detection | `src/printer/printer_discovery_parse.cpp#PrinterDiscovery/parse_objects`, `#PrinterDiscovery/parse_config_keys` |
| Shared heater in ToolState | `ToolTopology::heads_share_heater`, `include/tool_state.h#ToolState/names_mounted_nozzle` |
| Heat gate | `AmsBackend::nozzle_heat_blocked_reason()`, enforced in `src/ui/temperature_controller.cpp#TemperatureController/set_target` |
| Mock | `HELIX_MOCK_AMS=indx`, `src/api/moonraker_client_mock_indx.cpp` |

## The macro contract

INDX publishes no tool-changer status object. Tool state lives in `save_variables` and in
variables of Bondtech's upstream macros ([BondtechAB/INDX](https://github.com/BondtechAB/INDX)
`macros/indx.cfg`, `macros/indx-tc-macros.cfg`, `macros/indx-cal.cfg`). Those macros are the
contract. Nothing outside these two tables is read or sent.

### Read

| What | Where | Meaning |
|---|---|---|
| Tool on the head | `save_variables.variables.active_tool` | `0..N-1`, `-1` empty head. Absent until `INIT_STATE` runs: no news, not "empty" |
| Half-seated fault | `save_variables.variables.seat_suspect` | `>= 0` names a tool that may be half on the head; changes, parks and `G28 Z` refuse until `MANUAL_TOOL_REMOVE` / `MANUAL_TOOL_SEAT` clear it |
| Toolchange count | `save_variables.variables.toolchange_count` | Diagnostic |
| Latch, speed mode | `gcode_macro _TOOLCHANGE_VARS.toolhead_state` / `.mode` | `0` locked, `2` open; diagnostic |
| Tool count | `gcode_macro TOOL_POSITIONS.tool_count` | Slot count. Discovery also reads `variable_tool_count` from `configfile.config` so the slots exist before the first frame |
| Tools with no hotend | `gcode_macro TOOL_POSITIONS.no_heat_tools` | A pen or cutter: never heated, no filament ops |
| Docks measured | `gcode_macro TOOL_POSITIONS.docks_calibrated` | `False` refuses every change and park |
| Load presets | `gcode_macro LOAD_FILAMENT.filament_presets` | `TYPE=` names for Load |
| Per-tool spool | `gcode_macro T<n>.spool_id` | Not upstream: read where a macro set declares it, filed as the lane's Spoolman id. Never written |

Status keys are matched case-insensitively and subscribed under the configured case
(`PrinterDiscovery::macro_config_name`).

### Send

| Command | Used for |
|---|---|
| `T<n>` when `gcode_macro T<n>` exists, else `CHANGE_TOOL TOOL=<n>` | Change tool. `T<n>` runs a user's wrapper exactly as a slicer change does. The macro homes an unhomed printer, turns the coil off before docking and `M109`s the new tool to the outgoing target |
| `PARK_TOOL` | Park, leaving the head empty |
| `LOAD_FILAMENT TOOL=<n> TYPE=<preset>`, or `TEMP=<c>` when the material names no preset | Load: picks the tool up, sets the heater's filament model, heats, feeds and primes. Leaves the tool mounted and hot |

### Recognized, or not

Required in `printer.objects.list`: `save_variables`, `gcode_macro TOOL_POSITIONS`,
`gcode_macro _TOOLCHANGE_VARS`, `gcode_macro CHANGE_TOOL`, `gcode_macro PARK_TOOL`. With any
missing, the backend still registers but reports **INDX macros not recognized**: no slots, a
`current_error()` naming what is missing, every operation refused, nothing sent.
`LOAD_FILAMENT` is optional; without it Load is Manual.

The #1333 reporter's macros also define `INDX_LOCK`, `INDX_UNLOCK`, `INDX_FORCE_STATE`,
`VERIFY_TOOL_PRESENT` and `T<n>.spool_id`. None of them exists in the upstream repository,
so none is used. `T<n>.active` (for Mainsail/Fluidd's tool buttons) is recent upstream and
missing on older installs, so `active_tool` is the only source of the mounted tool.

## Detection

Either signal registers `AmsType::INDX`:

- `indx` in `printer.objects.list` (the mainline `indx_klipper` module has a `get_status`);
- an `indx` section in `configfile.config` (Kalico's module may not list as an object).
  `parse_config_keys()` runs after `parse_objects()` registered the filament systems, so it
  registers them again when it is the one that finds INDX.

The INDX arm sits after Snapmaker and before the generic tool-changer arm; a real MMU still
wins. Macros alone never detect INDX.

## One heater, several nozzles

Every tool prints through the one `extruder`, so the backend answers
`heads_share_heater()` true and `build_ams_topology()` carries it into `ToolState`:

- `physical_heads()` is one head per tool (the tool switcher and printer chips see six), while
  `heated_heads()` is one entry, standing for the mounted tool. `extruder_count()` is 1.
- `names_mounted_nozzle()` is true, so the label is the tool changer one: "Nozzle 3" while T2
  is mounted (or the tool's own name), plain "Nozzle" with the head empty. It drives
  `nozzle_label()`, the nozzle icon badge, the print status tool label and the nozzle temps
  widget's row name. The print status nozzle picker stays on `has_multiple_extruders()`:
  one heater, nothing to pin. The temp graph series stays "Nozzle": it is the heater's
  history across every change.
- `nozzle_heat_blocked_reason()` is "No tool on the head" or "Tool 4 (T3) has no hotend".
  HelixScreen heats with `SET_HEATER_TEMPERATURE`, which bypasses INDX's own `M104` guard,
  so `TemperatureController::set_target()` refuses a nozzle target above 0 while it is set
  and toasts the reason. `ams_nozzle_heat_blocked` dims the nozzle presets and Custom button
  (`styles.heat_blocked`); a tap still reaches the controller, which says why.
- `slot_for_extruder()` answers nothing, so filament usage is charged to the mounted slot.
- A change of mounted tool clears the swap preheat latch: the next nozzle is a different melt
  zone, so the previous tool's temperature has nothing to purge.

## Filament operations

| Op | Support |
|---|---|
| Load | Automatic through `LOAD_FILAMENT` (the backend auto-heats, the UI never preheats). Manual without the macro; Unavailable on a `no_heat_tools` tool |
| Unload | Manual: "Pick up the tool, heat it, and pull the filament out". Upstream has no unload macro |
| Change, park | `T<n>` / `CHANGE_TOOL`, `PARK_TOOL`; refused on an unrecognized contract, a half-seated tool or uncalibrated docks |

A half-seated tool shows as an error with the macro's own instructions and no recovery
buttons: `MANUAL_TOOL_REMOVE` opens the latch, so a tool can drop.

## Spoolman

`manages_active_spool()` is true when a `T<n>` or `CHANGE_TOOL` template calls
`SET_ACTIVE_SPOOL` or `spoolman_set_active_spool` (`CachedMacroInfo::sets_active_spool`).
Stock Bondtech macros do not, so HelixScreen posts the mounted tool's spool on each mount;
a macro set that does keeps it.

## Mock

`HELIX_MOCK_AMS=indx` is mock hardware with the real backend (it implies `--real-ams`). The
mock publishes the objects and macro state above, walks a change through park and pickup over
a few notifications, and refuses heat with no tool on the head or on a no-hotend tool, so a
missing gate shows as a target that never moves. Knobs in
`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.

## Not yet hardware-verified

No INDX is in the test fleet; verification goes through the #1333 reporter. Open: whether
Kalico's `[indx]` lists as an object, how the reporter's `spool_id` macros persist a spool,
and the timing of the change ack against the `active_tool` frame.

Part of the filament system: see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) and the
base this backend extends, [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md).
