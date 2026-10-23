# HelixPrint - Moonraker Plugin

A Moonraker component that handles modified G-code files while preserving original file attribution in Klipper's `print_stats` and Moonraker's history.

Developer guide (feature framework, each feature's app side, adding a feature): [docs/devel/HELIXPRINT_PLUGIN.md](../docs/devel/HELIXPRINT_PLUGIN.md).

## Quick Install (Recommended)

On a printer that runs HelixScreen, install the plugin from the app: **Advanced > Install
HelixPrint Plugin**. It installs the plugin of the app's own release.

When HelixScreen runs on another machine, the same screen shows a command to run over SSH on
the printer, pinned to the app's release:

```bash
curl -fsSL https://raw.githubusercontent.com/prestonbrown/helixscreen/v1.2.0/moonraker-plugin/remote-install.sh | sh -s -- v1.2.0
```

It downloads that release's plugin files to `~/helix_print` and runs their `install.sh --auto`:
the Moonraker symlink, the `[helix_print]` section and a Moonraker restart. It needs `sh` and
one of curl, wget or python3, and no git. Add `--uninstall` after the release to remove the
plugin, its config sections and `~/helix_print`. After the app updates, run the command it
shows again to match the plugin to it.

## Problem

When a touchscreen UI (like HelixScreen) modifies G-code files before printing (e.g., disabling bed leveling), the modifications create temporary files that pollute:
- Klipper's `print_stats.filename` shows the temp filename
- Moonraker's print history records the temp filename
- Statistics aggregate under temp filenames instead of originals

## Solution

This plugin provides a single API endpoint that:
1. Saves modified G-code to a temp directory
2. Creates a symlink with the original filename pointing to the temp file
3. Starts the print via the symlink (so Klipper sees the original name)
4. Patches history entries to record the original filename
5. Automatically cleans up temp files after a configurable delay

## Manual Installation

If you prefer manual installation:

```bash
# Clone or copy the plugin
cd /path/to/helixscreen/moonraker-plugin

# Run the installer
./install.sh

# Or specify Moonraker path explicitly
./install.sh /home/pi/moonraker
```

The installer creates a symlink from Moonraker's components directory to `helix_print.py`.
`helix_3mf.py` and `helix_capabilities.py` must stay beside `helix_print.py`: the plugin
loads them from the link's target directory, and runs without .3mf conversion when either
is missing. `helix_plr_qidi.py`, power-loss recovery through newer QIDI firmware's own
`print_state` resume, sits beside them the same way, as does `helix_journal.py`, the
printer event journal.

Vendor knowledge sits behind capability probes. The plugin asks "does this Moonraker print
.3mf natively?" through `helix_capabilities.prints_3mf_natively`, which combines a generic
probe (`VALID_GCODE_EXTS`) with one probe per vendor fork that adds .3mf printing (QIDI's).
`install.sh` asks "does this Klipper hide dot-directories?" through `klipper_hides_dot_dirs`,
which combines a generic probe with Forge-X's spelling of the filter. A new vendor gets a
new probe in that list, never a branch in the generic code.

When Moonraker runs in a chroot (Forge-X), the installer finds it through
`/proc/<pid>/root` and links the plugin by the path Moonraker sees. When `moonraker.conf`
is a symlink that includes a `user.moonraker.conf` (a firmware mod owns the main file),
`[helix_print]` is added to that user file instead. `--auto` restarts Moonraker over its
own HTTP API and never through a service manager: `POST /server/restart` when it runs no
plugin yet or only the config changed, and `POST /machine/services/restart` for the service
Moonraker reports as its own when it runs another plugin version, which only a new process
reads.

### Update Manager (Auto-Updates)

To enable automatic updates via Moonraker's update manager, add to `moonraker.conf`:

```ini
[update_manager helix_print]
type: git_repo
origin: https://github.com/prestonbrown/helixscreen.git
path: ~/helix_print
primary_branch: main
managed_services: moonraker
```

## Configuration

Add to your `moonraker.conf`:

```ini
[helix_print]
# Enable/disable the plugin (default: True). False refuses print_modified and
# starts no feature module: every feature reads `available: false`,
# reason "plugin_disabled".
enabled: True

# Directory for storing modified G-code (relative to gcodes root)
temp_dir: .helix_temp

# Directory for symlinks (relative to gcodes root). Must be a directory
# Klipper will open files under: see "Visible symlink directory" below.
symlink_dir: .helix_print

# Time in seconds to keep temp files after print completes (default: 86400 = 24 hours)
cleanup_delay: 86400

# Convert each uploaded .gcode.3mf into a printable .gcode (default: True).
# Off on Moonrakers that print .3mf natively, whatever this says.
convert_3mf: True

# Start the converted print when the slicer's upload asked to print it
# (OctoPrint-style `print=true`, as OrcaSlicer's Print button sends). Default: False.
start_3mf_on_upload: False
```

Restart Moonraker after configuration:
```bash
sudo systemctl restart moonraker
```

## API Reference

### Check Plugin Status

```http
GET /server/helix/status
```

JSON-RPC: `server.helix.status`.

Response:
```json
{
  "result": {
    "enabled": true,
    "temp_dir": ".helix_temp",
    "symlink_dir": ".helix_print",
    "cleanup_delay": 86400,
    "active_prints": 0,
    "version": "1.4.0",
    "convert_3mf": true,
    "start_3mf_on_upload": false,
    "api": 2,
    "features": {
      "convert_3mf": {"version": 1, "available": true, "enabled": true, "reason": null},
      "file_analysis": {"version": 1, "available": true, "enabled": true, "reason": null}
    }
  }
}
```

`api` is the shape of this reply; 2 added `features`. A plugin without `api` predates the map.

#### Features

Each optional feature has one entry in `features`, keyed by name:

| Field | Meaning |
|-------|---------|
| `version` | The feature's own contract version, bumped when its endpoints change shape. Clients gate on a minimum version of a feature, never on the plugin's `version` string |
| `available` | Its module loaded and its requirements (Moonraker version, components) hold |
| `enabled` | The user's choice, and always `false` when not available |
| `reason` | Why it is not available (`null` when it is), e.g. `"plugin_disabled"`, `"module_missing"`, `"load_failed: ..."` or `"start_failed: ..."` |

A feature that only observes defaults on; one that acts on a print or a spool defaults off.
The user's choices live in Moonraker's database, namespace `helix-screen`, key
`plugin_features`, written by the screen's settings: `{"<feature>": true|false, ...}`. The
plugin reads it when a feature is about to act, so a change applies without a restart. A
key that is missing or not a bool means the default.

A feature may own its switch instead, when its on/off depends on more than a boolean. Its
`plugin_features` key is then never read, and `enabled` reports the feature's own switch.
`convert_3mf` is one: `convert_3mf` under `[helix_print]` in `moonraker.conf` switches it.

A feature lives in its own `helix_<feature>.py` beside `helix_print.py`. One that fails to
import or to start reports `available: false` with the reason, and every other feature keeps
working.

#### Helix-initiated jobs

A plugin path that starts a job itself marks it immediately before the single command that
starts the print (`SDCARD_PRINT_FILE`, or the `M24` of a resume), with one of three kinds:
`print_modified`, `print_edited` or `plr_resume`, and withdraws the marker if that command
fails. When history adds the job for that file (`history:history_changed`, action `added`),
the marker is bound to the job id and stored in the `helix_jobs` namespace
(`{"<job id>": {"kind": ..., "filenames": [...], "start_time": ..., "bound_at": <unix time>}}`,
kept 30 days). A marker no job claims within 60 s expires. Plugin modules ask
`helix_initiated(job_id, filename, start_time)` with the job's values from history, so a feature
that runs at job start can leave a job the plugin started itself alone. A start through a link
binds both names, the link's and the original file's that history shows once the job finishes,
and either answers. History reuses job ids after a delete, so a binding answers only for its
job's `start_time` and names, and an id history adds again with no marker loses its binding.

### Print hooks (`print_hooks`)

Applies the screen's standing print options to a print some other client started: Mainsail,
Fluidd, a slicer, or Moonraker's job queue moving to its next job. Off until the user opts in.

The screen keeps the options in Moonraker's database, namespace `helix-screen`, key
`standing_print_options`, as finished G-code rendered by the same code its own print start
uses: `{"version": 1, "gcode": "..."}`. The plugin never renders backend G-code itself. When
history adds a job that no HelixScreen path started, the plugin sends that G-code once, and
only if `print_stats` still reports that file printing at the moment it sends. A job
with a helix-initiated marker of any kind gets nothing, and neither does a start the screen
made itself, which it announces just before its start command:

```
POST /server/helix/print_hooks/screen_start
{"filename": "benchy.gcode"}
```

The announcement waits 10 minutes for its job (a start can wait that long behind a busy
Klipper), and it is private to this feature:
`helix_initiated()` never answers it. A screen whose start command fails withdraws it, so a
start of the same file from elsewhere still gets its hooks:

```
POST /server/helix/print_hooks/screen_start
{"filename": "benchy.gcode", "withdraw": true}
```

### File-fits-printer check (`fit_check_blocks_autostart`)

Holds a slicer's upload-and-print (an upload carrying `print=true`, as OrcaSlicer's
**Print** button sends) while the file breaks a limit of this printer. Off until the user
opts in; the screen's switch is Settings > Safety & Alerts.

With the feature on, `helix_fitcheck.py` stores a plain G-code upload without Moonraker's
print flag, waits up to 120 s for its file analysis (moved to the front of the queue), and
checks it. With no blocking finding the print starts through Moonraker's own rules (started,
or queued when the printer is busy and `queue_gcode_uploads` is on); with one, nothing starts,
the file stays uploaded, the upload response says `print_started: false`, and a
`helix:fit_check_refused` notification (`notify_fit_check_refused` to clients) names the file
and the blocking findings:

