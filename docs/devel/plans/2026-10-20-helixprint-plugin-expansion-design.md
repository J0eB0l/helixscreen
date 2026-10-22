# HelixPrint Plugin Expansion - Design

Status: draft for review (2026-10-20). Delete this file in the change that ships the last phase.

## Why

The HelixPrint Moonraker plugin (`moonraker-plugin/`) does three things today: `print_modified`
(original-filename attribution for rewritten G-code), `.gcode.3mf` conversion, and `status`. It
runs beside Moonraker, usually on hardware stronger than the screen, and it sees every job no
matter which client started it. This design uses those two properties for a set of features the
app cannot do well or at all on its own.

Rulings already made:

- One umbrella design: a capability framework (Phase 0), then features as phases that ship on
  their own.
- Every server-side action on a print the screen did not start is **opt-in per feature**.
- **Every feature attempts an app-side fallback.** Where a fallback is weak or impossible, the
  phase says so under "Fallback" and lists it in § Open questions, to revisit the plugin-required
  question feature by feature.

## Non-goals

What Moonraker already does stays with Moonraker: database storage of app settings (the app
already uses the `helix-screen` namespace), phone notifications (`[notifier]`), timelapse, zeroconf.
No feature moves the machine on its own: the plugin never sends motion, heat or print-control
gcode except where a phase says so and the user opted in.

---

## Phase 0 - Capability framework (prerequisite for every phase)

### Today

- App detection: `src/application/discovery_steps_core.cpp#helix_plugin_check_step` calls
  `server.helix.status`, reading `enabled`, `version`, `symlink_dir` into
  `include/moonraker_types.h#HelixPluginInfo` and `include/printer_plugin_status_state.h#PrinterPluginStatusState`.
- "Outdated" is string inequality against the bundled version
  (`src/system/helix_plugin_installer.cpp#plugin_versions_differ`), which drives
  `src/ui/plugin_self_heal.cpp#PluginSelfHeal`.
- The app reads no plugin capabilities; `convert_3mf` / `start_3mf_on_upload` in status are unused.
- Version lives in `helix_print.py#PLUGIN_VERSION` (1.2.0), bumped by hand.
- Optional modules load through `helix_print.py#_load_sibling`; a missing or broken sibling
  disables that feature only. Vendor knowledge lives in `helix_capabilities.py` probes.

### Design

**Plugin: one module per feature.** Each phase ships as `helix_<feature>.py`, loaded with
`_load_sibling`, registering its own endpoints and event handlers. A module that fails to import
or start disables only itself and reports why.

**Status grows a `features` map** (additive; old app builds ignore it):

```json
"api": 2,
"features": {
  "file_analysis": {"version": 1, "available": true, "enabled": true, "reason": null},
  "print_hooks":   {"version": 1, "available": true, "enabled": false, "reason": null},
  "event_journal": {"version": 1, "available": false, "enabled": false, "reason": "moonraker<0.9"}
}
```

- `available`: the module loaded and its requirements (Moonraker version, components) hold.
- `enabled`: the user's opt-in, read from settings (below). Ruled 2026-10-20: features that only
  observe (file analysis, storage report, event journal, photos) default on; anything that acts on
  a print or a spool defaults off.
- `version`: per-feature contract version, bumped when its endpoint shape changes.
- `api`: the shape of the status reply itself; 2 is the first with `features`. A plugin without
  `api` predates the map.
- A feature that is not available always reports `enabled: false`.

**Opt-in lives in the app's settings namespace.** The plugin reads `helix-screen` /
`plugin_features` (`{"print_hooks": true, ...}`) from Moonraker's database in-process, at the
moment it acts (cheap, and no change notification is needed). The app's settings UI writes it.
One store, so every screen on the printer agrees.

**One switch per feature (2026-10-21).** A feature may own its switch instead, when its on/off
depends on more than a boolean (power-loss recovery: `helix-screen` / `plr_user.enabled`, with a
storage-dependent default). Its `plugin_features` key is then unused, and the `features` map
reports that feature's own `enabled`. Never two switches for one feature.

