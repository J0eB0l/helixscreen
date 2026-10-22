# HelixPrint - Moonraker Plugin

A Moonraker component that handles modified G-code files while preserving original file attribution in Klipper's `print_stats` and Moonraker's history.

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
# Enable/disable the plugin (default: True)
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
      "convert_3mf": {"version": 1, "available": true, "enabled": true, "reason": null}
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
| `reason` | Why it is not available (`null` when it is), e.g. `"module_missing"` or `"load_failed: ..."` |

A feature that only observes defaults on; one that acts on a print or a spool defaults off.
The user's choices live in Moonraker's database, namespace `helix-screen`, key
`plugin_features`, written by the screen's settings: `{"<feature>": true|false, ...}`. The
plugin reads it when a feature is about to act, so a change applies without a restart. A
key that is missing or not a bool means the default.

A feature may own its switch instead, when its on/off depends on more than a boolean. Its
`plugin_features` key is then never read, and `enabled` reports the feature's own switch.
`convert_3mf` is one: `convert_3mf` under `[helix_print]` in `moonraker.conf` switches it.

A feature lives in its own `helix_<feature>.py` beside `helix_print.py`. One that fails to
import reports `available: false` with the reason, and every other feature keeps working.

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

### Start Modified Print

The client uploads the modified file first, through Moonraker's own file upload, into
`temp_dir`, then names it here.

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
| 400 | Original or temp file not found, or a path outside the gcodes root |
| 500 | Failed to create the symlink or start the print |
| 503 | Plugin is disabled |

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
with the entry as its only parameter. The feature is `event_journal`, on unless the user
opts out.

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
  of any queued backfill. A multi-plate file is converted but not started. This hooks Moonraker's upload
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