```json
{"filename": "big_part.gcode",
 "findings": [{"kind": "out_of_bounds", "severity": "block",
               "detail": {"axis": "x", "side": "max", "value": 361.0, "limit": 350.0}}]}
```

A `.gcode.3mf` with `start_3mf_on_upload` asks the same check before the converter starts its
plate. A file whose analysis does not arrive in time, or a printer whose config cannot be
read, starts unchecked, with the reason logged at info.

The checker compares a file analysis record (`bounds`, `header`, tools) with Klipper's
`configfile.settings`. Each finding is `{kind, severity, detail}`:

| `kind` | `severity` | `detail` | When |
|--------|------------|----------|------|
| `out_of_bounds` | block | `axis` (`x`, `y`, `z`, `radius`), `side` (`min`, `max`), `value`, `limit` | A move outside `stepper_[xyz]` `position_min`/`position_max` (a `dual_carriage` widens its axis), or on a delta a box edge farther from the centre than `print_radius` (else `delta_radius`), or above the lowest tower `position_endstop` / below `minimum_z_position`. Travel moves are checked, else printing moves; exactly at a limit passes |
| `temp_over_max` | block | `heater` (`extruder`, `heater_bed`), `value`, `limit` | A target above the hottest `extruder*` `max_temp`, or above `heater_bed` `max_temp` |
| `too_many_tools` | block | `tool`, `limit` | A tool number at or past the count of extruders and `T<n>` macros. The plugin's hold never uses this one: a filament system's T commands are not in the config |
| `nozzle_mismatch` | warn | `tool`, `file`, `printer` | The slicer's nozzle for an extruder differs from that extruder's `nozzle_diameter` by more than 0.005 mm |
| `printer_model_mismatch` | note | `file`, `printer` | Neither model name contains the other, ignoring case and punctuation. The file's model is the slicer's `printer_model` (Cura: its machine name), never a preset name; the printer's comes from the screen, so the plugin never reports this one |