**Helix-initiated job marker (2026-10-21).** A plugin path that starts a job itself records it
immediately before the single command that starts the print (`SDCARD_PRINT_FILE`, or `M24`),
never before a script that homes, heats or purges first: `mark_helix_initiated(filename, kind)`,
withdrawn with `unmark_helix_initiated(filename)` when that command fails, with `kind` one of
`"plr_resume"`, `"print_modified"`, `"print_edited"`. The plugin binds the marker to the history
job id when history announces the job: the `history:history_changed` event with action `added`,
which carries filename and job id. `job_state` events carry no job id, so the binding cannot
happen on the `job_state` start, and history's `added` handler can run after the plugin's own
`job_state` handlers. Binding stores namespace `helix_jobs`, key = job id, value
`{"kind": ..., "filenames": [...], "start_time": ..., "bound_at": <unix time>}` (pruned after 30
days, on each bind and at startup); an unmatched marker expires after 60 s. A start through a link
passes the original name too (`mark_helix_initiated(link, kind, history_name)`), since
`_patch_history_entry` renames the finished entry to it; both names answer.
`helix_initiated(job_id, filename, start_time) -> Optional[str]`, with the job's values as history
reports them, answers for every module. History reuses job ids (the SQL table has no
AUTOINCREMENT; the older LMDB history resets its counter on delete-all), and neither mainline nor
QIDI Moonraker announces a deleted job, so a binding answers only for its job's `start_time` and
names, and an `added` event with no matching marker drops any binding its id had. The in-memory
binding is made before the plugin's handler first awaits, and helix_print registers its history
handler before any feature module, so a module's own handler of the same `added` event sees it. Start-of-job features
(Phases 3, 5, 7) check it, so a resumed job does not re-run start hooks partway through a part,
and therefore **act from history's `added` event (or after it), never from the `job_state`
start**: there, `helix_initiated()` can still be None for a job the plugin started.

**Owner (2026-10-21):** Phase 0 is implemented as the first task of power-loss recovery v1, by the
session that owns that work.

**App: gate on features, not version equality.** `PrinterPluginStatusState` gains the parsed
feature map and one query, `plugin_feature(name, min_version) -> {absent, disabled, ready}`.
Callers ask that, never the version string. `ready`: available, `version >= min_version`, switched
on. `disabled`: available at that version but switched off. `absent`: everything else, i.e. no
plugin, a plugin without the map, no such entry, a contract below `min_version`, or
`available: false`. A feature that cannot serve the caller's contract is never `disabled`. Version mismatch keeps driving self-heal, but a
mismatched plugin still serves whatever features it reports, so an older plugin degrades by
feature instead of all at once.

**Fallback rule.** Each consumer decides between plugin path and fallback in one place, as a
pure function of `plugin_feature(...)` and its own inputs, so the choice is unit-testable without
Moonraker.

**Compatibility floor** (fleet survey 2026-10-20, `machine.system_info` + `server.info`):

| Device | Python | Moonraker |
|---|---|---|
| K1C (Buildroot 2020.02) | 3.8.2 | v0.9.3 |
| K2 Plus (OpenWrt 21.02) | 3.9.12 | API 1.4.0, no version string |
| QIDI Q2 (Debian 11) | 3.9.2 | API 1.4.0 |
| Voron V0 (Debian 11) | 3.9.2 | v0.11.0 |
| SonicPad (Debian 11) | 3.9.2 | v0.10.0 |
| AD5M (Forge-X 1.4.2) | 3.11.10 | API 1.4.0 |
| U1 (Buildroot 2024.02) | 3.11.8 | "1.6.0" (vendor string) |
| CC1 (OpenCentauri) | 3.12.12 | API 1.5.0 |
| AD5X (Buildroot 2025.02) | 3.12.10 | API 1.4.0 |
| CB1 Voron (Armbian trixie) | 3.13.5 | v0.10.0 |

