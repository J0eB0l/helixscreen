# HelixPrint Plugin (Developer Guide)

How the HelixPrint Moonraker plugin and the app divide work: the feature framework every
plugin feature registers with, how the app decides between the plugin and its own fallback,
where each feature's setting lives, and one section per shipped feature.

**Plugin reference** (endpoints, full record shapes, install, configuration):
[`moonraker-plugin/README.md`](../../moonraker-plugin/README.md)
**User-facing doc**: [HelixPrint Plugin](../user/guide/helixprint-plugin.md)
**Power-loss recovery** is a plugin feature with its own guide:
[POWER_LOSS_RECOVERY.md](POWER_LOSS_RECOVERY.md#generic-helixprint-plugin---asked)

---

## Overview

HelixPrint is a Moonraker component (`[helix_print]` in `moonraker.conf`) that ships in this
repo under `moonraker-plugin/`. It runs inside Moonraker's process on the printer's host, which
gives it two things the app does not have:

- **It sees every job**, whichever client started it: the screen, Mainsail, Fluidd, a slicer
  upload with `print=true`, or Moonraker's job queue.
- **It runs where the files are**, usually on hardware stronger than the screen, and it is up
  whenever Moonraker is, whether or not any HelixScreen is connected.

The app works without the plugin. Every feature has an app-side fallback, or is an
optimization the app does without, and the app picks between plugin and fallback per feature,
never per plugin version. A remote screen (the K-Touch, a tablet, a desktop build) benefits
most, because it is the one that is not always connected and cannot afford to read whole files.

## Key Files

| File | Role |
|------|------|
| `moonraker-plugin/helix_print.py` | The component: `print_modified`, status, the feature framework (`HelixPrint#register_feature`, `always_on`, `opt_in_enabled`, `feature_entry`), the helix-initiated job marker (`HelixJobMarkers`), sibling loading (`_load_sibling`) |
| `moonraker-plugin/helix_<feature>.py` | One module per feature, loaded by `_load_sibling`; see each feature's section |
| `moonraker-plugin/helix_file_queue.py` | What a feature keeping one record per gcodes file shares: `file_event` / `FileTracker` (filelist_changed, directory events fanned out per record, hidden staging paths), `WorkQueue` (one at a time, `front`, a `ready` gate, an item's exception logged), `RecordStore` / `namespace_items` (a namespace keyed by path, with the `ns_items` fallback), `scan_files`, `IDLE_JOB_STATES`. Used by analysis and .3mf conversion |
| `moonraker-plugin/helix_gcode_reader.py` | Shared pure helpers beyond the G-code reader: `WorkerProcess`, `worker_argv`, `worker_nice`, `worker_timeout`, `atomic_write_bytes`, `atomic_publish`, `fsync_dir` |
| `moonraker-plugin/remote-install.sh` | Remote installer; `PLUGIN_FILES` names every file it downloads |
| `moonraker-plugin/tests/test_helix_features.py` | Framework tests: status entries, a sibling that fails to import, opt-in defaults |
| `include/helix_plugin_features.h`, `src/printer/helix_plugin_features.cpp` | `parse_plugin_features`, `plugin_feature_state`, `plugin_feature_opted_in`, `plugin_switch_mode`, `fallback_sender_role`, the `local_screen` presence record, `kKnownPluginFeatures`, `plugin_feature_lines` |
| `include/printer_plugin_status_state.h` | `PrinterPluginStatusState`: the parsed map, `plugin_feature(name, min_version)`, `kPluginSwitchFeatures`, the row subjects, `stored_opt_ins()` |
| `src/application/discovery_steps_core.cpp#helix_plugin_check_step` | Calls `server.helix.status` during discovery |
| `ui_xml/plugin_feature_toggle_row.xml`, `src/ui/ui_plugin_feature_row.cpp` | The shared settings row for a plugin feature's switch |
| `src/ui/ui_plugin_overview_overlay.cpp` | Advanced > HelixPrint Plugin, the read-only overview |
| `src/plugin/plugin_permissions.cpp#is_readonly_moonraker_method` | The read-only method list, including the HelixPrint reads a Lua plugin may call |
| `src/api/moonraker_gcode_guards.cpp#reject_plugin_rpc` | Refuses every other `server.helix.*` method from a Lua plugin |
| `tests/unit/test_helix_plugin_features.cpp`, `tests/unit/test_plugin_feature_row.cpp` | `[plugin_feature]`: the decision functions and the row |

---

## The Feature Framework

### The status `features` map

`server.helix.status` carries `api` (the shape of the reply itself; 2 is the first with
`features`) and a `features` map, one entry per feature:

```json
"api": 2,
"features": {
  "file_analysis": {"version": 1, "available": true, "enabled": true, "reason": null},
  "print_hooks":   {"version": 1, "available": true, "enabled": false, "reason": null}
}
```

| Field | Meaning |
|-------|---------|
| `version` | The feature's own contract version, bumped when its endpoints change shape |
| `available` | Its module loaded and its requirements hold |
| `enabled` | Switched on. Always `false` while not available (`feature_entry`) |
| `reason` | Why it is unavailable, `null` otherwise: `plugin_disabled`, `module_missing`, `load_failed: ...`, `start_failed: ...`, or the module's own probe reason |

A plugin without `api` predates the map.

**`[helix_print] enabled: False` turns off every feature.** No module starts, so no handler,
endpoint or worker of one exists: a call to `print_edited`, `config`, `config_edit`,
`journal`, `analysis` or `print_hooks/screen_start` finds no such endpoint, and
`print_modified` answers 503. Every entry reports `available: false` with reason
`plugin_disabled` (`feature_availability`), whatever its own switch or probe says.

**Each feature starts in isolation.** `component_init` runs every module's start through
`_start_isolated(name, acts, start)`. A module that fails to import registers with its load
error; one whose start raises registers `available: false` with
`start_failed: <exception>`. Either way the component and every other feature keep running.

### Registering a feature

Each feature's `_start_<feature>()` in `helix_print.py` calls `register_feature`:

```python
self.register_feature(
    helix_journal.JOURNAL_FEATURE, helix_journal.JOURNAL_FEATURE_VERSION, acts=False,
    switch=always_on,
)
```

| Argument | Meaning |
|----------|---------|
| `acts` | `True` for a feature that acts on a print or a spool. Its `plugin_features` default is off; an observer's is on |
| `switch` | An async callable that owns the feature's on/off. With one, the `plugin_features` key is never read |
| `probe` | Live availability, `(available, reason)`, read on each status call instead of the fixed `available` / `reason` |
| `extra` | Extra fields merged into the feature's status entry (PLR uses it) |

When the module is missing, the same name is registered with `available=False` and
`reason=SIBLING_LOAD_ERRORS.get(<module>, "module_missing")`, so the app can tell "not
installed" from "too old".

### Switches: `plugin_features`, own switches and `always_on`

A feature's on/off comes from exactly one place, never two:

- **The opt-in record.** `helix-screen` / `plugin_features` in Moonraker's database,
  `{"<feature>": true|false, ...}`. The app's settings rows write one field through a dotted
  key; the plugin reads the whole record (`_plugin_feature_opt_ins`) at the moment the feature
  acts, so a change applies without a restart or a notification. A missing key, or one that is
  not a bool, means the default (`opt_in_enabled`: observers on, actors off). One store means
  every screen on the printer agrees.
- **An own switch**, for a feature whose on/off depends on more than a boolean. PLR reads
  `helix-screen` / `plr_user.enabled` with a storage-dependent default; `convert_3mf` reads
  `[helix_print] convert_3mf` in `moonraker.conf`. The status entry reports that switch.
- **`always_on`**, the own switch of a feature with no user setting: one that only observes
  (file analysis, event journal) or a transport the user drives from the screen (server edits,
  config edit). No `plugin_features` key turns it off.

`HelixPrint#feature_enabled(name)` is what a module asks before it acts: registered, available,
and switched on.

### The helix-initiated job marker

A plugin path that starts a job itself marks it, so start-of-job features can leave it alone
(a resumed job must not re-run start hooks partway through a part):

- `mark_helix_initiated(filename, kind, history_name=None)` immediately before the single
  command that starts the print (`SDCARD_PRINT_FILE`, or the `M24` of a resume), never before a
  script that homes, heats or purges first. `kind` is one of `HELIX_JOB_KINDS`: `plr_resume`,
  `print_modified`, `print_edited`. A start through a link passes the original name as
  `history_name`, since the finished history entry is renamed to it; both names answer.
- `unmark_helix_initiated(filename)` when that command fails.
- When history adds the job (`history:history_changed`, action `added`), the marker binds to
  the job id and persists in namespace `helix_jobs` (key = job id, value
  `{"kind", "filenames", "start_time", "bound_at"}`, kept 30 days). An unclaimed marker expires
  after 60 s.
- `helix_initiated(job_id, filename, start_time) -> Optional[str]` answers for every module.
  History reuses job ids (and no Moonraker announces a deleted job), so a binding answers only
  for its own `start_time` and names, and an `added` event with no marker drops any binding its
  id had.

**Start-of-job features act from history `added` (or later), never from the `job_state`
start.** `job_state` events carry no job id, so the binding cannot exist yet there and
`helix_initiated()` can still answer None for a job the plugin started. `helix_print` registers
its history handler before any feature module and binds before its first `await`, so a
module's own handler of the same `added` event sees the binding.

A feature that needs a marker of its own uses a private `HelixJobMarkers` instance with its own
kind (print hooks' `screen_start`), which `helix_initiated()` never answers.

### The app side: `plugin_feature()` and one decision function

`PrinterPluginStatusState::plugin_feature(name, min_version)` returns a `PluginFeatureState`:

| State | When |
|-------|------|
| `Ready` | Plugin installed and enabled, entry present, `available`, `version >= min_version`, switched on |
| `Disabled` | The same, but switched off |
| `Absent` | Everything else: no plugin, a plugin without the map, no entry, a contract below `min_version`, `available: false` |

Callers gate on a feature and the minimum contract they need, never on the plugin's version
string. A mismatched plugin keeps serving whatever it reports, so an older plugin degrades
feature by feature; the version string still drives `PluginSelfHeal`.

**The rule:** each consumer chooses between plugin path and fallback in exactly one place, a
pure function of `plugin_feature(...)` and its own inputs (`file_analysis_source`,
`choose_edit_transport`, `decide_journal_source`, `config_edit_route`,
`standing_options_sender`). That function is tested over all states, and no caller re-derives
the choice.

### Compatibility floor

The plugin runs on every printer the app supports, measured on the fleet: Python 3.8.2 (K1C)
to 3.13.5, and Moonraker from v0.9.3 to vendor forks.

- **Python 3.8 to 3.13.** Keep `from __future__ import annotations` and `typing` generics. No
  `match`, `str.removeprefix`, dict `|`, `zoneinfo` or other 3.9+ stdlib API; no asyncio APIs
  deprecated by 3.12/3.13 (implicit `get_event_loop`). CI runs `pytest moonraker-plugin/tests/`
  on 3.8 and 3.11 (`.github/workflows/build.yml`).
- **API 1.4.0 forks.** Half the fleet (K2 Plus, QIDI Q2, AD5M, AD5X, and others) reports
  Moonraker API 1.4.0 with no `moonraker_version`. Gate on `api_version` and on probing the call,
  never on a version string.
- **History records live in the plugin's own namespace.** API 1.4.0 predates history's
  `auxiliary_data` / `save_job`, so any feature that records into history keeps its record in
  its own namespace keyed by job id, and treats `auxiliary_data` as a bonus where present.

---

## Settings Placement

No switch lives on a plugin page. Each switch sits on the settings page for what it affects,
as a `plugin_feature_toggle_row`, and the plugin gets one read-only overview.

### The switch row

`plugin_switch_mode(plugin_available, features, name, min_version, has_fallback, is_local)`
decides how the row shows, and the XML renders that mode (one place to change):

| Plugin state | Mode | The row |
|--------------|------|---------|
| Ready or Disabled | `Switch` | a normal switch |
| Plugin fine, entry present, `available: false` | `Hidden` | not shown: Install cannot help an unsupported printer. The reason shows on the overview only |
| No plugin, no map, no entry, or below `min_version`; feature has a fallback; screen on the printer's host | `Switch` | a working switch, no hint |
| The same on a remote screen | `FallbackInstallHint` / `FallbackUpdateHint` | a working switch, "Only while this app is connected. Install (or Update) the HelixPrint plugin to cover every print.", with Install for a missing plugin |
| The same for a plugin-only feature | `NeedsPlugin` / `NeedsUpdate` | greyed, "Requires the HelixPrint plugin" with Install, or "Requires a newer HelixPrint plugin" |

Before the plugin probe answers, the row is hidden. An outdated plugin gets no button: the
install flow refuses a plugin that is already installed, and version mismatch is
`PluginSelfHeal`'s job. Locality is `PrinterState::moonraker_locality()`; Unknown counts as
remote. With no plugin entry, a fallback switch shows the stored opt-in
(`PrinterPluginStatusState::stored_opt_ins()`, read on each probe) and still writes it, since
`plugin_features` lives in Moonraker's database and needs no plugin.

A feature that owns its switch (PLR) passes its own subject and callback and gets the same
modes.

**Which features get a switch.** Only features that act on a print or a spool. Observers
(file analysis, event journal) and transports (server edits, config edit, vendor push) get no
switch anywhere. Homes for the rest: print hooks and PLR on Settings > Printing; planned ones
in the plan doc.

### The overview

Advanced > HelixPrint Plugin (`PluginOverviewOverlay`) shows the plugin version and one line per
status-map feature, built by `plugin_feature_lines()`: Active, Off, or "Unavailable: <reason>".
A feature whose `kKnownPluginFeatures` row names a `PluginFeatureHome` leads to that page; a
feature with no setting, or one this app does not know (shown by its key), shows status only.
The Advanced panel shows Install while the plugin is known absent, and the overview and
Uninstall while it is installed.

### One fallback sender per printer

An attached screen and a tablet must never both send a fallback's G-code.
`fallback_sender_role(is_local, local_screen_present)`: the instance on the printer's host
sends; a remote instance sends only when no local HelixScreen is connected, and stands by when
that cannot be told.

Moonraker exposes no list of connected clients (`server.connection.identify` only registers
one; `server.extensions.list` lists agents only), so a local HelixScreen announces itself in
`helix-screen` / `local_screen` = `{"seen_at": <unix seconds>}` on each probe and every
`kLocalScreenPresenceIntervalS` (10 minutes) while connected. A remote instance reads it when it
would send: younger than two intervals (20 minutes) is present, a missing key is absent,
anything else is unknown. A crashed local screen keeps the remote standing by for up to 20
minutes, which fails toward not sending. Future fallbacks that send G-code reuse the same
function.

---

## The Lua-Plugin Allowlist

Lua plugins reach Moonraker through `helix.moonraker.call`, which runs every request through
`reject_plugin_rpc`. For `server.helix.*`:

- **Reads** pass without `moonraker_write`: `server.helix.status`, `plr.status`,
  `3mf_conversions`, `analysis`, `journal`, `config`. They are in the one read-only list,
  `is_readonly_moonraker_method`, which the permission check and the guard both read.
- **`print_modified` and `print_edited`** (with `moonraker_write`) start a print the way
  `printer.print.start` does and carry its checks: both paths validated, refused while spools
  lie on the bed.
- **Everything else is refused**, naming the method ("`<method>` is not available to
  plugins"): the PLR wizard calls, `config_edit`, `print_hooks.screen_start`, and any method
  added later. They heat, move, delete or drive the resume wizard, which only a confirmed tap on
  the screen may do.

A new read-only endpoint joins `kReadonlyMethods` in the same change, under its exact JSON-RPC
name. A mismatch fails closed. [PLUGIN_DEVELOPMENT.md](PLUGIN_DEVELOPMENT.md) has the Lua side.

---

## File Analysis (`file_analysis` v1)

The plugin reads every G-code file under `gcodes` once, in the background after an upload, a
change or startup, and keeps one record per file. The app uses the record as its first source
for per-tool facts and builds its streaming preview index from the layer index the analysis
writes. The K-Touch, which has no viewer and no whole-file parse, gets thumbnails for thumbless
files and per-tool amounts only this way. Observer: `always_on`, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_analysis.py` | Queue, backfill, endpoint, records (namespace `helix_analysis`), thumbnail publishing; run as a script, the worker that analyzes one file |
| `moonraker-plugin/helix_gcode_reader.py` | The one G-code reader shared with PLR: `scan_to_offset` with a `ScanObserver`, the layer index (`write_layer_index`, `LayerIndexWriter`, `read_layer_index`), `file_identity`, `prune_layer_indexes`, `WorkerProcess` |
| `include/file_analysis.h`, `src/printer/file_analysis.cpp` | `file_analysis_source`, `parse_file_analysis_reply`, `apply_file_analysis`, `with_layer_index_sidecar`; exception-free, compiled for the K-Touch |
| `src/printer/print_start_inputs.cpp#load` | `FileStartFactsLoader::load`: metadata, record, footer, scan, in that order |
| `src/rendering/gcode_layer_index.cpp` | `GCodeLayerIndex::build_from_sidecar`, `layer_index_file_hash` |
| `src/ui/ui_print_select_detail_view.cpp`, `src/ui/print_preview_controller.cpp` | The two previews: fetch the sidecar, then load |

**Endpoint.** `server.helix.analysis {"filename"}` answers `ready` with the record, `pending`
(queued at the front; `notify_analysis_ready {"filename"}` follows), `failed` (this version
failed twice; a change retries, a restart does not: namespace `helix_analysis_failures` keeps
`[size, mtime, attempts]` per path) or `disabled`. 400 for a path that is not a visible
`.gcode/.g/.gco`, 404 for a missing file. Record shape (schema 1: tools with per-field
`sources`, bounds, header, objects, layers, pauses, thumbnail): README § File Analysis and the
`helix_analysis.py` docstring.

**Layer index sidecar.** `.helix_index/<path>.layers.jsonl`, shared with power-loss recovery:
a header with the file's identity (size, mtime, sha1 of the first and last 64 KB), one line per
layer start with offset, Z and the full modal state, then `summary` and `complete`. One writer
per file: analysis writes `<index>.tmp` and publishes it whole (`atomic_publish`); the PLR
follower appends during a print and never touches a complete one. One prune rule:
`prune_layer_indexes` removes an index whose source is gone or changed, `.external` indexes and
orphaned `.tmp` files, except the paths PLR protects (the one it follows and the one its waiting
snapshot resumes from) and the index of the file the worker is reading. Analysis starts, and so
prunes, only after PLR has loaded its job.

**Worker.** One file at a time in a child process (Moonraker's interpreter, nice 19,
`ionice -c 3`, killed on shutdown). The timeout, `worker_timeout()` in `helix_gcode_reader.py`,
is 600 s + 60 s/MB, sized for a nice-19 worker on a MIPS or Cortex-A7 host; `print_edited`'s
worker uses the same rule. While printing it runs only on a multi-core host; a print starting
on a one-core host kills the worker and requeues the file. A file deleted while its worker ran
is forgotten, sidecar included; one changed meanwhile loses the sidecar just written and is
queued again. An embedded thumbnail block is dropped once it passes its cap or meets a line that
is not base64, and the comments after it are read as usual; an extruding move far off the
footprint grid is clipped to it before stepping.

**Decisions.** `file_analysis_source(plugin_feature("file_analysis", 1), filename)`: Plugin
only when Ready and the file is not a `.3mf`. `apply_file_analysis()`: the record wins per field
and per tool slot; grams only when every printing tool has them. `build_from_sidecar()`: only a
complete sidecar whose size and hash match the local copy, ending at its first `complete`
record.

**Fallback.** Without the plugin, with the feature unavailable, or for a pending or failed
record, the loader runs metadata, footer read and `Tn` scan, and a preview indexes the file
itself. The K-Touch then has no thumbnail for a thumbless file and no per-tool grams beyond the
footer.

**Testing.** `pytest moonraker-plugin/tests/test_helix_analysis.py
moonraker-plugin/tests/test_helix_gcode_reader.py` (needs pytest-asyncio; slicer fixtures in
`moonraker-plugin/tests/fixtures/analysis/`). `make t F='[plugin_file_analysis]'` covers the
sidecar-vs-file index equality and the loader against the mock
(`MoonrakerClientMock::set_mock_file_analysis`); `[layer_index]` covers the sidecar reader.

---

## Print-Start Hooks (`print_hooks` v1)

A print started outside HelixScreen skips the screen's pre-print step. The part of that step
that still applies once a print runs is kept as finished G-code in Moonraker's database; the
plugin sends it at each foreign start, and the app sends it itself when the plugin cannot.
Acts on a print: opt-in through `plugin_features.print_hooks`, default off. Switch:
Settings > Printing > Apply start options to prints from other apps.

| File | Role |
|------|------|
| `moonraker-plugin/helix_print_hooks.py` | `standing_gcode()`, `hook_gcode()` (pure), `PrintHooks` (history handler, `screen_start` endpoint) |
| `include/ams_backend.h#standing_print_gcode` | `AmsBackend::standing_print_gcode()`, the capability: per-print choices that still apply to a running print. Default empty |
| `src/printer/ams_backend_snapmaker.cpp#standing_print_gcode` | U1: the end-unload list, with `FORCE=1` because the print is already running |
| `include/standing_print_options.h` | `render_standing_gcode`, `standing_options_record`, `standing_options_sender`, `announce_screen_start`, `ScreenStartLedger` |
| `src/printer/standing_print_options_controller.cpp` | Keeps the record current, registers `notify_history_changed`, the app fallback send, the presence heartbeat |
| `include/moonraker_db_item.h` | `post_item_if_changed()` |

**Records and endpoints.** `helix-screen` / `standing_print_options` =
`{"version": 1, "gcode": "<newline-joined>"}`, rendered by the backends' own function and
written only when it differs from the stored value. The record is rendered from one screen's
settings, so only the screen whose `fallback_sender_role` is Sender writes it, and never an
empty one: before the filament backend reports, nothing is known, and no record beats a wrong
one (`src/printer/standing_print_options.cpp#writes_standing_record`). `POST /server/helix/print_hooks/screen_start
{"filename"}`: the screen announces its own direct start just before `printer.print.start`,
only while `print_hooks` is Ready; a private marker (`SCREEN_START_TTL_S`, 15 s) that
`helix_initiated()` never answers. `{"filename", "withdraw": true}` takes it back after a
start that failed, so a start of the same file elsewhere still gets its hooks. The plugin acts
from history `added`, and sends only if `job_still_running()` holds when it is about to:
`print_stats` reports that file `printing`. Every await before the send can outlive the job.

**Decisions.**
- Plugin `hook_gcode(kind, screen_started, enabled, record)`: nothing for any helix-initiated
  kind, a screen start, opt-in off, or an absent, empty or other-version record.
- Plugin `job_still_running(status, filename)`: the last check before the send; an
  unreadable `print_stats` reads as not running.
- App `standing_options_sender(state, opted_in, screen_started, has_gcode, role)`: a screen
  start or nothing rendered sends nothing; Ready means the plugin sends; Disabled sends nothing;
  Absent sends from the app when opted in and `fallback_sender_role` says Sender.

**Foreign-start detection (fallback).** `MoonrakerJobAPI::start_print` notes the filename in
`screen_start_ledger()`; `start_modified_print` and the PLR commit note "any job" (the commit
with its 30-minute window). A history `added` the ledger cannot claim is foreign.

**Fallback coverage.** A screen on the printer's host covers every start except one during
boot or an app restart. A remote screen covers starts only while connected, which is what its
row's hint says.

**Testing.** `pytest moonraker-plugin/tests/test_helix_print_hooks.py`;
`make t F='[plugin_print_hooks],[plugin_feature],[plr][settings]'`. Mock:
`HELIX_MOCK_HELIX_PLUGIN=1` reports `print_hooks`, enabled from the mock database.

---

## Server-Side G-code Edits (`server_edits` v1)

A print the user starts from the screen sometimes prints a rewritten copy: a pre-print toggle
comments out a step the file runs itself, a skip parameter is appended to `PRINT_START`, or
Filament Mapping renumbers tools on firmware with no mapping table. With `server_edits` the app
sends the plugin the list of edits instead of the copy; the plugin rewrites beside the original
and starts the result through the same linked-print path as `print_modified`. Transport:
`always_on`, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_edits.py` | `parse_edits` / `resolve_spans`, `apply_plan` / `apply_file` (pure), `HelixEdits` (endpoint); run as a script, the worker that rewrites one file |
| `moonraker-plugin/helix_print.py` | `_start_edits`; `resolve_original`, `resolve_temp`, `start_linked_print(..., kind)` shared with `print_modified` |
| `include/gcode_edit_list.h`, `src/rendering/gcode_edit_list.cpp` | `EditList`, `to_server_edits`, `from_server_edits`, `choose_edit_transport` |
| `src/ui/ui_print_preparation_manager.cpp` | `modify_and_print`, `modify_and_print_with_remap` fill the server-edits route; `rewrite_and_print` picks it; `start_staged_print` starts either |
| `src/api/moonraker_job_api.cpp#start_edited_print` | JSON-RPC `server.helix.print_edited` |
| `src/rendering/gcode_file_modifier.cpp`, `src/rendering/gcode_tool_remapper.cpp` | The app's half of the grammar |
| `tests/fixtures/gcode_edits/` | Golden cases both suites apply |

**Endpoint.** `server.helix.print_edited {original_filename, temp_file_path?, edits[],
modifications[]}`. The reply is `print_modified`'s plus `lines_changed`. Errors: 400 for a
malformed or out-of-bounds list (lines 1..2147483647, `end_line` at least `line_number`, at
most `MAX_EDITS` = 10000 edits), a path outside gcodes, or a copy path that is not directly in
`temp_dir` / `.helix_temp` / `helix_staging` or is the original (`resolve_temp`, shared with
`print_modified`); 409 for a line that differs from its `expect_raw_line` or a copy path that
already exists; 500 when the worker crashes or times out, or the write or start fails. With the
plugin disabled the endpoint does not exist. A range stays one span (`resolve_spans`), so a
plan's size follows the edit count, never a range's length. The rewrite runs in a worker process
(`helix_edits.py` as a script, nice 19), never in Moonraker's: its stdout is `{"changed"}` or
`{"refused": line}`. It writes a uniquely named `<name>.<random>.part` beside the copy, which is
published with `atomic_publish(exclusive=True)`: two requests for one path give one whole copy
and a 409. The job is marked `print_edited` immediately before `SDCARD_PRINT_FILE`. Edit grammar
(`comment_out`, `delete`, `inject_before`, `inject_after`, `replace`, `tool_remap`) and line
rules: README § Start Edited Print.

**Decision.** `choose_edit_transport(plugin_feature("server_edits", 1), archive_plate,
expressible)` returns Plugin only for Ready, not a `.3mf` plate, and a list `to_server_edits`
could write. A list cannot travel when it is empty, mixes edits and a remap, puts a range on a
kind without one, has a line 0 or negative tool, or holds text that is not UTF-8 (one validator for the app,
`decode_utf8` in `include/utf8_validation.h`).

**Fallback.** `DownloadRewrite`: the screen downloads the file, rewrites it and uploads the copy.
The copy ends with a newline exactly when the original does, on both paths (Klipper never runs
an unterminated final line).

**Testing.** `make t F='[plugin_server_edits]'` and `pytest moonraker-plugin/tests/test_helix_edits.py`.
A new edit kind or line rule needs a fixture case in `tests/fixtures/gcode_edits/`, which both
suites read.

---

## Printer Event Journal (`event_journal` v1)

A record of what happened to the printer: klippy shutdowns with their `state_message`, lost
MCUs, klippy disconnect and ready, firmware restarts, pauses with their cause, runout trips
during a job, classified `!!` lines, and how each job ended. Each entry carries a wall-clock
time and the job history had open. The plugin keeps it whichever client is connected; without
it the screen keeps its own and backfills gaps on reconnect. Observer: `always_on`, no switch.

| File | Role |
|------|------|
| `moonraker-plugin/helix_journal.py` | Pure rules (`classify_error`, `is_mcu_disconnect`, `pause_cause`, `runout_trips`, `JournalStore`) and `HelixJournal` |
| `tests/fixtures/journal/error_line_classes.json` | Pins the Python and C++ `!!` classifiers to one table |
| `include/printer_event_journal.h`, `src/printer/printer_event_journal.cpp` | `JournalEntry`, `classify_journal_error`, `decide_journal_source`, wire and backfill parsers, `describe_journal_entry` (firmware-compiled) |
| `include/event_journal_recorder.h`, `src/application/event_journal_recorder.cpp` | `EventJournalRecorder` (the fallback recorder and reconnect backfill) and `LocalEventJournal` |
| `src/ui/ui_printer_events_overlay.cpp`, `ui_xml/printer_events_overlay.xml` | The Printer Events view, opened from the notification history header |
| `src/system/debug_bundle_collector.cpp#build_event_journal` | The debug bundle's `event_journal` section |

**Endpoint.** `server.helix.journal {since=0, limit=100}` (limit capped at 500) returns
`{entries, truncated, latest_id, enabled}`, oldest first; `latest_id` is the next `since`. Each
append is broadcast as `notify_helix_journal_appended [entry]`. Entry kinds, classes and the
`detail` field: README § Printer Event Journal. Storage: namespace `helix_journal`, one key per
entry, 500 entries or 30 days. A `gcode_error` repeating one of the `REPEAT_LOOKBACK` (16)
newest entries (same text, class, detail and job) within `REPEAT_WINDOW_S` (60 s) of its
`last_time` is counted on it: `count` (absent means 1) and `last_time`. A repeat is written back
at most every `REPEAT_PERSIST_S` (10 s), and the next new entry flushes its final count; it is
broadcast only when first recorded.

**Decision.** `decide_journal_source(plugin_feature("event_journal", 1), recorder_built)`:
Plugin only for Ready; everything else is Screen with `record_locally` in a build that has the
recorder (`kLocalJournalRecorderBuilt`), and None in one that does not. The ESP32 firmware
(K-Touch) leaves the recorder out, so without a Ready journal its view shows "Printer events
need the HelixPrint plugin on this screen". The view, the recorder and the debug bundle all ask
it, and the view falls back to the screen journal on a plugin RPC error.

**Fallback.** `EventJournalRecorder` records only while the plugin's journal is not Ready, into
`<config dir>/event_journal.json` (200 entries, 30 days). On every (re)connect it backfills
since its newest entry from `server.gcode_store`, `server.history.list` and, after a shutdown, a
klippy.log tail. Backfilled entries carry `recovered`. The screen cannot recover firmware
restarts, the "command" pause cause, runout trips, or a shutdown with no wall-clock reference in
the log window.

**Testing.** `cd moonraker-plugin && ../.venv/bin/python -m pytest tests/test_helix_journal.py`;
`make t F='[plugin_event_journal]'` (recorder cases also `[event_journal_recorder]`) and
`make t F='[notifications]'`. Mock: `HELIX_MOCK_HELIX_PLUGIN=1` serves five journal events;
without it the screen journal can be seeded through `$HELIX_CONFIG_DIR/event_journal.json`.

---

## Config Read and Edit (`config_edit` v1)

The plugin reads the Klipper config the way Klipper does, with each option's file and line,
and writes edits atomically into the line that defines each option. `KlipperConfigEditor` uses
it for input shaper saves, PID/MPC migration and probe config rows when it is Ready. Transport:
own switch `always_on` (it writes only when the user saves on the screen); no `plugin_features`
key.

| File | Role |
|------|------|
| `moonraker-plugin/helix_config.py` | Reader (`read_config`), planner (`plan_edits`), writer (`write_plan`, `atomic_write`, backups), `HelixConfig` |
| `include/klipper_config_editor.h`, `src/system/klipper_config_editor.cpp` | `KlipperConfigEditor`, `config_edit_route`, `plan_plugin_config_edits`, `plugin_config_edit_backups`, `config_edit_target_file`, `config_backup_path` |
| `tests/fixtures/klipper_config_tree/` | One fixture tree and `expected.json` both suites read |

**Endpoints.** `server.helix.config {section?}`: the include-resolved config, per option
`{value, file, line, end_line, autosave}`, plus per-section `defined_in` and `errors`. It works
with Klipper down. Includes are read where their line stands; the root's `SAVE_CONFIG` block is
read last and supplies only options the regular config leaves undefined.
`server.helix.config_edit {edits: [{section, option, action, value, expect}], dry_run}` returns
`{dry_run, files, backups, diff}`; a refusal is 409 naming its rule first (`autosave`, `moved`,
`outside_root`, `invalid`). `invalid` includes a section or option name Klipper could not read
back (a newline, `[`, `]`, `#`, `;`; and `=`, `:` or whitespace in an option). Include globs
expand as Klipper's `glob.glob` does, without `recursive=True`, so `**` is one directory.
Files are written through `atomic_write_bytes` with their own mode. Backups go to `.helix_backups/<file>.helixbak-<stamp>` under the
config root, newest five per file. The plugin never restarts Klipper. Full shapes: README
§ Read and Edit the Klipper Config.

**Decisions.** `config_edit_route(plugin_feature("config_edit", 1))`: Ready goes to the plugin,
anything else to the file API. `plan_plugin_config_edits(section, edits, read)`: Edits, FileApi
(an option lives only in `SAVE_CONFIG`, or the list is empty) or Error.
`config_edit_target_file()`: an added key for a `SAVE_CONFIG`-only option goes to `printer.cfg`.

**Fallback.** The file API: download the include chain, edit in memory, back up to
`.helix_backups/<file>.helix_backup`, upload. Also used when the plugin read fails. Both paths
end in `restart_and_monitor()`: `FIRMWARE_RESTART`, wait for Klipper, and on timeout restore the
backups and restart again.

**Testing.** `.venv/bin/python3 -m pytest moonraker-plugin/tests/test_helix_config.py` (includes
a configparser oracle running Klipper's own read algorithm); `make t F='[plugin_config_edit]'`.
A change to either parser must keep `tests/fixtures/klipper_config_tree/expected.json` true for
both.

---

## Adding a Feature

1. **Module.** `moonraker-plugin/helix_<feature>.py`, with its feature name and contract version
   as module constants. Keep the rules pure and the Moonraker-facing class thin. Python 3.8.
   A feature that keeps one record per gcodes file builds on `helix_file_queue.py`
   (`FileTracker`, `WorkQueue`, `RecordStore`, `scan_files`); one that reads whole files does
   so in a worker (`WorkerProcess`, `worker_timeout`); one that writes a file uses
   `atomic_write_bytes` / `atomic_publish`. A sibling it imports by name loads before it in
   `helix_print.py`.
2. **Register.** A `_start_<feature>()` in `helix_print.py` that loads the sibling, registers the
   feature (`acts`, and `switch=always_on` if it has no setting), and registers it with
   `available=False` and its load error when the module is missing. Call it from
   `component_init` through `_start_isolated(name, acts, start)`, which handles a disabled
   plugin and a start that raises; a module checks `feature_enabled(name)` before it acts. A start-of-job feature acts from history `added` and checks
   `helix_initiated()`; a path that starts a job marks it.
3. **Install list.** Add the file to `PLUGIN_FILES` in `moonraker-plugin/remote-install.sh`.
   `tests/shell/test_plugin_remote_install.bats` fails for any shipped `.py` or `.sh` the
   wrapper does not fetch.
4. **Allowlist.** A read endpoint joins `kReadonlyMethods` in
   `src/plugin/plugin_permissions.cpp` under its exact JSON-RPC name. A write endpoint stays out,
   and `reject_plugin_rpc` refuses it.
5. **App decision.** One pure function of `plugin_feature(name, min_version)` and the caller's
   inputs, tested over Absent / Disabled / Ready / below minimum / unavailable. Every caller asks
   it.
6. **Switch, only if the feature acts.** One `kPluginSwitchFeatures` row (name, min version,
   `has_fallback`, `acts`, mode subject, on subject or nullptr for an own switch), one
   `kKnownPluginFeatures` row (label, icon, `PluginFeatureHome`), and on the page:
   ```xml
   <plugin_feature_toggle_row feature="x" label="..." description="..." icon="..."
                              subject="plugin_feature_on_x" callback="on_plugin_feature_switch_changed"/>
   ```
   A fallback that sends G-code asks `fallback_sender_role`.
7. **Mock.** Report the feature from the mock's `server.helix.status` under
   `HELIX_MOCK_HELIX_PLUGIN=1` and serve its endpoint; document any new variable in
   `MOCK_ENVIRONMENT_VARIABLES.md`.
8. **Tests.** pytest for the module; a C++ tag for the decision and the consumer. Shared rules
   (a parser both sides implement) get one fixture under `tests/fixtures/` that both suites read.
9. **Docs.** The README's endpoint section, a section here, the user page
   (`docs/user/guide/helixprint-plugin.md`), and the plan doc's phase deleted with the change
   that ships it.

---

## Phases Not Yet Built

Designs live in `docs/devel/plans/2026-10-20-helixprint-plugin-expansion-design.md`.

| Phase | Feature |
|-------|---------|
| 2 | File-fits-printer check: Phase 1 bounds and header against `configfile.settings`, as print-start gates; opt-in refusal of a slicer auto-start |
| 5 | Filament accounting per lane and tool: the plugin drives Moonraker's active spool for backends that do not; never deducts |
| 7 | Camera photos in history: snapshots after layer 2 and at job end, linked from the history entry |
| 8 | Storage report and guard: usage by consumer, cleanup, a free-space floor for `print_modified` / `print_edited` |
| 11 | Vendor push shims: one module per vendor emitting state changes instead of the app polling |