A block stops a start; a warn asks before one on the screen; a note is only shown. Neither a
warn nor a note holds a slicer's start. The screen runs the same checker as print-start gates
(`src/printer/fit_check.cpp`); `tests/fixtures/fit_check/cases.json` holds both to one table.

### Start Modified Print

The client uploads the modified file first, through Moonraker's own file upload, into
`temp_dir`, then names it here. The copy must sit directly in `temp_dir` or in one of the
app's staging directories (`.helix_temp`, `helix_staging`) and must not be the original: a
start that fails deletes it, so any other path is refused with 400.

```http
POST /server/helix/print_modified
Content-Type: application/json

{
  "original_filename": "benchy.gcode",
  "temp_file_path": ".helix_temp/mod_1702400000_benchy.gcode",
  "modifications": ["bed_leveling_disabled", "z_tilt_disabled"],
  "copy_metadata": true
}
```

Response:
```json
{
  "result": {
    "original_filename": "benchy.gcode",
    "print_filename": ".helix_print/full/benchy.gcode",
    "temp_filename": ".helix_temp/mod_1702400000_benchy.gcode",
    "status": "printing"
  }
}
```

#### Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `original_filename` | string | Yes | - | Path to the original G-code file (relative to gcodes root) |
| `temp_file_path` | string | Yes | - | Path of the already-uploaded modified file (relative to gcodes root) |
| `modifications` | array | No | `[]` | List of modification identifiers (for logging/history) |
| `copy_metadata` | bool | No | `true` | Whether to copy thumbnails from original |

#### Errors

| Code | Description |
|------|-------------|
| 400 | Original or temp file not found, a path outside the gcodes root, or a temp path outside the staging directories |
| 500 | Failed to create the symlink or start the print |
| 503 | Plugin is disabled |

### Start Edited Print