- **Python 3.8 to 3.13.** Keep `from __future__ import annotations` and `typing` generics; no
  `match`, no `str.removeprefix`, no dict `|`, no `zoneinfo`, no other 3.9+ stdlib API. Avoid
  asyncio APIs deprecated by 3.12/3.13 (implicit `get_event_loop`). CI runs the plugin tests on
  3.11 only today: add a 3.8 job.
- **Five of ten report Moonraker API 1.4.0**, below the 0.9 release that history
  `auxiliary_data` / `save_job` needs. Every feature that records into history (Phases 5 and 7)
  must also keep its record in its own namespace keyed by job id, and treat `auxiliary_data` as a
  bonus where present. Vendor forks also report no `moonraker_version`, so gate on
  `api_version` and on probing the call, never on the version string.

**Housekeeping in this phase:** fix the stale README (`modified_content`, the `1.0.1` status
example); document the `features` map there.

### Tests

- pytest: status reports each module's `available` / `enabled` / `reason`; a sibling that raises
  on import shows `available: false` with a reason and leaves the others working; opt-in read
  from a mocked database namespace, absent key = default.
- C++: `plugin_feature()` over absent plugin / old plugin without `features` / disabled / ready /
  version below minimum. Every later phase's decision function is tested against all four states.

---

## Phase 1 - File analysis on upload

A shared analyzer several later phases read from.

### Today

- Preview: `docs/devel/architecture/16-gcode-pipeline.md`. The app downloads the file and builds
  either a full `GCodeParser` result or a streaming `GCodeLayerIndex`; nothing persists, so every
  load re-parses. A 130 MB file took a K2 Plus to about 387 MB RSS on full parse. ESP32 K-Touch
  compiles the viewer out.
- Per-tool facts: `src/printer/print_start_inputs.cpp#FileStartFactsLoader::load` from metadata,
  a footer tail read (`gcode::parse_gcode_footer_summary`, the only per-tool grams with a certain
  unit), and a whole-file Tn scan; `ToolsUsedCache` keeps only the tool set.
- Thumbnails: metadata, else first-100 KB extraction (`helix::fetch_thumbnail_from_gcode`, off on
  ESP32), plus `.3mf` plate PNG ranged reads. Metascan is single-flight per file.
- Objects: without `EXCLUDE_OBJECT_DEFINE` there are none (`pre_start_exclude.cpp#merge_defined_objects`);
  `; printing object`, `M486` and `preprocess_cancellation` output are not read.

### Design

`helix_analysis.py` hooks `file_manager:filelist_changed` (the 3mf converter's pattern, including
its queue and backfill) and, per gcode file, writes an analysis record keyed by path + size + mtime:

- `tools`: per tool, grams and mm, color, material (footer, else metadata, else Tn scan).
- `bounds`: min/max XYZ of printing moves, max travel extents, max hotend and bed temps commanded.
- `header`: slicer printer model, nozzle diameter(s), bed size if declared, filament types.
- `objects`: from `EXCLUDE_OBJECT_DEFINE`, else `; printing object` / `M486` labels with
  computed polygons.
- `layer_index`: byte offset, Z, tool set and the modal state at each layer start (positioning
  and extrusion modes, logical E, F, active tool, factors, retract state) as a compact sidecar
  file under a dot directory. The modal state is what lets power-loss recovery's
  `scan_to_offset(start_offset=..., start_state=...)` begin at a layer start instead of byte 0.

**One shared G-code reader (2026-10-21).** Analysis reads files through
`helix_gcode_reader.py`, the module power-loss recovery builds: tokenizer, layer markers,
`EXCLUDE_OBJECT_DEFINE`, `T<n>`, the modal-state reducer and the throttle policy. No second parser.
- `thumbnail`: generated or extracted when metadata has none, published the way Moonraker
  publishes `.thumbs` so existing clients benefit.

Endpoint: `server.helix.analysis {filename}` returns the record or `pending`. A notification
`helix:analysis_ready {filename}` lets the detail view refresh.

Cost guard: the throttle policy in `helix_gcode_reader.py`, shared with power-loss recovery.
One file at a time, niced. While a print runs, whole-file analysis runs only on a multi-core host
(probe). The one exception is the printing file's own layer index when it does not exist yet:
power-loss recovery's follower builds it on any host, reading no further than
`virtual_sdcard.file_position` (bytes Klipper has just read, so already in page cache) at a
capped rate. It writes the same sidecar, so it is the layer index, not a second one.

### App integration

`FileStartFactsLoader` takes the analysis record as its first source when ready.
`GCodeLayerIndex` gains a constructor from the sidecar, so streaming preview skips its index pass.
Thumbnails and objects flow through their existing paths.

### Fallback

Linux screens: today's behaviour. **K-Touch: plugin-only (ruled 2026-10-20).** The ESP32 build has
no viewer and no whole-file parse, so thumbnails for thumbless files, objects without DEFINE
lines and per-tool amounts reach it only through the plugin. Nothing regresses: it has none of
them today. This is the design's one plugin-required case.

### Tests

pytest over fixture files: Orca, PrusaSlicer, Cura, a `.3mf`-converted output, a file with
`M486`, a file with no DEFINE lines; record invalidated on mtime change; analysis paused while
printing. C++: loader prefers the record, falls back per field when a field is missing.

---

## Phase 2 - File-fits-printer check

### Today

`src/printer/preflight_validator.cpp#PreflightValidator::validate` checks tool-to-slot mapping
only; `src/printer/print_start_checks.cpp#default_print_start_gates` adds lane weight, bypass,
material and stopping-command gates. Nothing compares the file with the machine; the app does not
read nozzle diameter or printer model from metadata.

### Design

A pure checker (plugin side in `helix_analysis.py`, consuming Phase 1 `bounds` and `header`)
compares against `configfile.settings`: `stepper_[xyz]` position_min/max, `extruder*` and
`heater_bed` max_temp, tool count, nozzle diameter. Output is a list of findings
`{kind, severity, detail}`: `out_of_bounds` (block), `temp_over_max` (block),
`too_many_tools` (block), `nozzle_mismatch` (warn), `printer_model_mismatch` (warn).

Findings become gates in `default_print_start_gates`, shown in the detail view beside existing
preflight. Opt-in `fit_check_blocks_autostart`: the plugin refuses an Orca `print=true` start
(extending `helix_3mf.py#_finalize_upload_hook` to plain gcode) when a blocking finding exists,
and the file stays uploaded.

### Fallback

The app runs the same checker over what it reads itself (ruled 2026-10-20): metadata plus the 200
KB header scan give printer model, nozzle diameter and temps. Bounds come from the preview's
`LayerIndexStats` when a preview has loaded, else from one streaming pass over the file at print
start (constant memory, one sequential read). The checker is one function fed by either source.
The K-Touch gets the header checks only.

### Tests

Checker table tests per finding kind; boundary values (exactly at position_max passes; one step
over fails); delta and CoreXY printers whose limits read differently; the gate surfaces in the
detail view.

---

## Phase 3 - Print-start hooks for prints started anywhere

### Today

Options applied only when the screen starts the print (`PrintPreparationManager::start_print`):
pre-print skips (file rewrite and skip params), the pre-start gcode block, per-print timelapse,
AMS pre-print gcode (`ui_print_start_controller.cpp`), and U1 end-unload
(`AmsBackendSnapmaker::end_unload_gcode`; firmware resets it after every print). Queued-job
options apply only on the app's "Start next". Per-spool PA already works everywhere.

### Design

The app writes a **standing options record** to `helix-screen` / `standing_print_options`: the
subset that makes sense for any print (U1 end-unload tools, timelapse default, AMS pre-print
routing defaults). The plugin's `helix_print_hooks.py` watches `job_state` and, at the start of a
job not started through `print_modified`, sends the gcode those options require, through the same
generators the app uses. A job with a helix-initiated marker (Phase 0) of kind `plr_resume` gets no
hooks: U1 end-unload, AMS pre-print gcode and timelapse setup already ran when the part started.
The hook decision runs from history's `added` event for the job, not the `job_state` start, since
only from then does `helix_initiated()` answer for it (Phase 0).