Feature `server_edits` (on by default: it moves where a print the user started from the
screen is rewritten, and acts on nothing by itself). The client sends the edits instead of an
edited copy; the plugin applies them beside the original and starts the result as
`print_modified` does, marked `print_edited`. The rewrite runs in a worker process at the
lowest CPU and I/O priority, never inside Moonraker's own, with the same time limit as file
analysis (10 minutes plus 60 s per MB). It writes a uniquely named `<copy>.<random>.part`
beside the copy and publishes it only if the copy path is still free, so two requests for one
path give one whole copy and a 409, never a mix.

```http
POST /server/helix/print_edited
Content-Type: application/json

{
  "original_filename": "benchy.gcode",
  "temp_file_path": ".helix_temp/modified_1702400000_benchy.gcode",
  "edits": [
    {"line_number": 12, "expect_raw_line": "BED_MESH_CALIBRATE", "action": "comment_out",
     "args": {"comment": "Disabled Bed mesh"}}
  ],
  "modifications": ["bed_mesh_disabled"]
}
```

The response is `print_modified`'s, plus `lines_changed`. Each edit is
`{line_number, expect_raw_line, action, args}`:

| `action` | `args` |
|----------|--------|
| `comment_out` | `comment`?, `end_line`? |
| `delete` | `end_line`? |
| `inject_before`, `inject_after` | `gcode` |
| `replace` | `gcode`, `comment`? |
| `tool_remap` | `map`: `{"<logical tool>": <physical tool>}`; `line_number` 0, and the only edit in the list |

A non-empty `expect_raw_line` must match the file's line, surrounding whitespace ignored, or
the whole list is refused and nothing is written or started. Line numbers are 1 to
2147483647, `end_line` is at least `line_number`, and a list holds at most 10000 edits; a
later edit for a line overrides an earlier one, ranges included. The app applies the same grammar
itself when the feature is off; `tests/fixtures/gcode_edits/` holds the cases both
implementations must turn into the same bytes.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `original_filename` | string | Yes | - | The file to edit (relative to gcodes root) |
| `temp_file_path` | string | No | `<temp_dir>/modified_<time>_<name>` | Where the edited copy goes (relative to gcodes root): directly in a staging directory, as for `print_modified`, and not an existing file |
| `edits` | array | Yes | - | The edit list |
| `modifications` | array | No | `[]` | List of modification identifiers (for logging/history) |

| Code | Description |
|------|-------------|
| 400 | Malformed or out-of-bounds edit list, original not found, a path outside the gcodes root, or a copy path outside the staging directories or equal to the original |
| 409 | A line differs from its `expect_raw_line`, or `temp_file_path` already exists |
| 500 | The rewrite crashed or timed out, or writing the copy, creating the symlink or starting the print failed |

With `enabled: False` the endpoint is not registered at all.

### List .3mf Conversions

```http
GET /server/helix/3mf_conversions
GET /server/helix/3mf_conversions?source=parts/benchy.gcode.3mf
```

JSON-RPC: `server.helix.3mf_conversions`, optional `source`.

Response:
```json
{
  "result": {
    "conversions": [
      {
        "source": "parts/benchy.gcode.3mf",
        "source_mtime": 1760558186.4,
        "source_size": 2139171,
        "plate": 9,
        "plate_md5": "09c7c682664f918b78ad79bca8996f82",
        "output": "parts/benchy.gcode",
        "output_md5": "09c7c682664f918b78ad79bca8996f82",
        "state": "done",
        "error": ""
      }
    ]
  }
}
```

One entry per sliced plate of each source; a source with no sliced plate (a
project .3mf) gets one `failed` entry with `plate: 0`. Every path is relative to
the gcodes root. `source_mtime` / `source_size` are the source file's as
converted: an entry whose values differ from the file's current ones is stale.

A source's entries always cover every sliced plate it holds. When a conversion
starts, every plate's entry is written as `pending` (with the source's current
mtime and size) before any plate is converted, and the finished entries replace
them together. A listing therefore never shows some plates `done` with others
missing: "every entry for this source is current and `done`" means every plate
has its output.

| `state` | Meaning |
|---------|---------|
| `done` | `output` holds the plate's G-code, md5 `output_md5` (equal to `plate_md5`) |
| `pending` | Queued, converting, or waiting for the print that holds `output` to end |
| `failed` | Not converted; `error` says why. `output` is empty |

With no `source`, every known conversion is listed. A `source` with no entry
returns an empty list. When the plugin is absent the endpoint is too (HTTP 404,
JSON-RPC -32601): treat that as "no device-side converter".

### File Analysis

Feature `file_analysis` (version 1). It only observes, so it has no switch: it
runs whenever the plugin is installed and enabled.