**DRY constraint (ruled 2026-10-20): the app renders the gcode.** The record holds finished gcode
produced by the same functions the screen's own start uses; the plugin only sends it. It is
current as of the app's last write, so the app rewrites it whenever an input changes (setting
edit, lane change, backend reconnect). Never reimplement a backend's gcode in Python.

**Exception (ruled 2026-10-21): power-loss recovery renders its resume gcode in the plugin.** The
resume script is built from a G-code file scan that runs in the plugin, the K-Touch cannot scan
files, and one endpoint serves every client; no vendor backend's gcode is duplicated. Details and
the reasoning: `docs/devel/plans/2026-10-21-plugin-power-loss-recovery-design.md` § 3.2.

Skips and file rewrites are excluded: once a foreign client has started the file, it is too late
to rewrite it. Moonraker's own queue transitions are in scope.

### Fallback

The app observes a job start it did not initiate and sends the same rendered record itself. Works
whenever the screen is running and connected (the normal case on a device screen); misses starts
while the app is down. That gap is the plugin's value.

### Tests

pytest: hook fires once per job start; not for `print_modified` jobs; not when opt-in is off;
record absent = no-op. C++: "foreign start" detection; the rendered record matches what the
screen's own start sends (same function produces both).

---

## Phase 4 - Server-side G-code edits

### Today

`PrintPreparationManager::rewrite_and_print` downloads the full file to the screen's disk, rewrites
it line by line (`src/rendering/gcode_file_modifier.cpp`: comment out, macro-param rewrite,
`PRINT_START` skip params; tool remap via `GcodeToolRemapper::apply_to_file`), uploads the copy,
and calls `server.helix.print_modified` with its path. Memory is one line; transfer is the whole
file twice and local disk is about two copies.

### Design

`server.helix.print_edited {original_filename, edits[], modifications[]}` where each edit is the
same `{line_number, expect_raw_line, action, args}` the app's modifier consumes. The plugin applies
them disk to disk with the same refuse-if-the-line-differs rule (`apply_streaming`), then follows
the existing `print_modified` path. Tool remap is an edit kind with a map argument.

**DRY constraint:** two implementations of one edit grammar. Pin them with shared golden fixtures:
the same input file + edit list must produce byte-identical output in C++ and Python; one fixture
directory both suites read.

### Fallback

Today's download-rewrite-upload path, unchanged.

### Tests

Golden fixtures run by both suites; mismatch on `expect_raw_line` refuses the whole edit; CRLF
and no-trailing-newline files.

---

## Phase 5 - Filament accounting per lane and tool

### Today

The app never deducts from Spoolman; Moonraker's spoolman component deducts from one active spool.
Active spool follows the loaded lane (`AmsState::sync_current_loaded`), unless the backend
`manages_active_spool()` (AFC always; HH when spoolman mode is on). `FilamentConsumptionTracker`
observes per-extruder `filament_used` and persists grams to `lane_data` only for lanes with no
Spoolman link. No history record of which spools a print used.

### Who deducts today (source survey 2026-10-20)

Moonraker's `spoolman.py` is the only deductor anywhere. It tracks ONE `spool_id` and charges it
the rise in `toolhead.position[3]` above a high-water mark (retracts are not counted twice); a
change of `toolhead.extruder` or of the active spool resets the baseline without charging. So
usage is right exactly when the active spool is switched at every tool or lane change.

| System | Sets Moonraker's active spool | Effect |
|---|---|---|
| AFC | yes, on lane load (`AFC_spool.py#set_active_spool`) | correct; never writes `/use` itself |
| Happy Hare v3/v4, spoolman mode not off | yes (`_spoolman_activate_spool`) | correct |
| Happy Hare, mode off | no | nothing deducted |
| U1 with paxx SpoolLink | yes, on extruder change | correct |
| U1 stock, CFS, ACE (Bunny/Duck/ValgACE), toolchanger macros without SET_ACTIVE_SPOOL | no | nothing, or every tool charged to one spool |
| Bondtech INDX (`BondtechAB/INDX` `macros/indx-tc-macros.cfg`) | not in the stock macros; some users add it to their `T<n>` wrappers | varies by config |

### Design

**Never deduct; drive the active spool.** `helix_filament.py` does what AFC and SpoolLink do, for
the backends that do not: on each `toolhead.extruder` change and each lane load mid-print it calls
Moonraker's `spoolman` set-active-spool with the spool linked to the now-feeding lane or tool. Moonraker
stays the single deductor, so double counting is impossible by construction.

- The lane/tool-to-spool map comes from a `helix-screen` record the app keeps current (the same
  data `AmsState` already holds); the plugin does not learn backends.
- The plugin acts only when the backend reports `manages_active_spool() == false`, written into
  that record by the app. One pure function decides it; plugin and fallback share the rule.
  Macro-based changers (INDX, klipper-toolchanger configs) are decided per printer: if the
  `T<n>` / change macros in `configfile` already call `SET_ACTIVE_SPOOL`, stay out.
- INDX has one extruder and docked nozzles, so `toolhead.extruder` never changes; its "now
  feeding" signal is `save_variables.variables.active_tool`.
- Lane changes within one extruder (CFS, ACE) need the backend's "now feeding lane N" signal; the
  record names the status object and field per backend, so the plugin stays vendor-free.
- At job end the plugin writes `{tool, lane, spool_id, grams}` per spool into its own namespace
  keyed by job id (computed from the same position deltas), and into `auxiliary_data` where the
  Moonraker supports it. History detail shows it.
- A job marked `plr_resume` (Phase 0 marker) continues the interrupted job's per-spool record
  instead of opening a new one. The per-job record opens on history's `added` event, when the
  marker is bound, not on the `job_state` start.

### Fallback

The app already sets the active spool on lane load (`AmsState::sync_current_loaded`) while it runs.
Extend that to `toolhead.extruder` changes for toolchangers. It misses changes while the app is
down; the plugin closes that gap.

### Tests

Ownership table (backend x spoolman mode -> plugin acts or stays out); two-tool print switches the
active spool at each tool change and never otherwise; an AFC or HH-push printer sees zero calls
from the plugin; per-job record sums match the position deltas; reconnect mid-print does not
switch to a stale spool.

## Phase 6 - Printer event journal

### Today

Klippy disconnect/shutdown route to the recovery dialog (`moonraker_event_routing.cpp#decide_moonraker_event`);
`!!` lines through `GcodeErrorRouter`, replayed from `server.gcode_store` on reconnect.
`ui_notification_history.cpp` is an in-memory ring of 100. Only crashes persist
(`config/crash_history.json`). The debug bundle collects log tails over HTTP Range with app-side
redaction (`DebugBundleCollector::collect`).

### Design

`helix_journal.py` records, with timestamp and active job: klippy shutdown + `state_message`,
klippy disconnect/ready, MCU disconnect, firmware restart, pause + cause, runout sensor trips,
`!!` lines (classified), job end with status. Bounded store (count and age) in a Moonraker
namespace. Endpoints: `server.helix.journal {since, limit}`; notification on append.

App: a "Printer events" view, and the debug bundle includes the journal.

### Fallback