Every G-code file under `gcodes` (dot directories and dot files excepted) gets
one analysis record, written in the background after it is uploaded, changed or
found at startup. Each file is read by a worker process (`helix_analysis.py`
run as a script under Moonraker's own Python), one at a time, at nice 19 and
idle I/O priority where `ionice` exists, so the scan never stalls Moonraker.
While a print runs, analysis continues only on a multi-core host; on a one-core
host the worker is stopped and the file waits for the print to end. A worker
that crashes or runs past its timeout (10 minutes plus 60 s per MB) is retried
once; after that the file reads `failed` until it changes, across Moonraker restarts too
(the `helix_analysis_failures` namespace keeps the failed version's size and mtime).

```http
GET /server/helix/analysis?filename=parts/benchy.gcode
```

JSON-RPC: `server.helix.analysis {"filename": ...}`.

| `status` | Meaning |
|----------|---------|
| `ready` | `analysis` holds the record for the file as it is now |
| `pending` | No current record; the file went to the front of the queue. Wait for `notify_analysis_ready` |
| `failed` | Analysis of this version of the file failed twice; a change to the file retries it |
| `disabled` | Analysis is not running here; the `file_analysis` entry in the status says why |

A missing file answers 404; a path that is not an analyzable G-code file, 400.
Each record written is announced with the `helix:analysis_ready` notification
(`notify_analysis_ready` on the websocket), `{"filename": "parts/benchy.gcode"}`.

The record, stored in the `helix_analysis` namespace keyed by the
gcodes-relative path. A field the file does not answer is `null` (or `[]`),
never a guess:

```json
{
  "schema": 1,
  "filename": "parts/benchy.gcode",
  "file_size": 2943775,
  "file_mtime": 1760558186.4,
  "analyzed_at": 1760558190.1,
  "slicer": "OrcaSlicer 2.3.1",
  "tools": [
    {"tool": 0, "mm": 3680.43, "grams": 10.98, "color": "#00A899", "material": "PLA",
     "sources": {"mm": "footer", "grams": "footer", "color": "footer", "material": "footer"}}
  ],
  "bounds": {
    "print": {"min": [-29.79, -15.29, 0.2], "max": [29.79, 15.29, 48.0]},
    "travel": {"min": [-29.79, -15.29, 0.2], "max": [29.79, 15.29, 48.4]},
    "max_hotend_temp": 220.0,
    "max_bed_temp": 50.0
  },
  "header": {
    "printer_model": "Flashforge Adventurer 5M Pro",
    "nozzle_diameters": [0.4],
    "bed": {"min": [-110.0, -110.0], "max": [110.0, 110.0], "max_height": 220.0},
    "filament_types": ["PLA"]
  },
  "objects": {
    "source": "define",
    "list": [{"name": "3DBenchy_id_0_copy_0", "center": [0.0, 0.0], "polygon": [[-29.8, -15.3], ...]}]
  },
  "layers": {"count": 241, "index": ".helix_index/parts/benchy.gcode.layers.jsonl"},
  "pauses": 0,
  "thumbnail": null
}
```

- `file_size` / `file_mtime`: the version analyzed. A record whose values differ
  from the file's is stale; the endpoint never returns one.
- `tools`: one entry per tool the file prints with. Each field comes from the
  slicer's own config comments (`footer`), else Moonraker's metadata
  (`metadata`), else the scan (`scan`: filament length extruded under each
  `T<n>`), and `sources` says which. `mm` is filament length, `grams` comes
  only from a source that states grams.
- `bounds`: the toolhead frame (gcode offsets applied). `print` covers
  extruding moves, wipe tower and purge lines included; `travel` covers every
  move. An axis the file never names (the start position) is not in them.
  Temperatures are the highest targets the file commands.
- `header`: what the slicer declared about the printer and filaments.
- `objects`: from `EXCLUDE_OBJECT_DEFINE` (`define`), else from `; printing
  object`, `M486` or Cura `;MESH:` labels with the convex hull of each object's
  extrusion as its polygon (`label`), else `none`.
- `layers.index`: the layer index sidecar below, gcodes-relative.
- `thumbnail`: set when the analyzer published one because Moonraker's metadata
  had none: the largest PNG embedded in the file, else a top-down render of the
  print. It is written where Moonraker keeps thumbnails
  (`<dir>/.thumbs/<name>-300x300.png`) and added to the file's metadata, so
  every client shows it.

#### Layer index sidecar

`<gcodes>/.helix_index/<path>.layers.jsonl`, shared with power-loss recovery,
which builds the same file for a printing file that has none. JSON lines: a
header with the file identity (`file_size`, `file_mtime`, `file_hash`: sha1 of
the first and last 64 KB), then one line per layer start with its byte
`offset`, toolhead `z` and the full modal `state` there (positioning and
extrusion modes, position and offsets, E, feed rate, factors, tool, heater
targets, fans, pressure advance, velocity limits, retract state), so a reader
can begin at any layer instead of byte 0. Analysis also writes each layer's
`tools`, `print_z` (Z of its first extruding move) and `feature` (the `;TYPE:`
section at its start), a `progress_lines` record listing the pause, `M73` and
`SET_PRINT_STATS_INFO` lines with their offsets, a `summary` record (model
bounds without purge and wipe tower, palette, tools), and a closing
`{"complete": true}`. The format is documented in `helix_gcode_reader.py`. The
screen builds its streaming preview's layer index from this file instead of
reading the whole G-code first, once its size and `file_hash` match its copy.

An index is removed when its G-code is gone or has changed size or mtime since
the index was written, at plugin startup and whenever a print starts, except the
index power-loss recovery is writing during a print or resuming from. An index is
written by one writer: analysis replaces it whole (write then rename), and
recovery never appends to a complete one.

### Printer Event Journal

```http
GET /server/helix/journal?since=0&limit=100
```

JSON-RPC: `server.helix.journal`, optional `since` (an entry id; only newer entries are
returned) and `limit` (default 100, at most the store's 500).

Response:
```json
{
  "result": {
    "entries": [
      {
        "id": 42,
        "time": 1761040800.5,
        "kind": "klippy_shutdown",
        "text": "MCU 'mcu' shutdown: Timer too close",
        "class": "mcu",
        "detail": null,
        "job": {"job_id": "00002A", "filename": "benchy.gcode"}
      }
    ],
    "truncated": false,
    "latest_id": 42,
    "enabled": true
  }
}
```

The newest `limit` entries above `since`, oldest first; `truncated` says older ones were
left out. Ids only grow, so `latest_id` is the `since` of the next read. `job` is the job
history had open, or null.

| `kind` | Recorded when | `text` / `class` / `detail` |
|--------|---------------|-----------------------------|
| `klippy_shutdown` | Klipper shuts down | its `state_message` and that message's class |
| `mcu_disconnect` | a shutdown or failed start reports a lost MCU | the message and its class |
| `klippy_disconnect` / `klippy_ready` | Klippy leaves or reaches ready | |
| `firmware_restart` | a client sends `FIRMWARE_RESTART` | |
| `pause` | a print pauses | `detail`: `runout`, `error`, `command` or `unknown` |
| `runout` | a filament sensor loses filament during a job | `detail`: the sensor's name |
| `gcode_error` | Klipper prints a `!!` line | the line and its class |
| `job_end` | history finishes a job | `detail`: history's status |

`class` is one of `firmware`, `mcu`, `heater`, `homing`, `move`, `filament`, `command`,
`other`. The screen classifies the same way; `tests/fixtures/journal/error_line_classes.json`
pins both. The store keeps 500 entries for at most 30 days, one key per entry in the
`helix_journal` namespace, and each append is broadcast as `notify_helix_journal_appended`
with the entry as its only parameter.

A `gcode_error` repeating one of the 16 newest entries (same text, class and job) within 60 s
of that entry's last occurrence is counted on it instead of added: the entry gains `count`
(occurrences, absent means 1) and `last_time`. A macro erroring in a loop is then one entry,
written back at most every 10 s and broadcast only when first recorded, so it cannot evict
the events around it. The feature is `event_journal`, which has no
switch: it is always on, and a `plugin_features` key for it is ignored.

### Read and Edit the Klipper Config

Feature `config_edit` (`helix_config.py`). It writes only when the user saves a change on
the screen, so it has no switch: it is on whenever the module loads. Neither endpoint
needs Klipper running, and neither restarts it; the screen sends `FIRMWARE_RESTART`.

```http
GET /server/helix/config?section=probe
```

JSON-RPC: `server.helix.config`, optional `section`. The config as Klipper reads it from
`printer.cfg` in Moonraker's config root: includes where their line stands, relative to the
including file, globs sorted and expanded as Klipper expands them (`**` is one directory,
not any depth; a glob matching nothing is fine, a missing plain include is listed in
`errors`), and the `SAVE_CONFIG` block last, supplying only options the regular
config leaves undefined. Every section is listed, used by Klipper or not.

```json
{
  "root": "printer.cfg",
  "files": ["printer.cfg", "hardware.cfg", "conf.d/20-overrides.cfg"],
  "sections": {
    "probe": {
      "options": {
        "z_offset": {"value": "1.5", "file": "conf.d/20-overrides.cfg", "line": 2,
                     "end_line": 2, "autosave": false}
      },
      "defined_in": [{"file": "hw/probe.cfg", "line": 1, "autosave": false}]
    }
  },
  "errors": []
}
```

`value` is what Klipper reads (comments stripped, a multi-line value joined with `\n`);
`line` and `end_line` are 1-based. A file outside the config root is named by its absolute
path.

```http
POST /server/helix/config_edit
{"edits": [{"section": "probe", "option": "z_offset", "action": "set",
            "value": "1.75", "expect": "1.5"}],
 "dry_run": false}
```

JSON-RPC: `server.helix.config_edit`. Each edit names a `section` and `option`, an
`action` (`set`, the default, or `remove`, which comments the definition out), a `value`
for `set`, and `expect`: the option's current value, or `null` when it should not exist
yet. Edits apply in order. `set` rewrites the line that defines the option, keeping its
inline comment; a new option goes at the end of the section's last block, and a new section
at the end of `printer.cfg` above the `SAVE_CONFIG` block.

Each changed file is first copied to `.helix_backups/<file>.helixbak-<YYYYmmdd-HHMMSS>`
under the config root (the newest five per file are kept), then written through a temp
file, `fsync` and rename. Klipper's include globs never match a dot directory, so no
`[include dir/*]` reads a backup as config.
`dry_run` writes nothing and returns the same diff.

```json
{"dry_run": false, "files": ["conf.d/20-overrides.cfg"],
 "backups": [{"file": "conf.d/20-overrides.cfg",
              "backup": ".helix_backups/conf.d/20-overrides.cfg.helixbak-20261022-101500"}],
 "diff": "--- a/conf.d/20-overrides.cfg\n+++ b/conf.d/20-overrides.cfg\n..."}
```

A refused list writes nothing and answers 409 with the rule first: `autosave` (the option
is in the `SAVE_CONFIG` block, which Klipper owns), `moved` (`expect` no longer matches, or
the file changed while the edit was prepared), `outside_root`, `invalid` (a malformed
edit, or a section or option name Klipper could not read back: a newline, `[`, `]`, `#` or
`;` in either, and `=`, `:` or whitespace in an option).

## Visible symlink directory

A print started through `print_modified` runs `SDCARD_PRINT_FILE` on the link
`<symlink_dir>/full/<original path>`. Upstream Klipper opens that under the
default hidden `.helix_print`, but Forge-X's Klipper drops hidden files and
directories from the list `SDCARD_PRINT_FILE` resolves against, so there the
print fails with "Unable to open file". `install.sh --auto` reads the running
Klipper's `<klipper>/klippy/extras/virtual_sdcard.py`; when it carries that filter, it writes
`symlink_dir: helix_print` under `[helix_print]`, unless the section already sets
`symlink_dir`. Nothing else about the plugin depends on the name.

A visible directory shows in every client's file list while a print runs.
HelixScreen hides it; at startup the plugin also removes any link whose temp file
is gone, with the directories that leaves empty, so a crash leaves nothing behind.
The temp files themselves stay under the hidden `temp_dir`: Klipper opens them
through the link, never by their own path.

## .3mf Conversion

Slicers with a "use 3MF" upload option (OrcaSlicer's `use_3mf`) send
`<name>.gcode.3mf`: a zip holding the sliced plate as `Metadata/plate_<N>.gcode`.
A Moonraker that does not list `.3mf` among its G-code extensions stores the zip
but cannot scan or print it. The plugin turns each one into a plain `.gcode`
beside it, so Moonraker's metadata, thumbnails and every client work.

- **When:** on each upload, copy or overwrite of a `.3mf` (Moonraker's
  `file_manager:filelist_changed`), and once at startup for any `.3mf` not yet
  converted. One conversion runs at a time, off Moonraker's event loop, streaming
  the inflate (well under 1MB of memory).
- **Name:** `<stem>.gcode` (`<stem>` is the name without `.gcode.3mf`). It is
  written only if that path is free or holds the plugin's own earlier conversion
  of the same source; a file there already identical to the plate (same md5) is
  reused as it is. Otherwise, and for every plate of a file holding more than one,
  `<stem>_plate<N>.gcode`. A file the plugin did not write is never overwritten,
  and a symlink at either name counts as one. The name is checked again after
  extraction, just before publishing: a file that arrived there meanwhile sends the
  plate round again under whatever name is free.
- **Integrity:** the zip's CRC and the plate's `plate_<N>.gcode.md5` are checked
  before the output is published through Moonraker's own upload path.
- **Cleanup:** deleting the `.3mf` deletes its outputs, and overwriting it
  reconverts and drops outputs for plates no longer in it, in both cases only an
  output the plugin wrote and that is unchanged since. A file being printed is
  left until the print ends, then deleted (remembered across restarts). A `.3mf`
  deleted while it converts leaves no output. Unfinished extractions
  (`<temp_dir>/3mf-*.partial`) are removed at startup.
- **Unreadable archives:** an encrypted plate, a compression method Python's
  zipfile lacks, or a damaged zip gives a `failed` entry naming the error.
- **Auto-start** (`start_3mf_on_upload: True`): an upload carrying `print=true`
  starts the converted print through Moonraker's own start/queue rules: when the
  printer is busy and `[file_manager] queue_gcode_uploads` is on, it is queued, and
  the upload response says `print_queued`. The upload's own conversion goes ahead
  of any queued backfill. A multi-plate file is converted but not started. With
  `fit_check_blocks_autostart` opted into, the plate is published first and started only
  when the file-fits-printer check allows it. This hooks Moonraker's upload
  method; on a Moonraker whose method does not have the expected shape the hook
  stays off with a log line, and conversion still works.

## How It Works

```
┌─────────────────────────────────────────────────────────────────┐
│ Client (HelixScreen)                                            │
│ POST /server/helix/print_modified                               │
│ { original_filename, temp_file_path, modifications }            │
└─────────────────────────────────┬───────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────┐
│ helix_print Plugin                                              │
├─────────────────────────────────────────────────────────────────┤
│ 1. Validate original and uploaded temp file                     │
│ 2. Copy thumbnails from original                                │
│ 3. Create symlink: .helix_print/full/benchy.gcode → temp file   │
│ 4. Mark the job helix-initiated (print_modified)                │
│ 5. Start print: SDCARD_PRINT_FILE FILENAME=".helix_print/..."   │
└─────────────────────────────────┬───────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────┐
│ Klipper                                                         │
│ print_stats.filename = ".helix_print/full/benchy.gcode"         │
│ (Follows symlink, reads modified content)                       │
└─────────────────────────────────┬───────────────────────────────┘
                                  │
                                  ▼ (on completion)
┌─────────────────────────────────────────────────────────────────┐
│ helix_print Plugin (event handler)                              │
├─────────────────────────────────────────────────────────────────┤
│ 1. Patch history: filename → "benchy.gcode"                     │
│ 2. Store modifications in auxiliary_data                        │
│ 3. Delete symlink immediately                                   │
│ 4. Schedule temp file deletion (24h default)                    │
└─────────────────────────────────────────────────────────────────┘
```

## File Locations

After installation:
```
~/printer_data/gcodes/
├── benchy.gcode              # Original file (unchanged)
├── .helix_temp/
│   └── mod_1702400000_benchy.gcode  # Modified content
├── .helix_print/
│   └── benchy.gcode          # Symlink → .helix_temp/mod_...
├── .helix_index/
│   └── benchy.gcode.layers.jsonl  # Layer index (file analysis, power-loss recovery)
└── .thumbs/
    ├── benchy-32x32.png      # Original thumbnails
    └── mod_1702400000_benchy-32x32.png  # Symlink → original
```

## Uninstallation

```bash
./install.sh --uninstall
```

Then remove the `[helix_print]` section from `moonraker.conf` and restart Moonraker.

If a HelixScreen before 1.1 had instrumented PRINT_START (the "detailed print
preparation tracking" prompt), uninstalling also removes those marker blocks
from whichever Klipper config file defines PRINT_START, backing up each file
it edits as `<file>.bak.<YYYYMMDD_HHMMSS>` first. That edit takes effect at
the next Klipper restart; `helix_macros.cfg` and its `[include]` are left in
place, since the macros it defines are shared HelixScreen helpers, not part
of the instrumentation.

## Troubleshooting

### Plugin not loading

1. Check Moonraker logs: `journalctl -u moonraker -f`
2. Verify symlink exists: `ls -la ~/moonraker/moonraker/components/helix_print.py`
3. Ensure `[helix_print]` section is in `moonraker.conf`

### API returns 404

The plugin may not be loaded. Check:
```bash
curl http://localhost:7125/server/info
```
Look for `helix_print` in the components list.

### Symlinks not working

Some file systems don't support symlinks. The plugin requires a POSIX-compliant filesystem for the gcodes directory.

## License

GPL-3.0-or-later

## Contributing

This plugin is part of the HelixScreen project. Issues and PRs welcome at:
https://github.com/prestonbrown/helixscreen