The app persists the events it sees, and on every reconnect backfills the gap from what Moonraker
retains (ruled 2026-10-20): `server.gcode_store` for `!!` lines, history for how jobs ended, and the
klippy.log tail (the debug bundle's Range read) for shutdown reasons. Backfilled entries carry a
`recovered` marker and are de-duplicated by timestamp and text. Lost: events that scrolled out of
those sources, such as MCU disconnects during a long outage.

### Tests

pytest: each event kind recorded once with the job attached; bounds evict oldest; survives a
plugin restart. C++: view and bundle read either source.

---

## Phase 7 - Camera photos in history

### Today

Webcams from `server.webcams.list`; frames via `CameraStream::fetch_snapshot`. `PrintHistoryJob` has
no image field; the app does not parse `auxiliary_data`.

### Design

`helix_photos.py` fetches the default webcam's snapshot URL after layer 2 (from `print_stats.info`)
and at job end, stores JPEGs under a dot directory, links them from the history entry's
`auxiliary_data`, and prunes them with the history entry. History detail shows them. A job marked
`plr_resume` (Phase 0 marker) skips the layer-2 photo; the interrupted job already has it. Layer 2
comes long after history's `added` event, so the marker is always bound by then.

### Fallback

The app fetches the snapshot URL and uploads the bytes unchanged (no decode) into the same dot
directory, with the same job-id-keyed record the plugin writes (ruled 2026-10-20). Pass-through
works on every screen, the K-Touch included (buffer in PSRAM). Misses only prints that ran while
the app was down.

### Tests

pytest: capture at layer 2 once, at end once (complete, cancel, error); no webcam = no-op;
pruning with history. C++: history parses the aux link.

---

## Phase 8 - Storage report and guard

### Today

`MoonrakerPerformanceSource::fetch_disk_usage` reads total/free of the gcodes volume for the host
health widget. No breakdown, no cleanup, no free-space check before upload.

### Design

`server.helix.storage` returns usage by consumer (`.helix_temp`, 3mf outputs, analysis sidecars,
photos, timelapse, logs, thumbnails, user gcode, `helix-plr/` power-loss snapshot and mesh file) and `server.helix.storage_cleanup {consumer, older_than}`.
`print_modified` / `print_edited` refuse with a clear error when the result would leave less than a
floor free. Opt-in automatic cleanup by age.

### Fallback

The app lists the known directories over the file API and sums them (slower, same answer);
checks free space against file size before its own upload.

### Tests

Breakdown per consumer on a fixture tree; guard refuses at the floor; cleanup never touches user
gcode.

---

## Phase 9 - Learned ETA correction (dropped: app-only)

### Today

`PrintEtaEstimator::remaining_seconds` blends slicer estimate and extrapolation with no history.
`PreprintPredictor` learns pre-print time only.

Ruled 2026-10-20: **app-only**, out of this design. The app already reads history; it goes in the
app's own backlog.

---

## Phase 10 - Config read and edit

### Today

`KlipperConfigEditor` (backup, `safe_multi_edit` over the file API, FIRMWARE_RESTART) already
serves input shaper, PID and probe. Machine limits are runtime-only `SET_VELOCITY_LIMIT`. HH env
sensor reads `configfile.settings[mmu_machine]`, falling back to live status.

### Design

`server.helix.config {section?}`: the include-resolved config with each option's source file and
line, including sections Klipper's `configfile` object leaves out. `server.helix.config_edit {edits[], dry_run}`:
atomic write with backup and a returned diff. #1699's curated limits editor uses it.

### Fallback

The existing `KlipperConfigEditor`. The plugin's value is atomicity and source attribution; the
feature works without it.

---

## Phase 11 - Vendor push shims

### Today

AD5X IFS downloads `Adventurer5M.json` every 5 s idle / 30 s printing and sends GET_ZCOLOR /
IFS_STATUS on change (`ams_backend_ad5x_ifs.cpp`). ACE's REST fallback polls `/server/ace/*` every
500 ms. Qidi and CFS appear subscription-driven.

### Design

One plugin module per vendor (vendor-abstraction rule), watching the vendor file or state and
emitting `helix:vendor_state {vendor, payload}` only on change. The app backend subscribes when the
feature is ready and keeps polling otherwise.

### Fallback

Today's polling.

---

## Phase order

0 framework -> 1 analysis -> 2 fit check (uses 1) -> 3 hooks -> 4 server edits -> 5 filament
(uses 1 for per-tool amounts) -> 6 journal -> 7 photos -> 8 storage -> 10 config -> 11 vendor
shims.

Power-loss recovery is its own design, depending on Phase 0 only, and its v1 starts by
implementing Phase 0: `docs/devel/plans/2026-10-21-plugin-power-loss-recovery-design.md`. It also
builds `helix_gcode_reader.py`, which Phase 1 then uses.

## Open questions

None outstanding. Rulings of 2026-10-20 are recorded in their phases.
