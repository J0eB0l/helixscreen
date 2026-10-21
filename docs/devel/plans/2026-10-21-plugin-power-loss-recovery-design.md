# Power-loss recovery for any Klipper printer (HelixPrint plugin): design

Status: design for review (2026-10-21). Rulings of 2026-10-21 are in § 1; everything else is a
proposal. Delete this file in the change that ships the last v1 piece.

Related: the plugin umbrella spec, `docs/devel/plans/2026-10-20-helixprint-plugin-expansion-design.md`
(this design depends on its Phase 0); today's vendor backends, `docs/devel/POWER_LOSS_RECOVERY.md`;
QIDI's newer-firmware mechanism, prestonbrown/helixscreen#1716.

Evidence markers: **[src]** read in source (path given), **[web]** primary web source (URL given),
**[inf]** reasoning not yet checked on hardware.

---

## 1. Rulings (Preston, 2026-10-21)

Recorded as given.

1. Generic PLR is our own design, not a copy of QIDI's. It's a HelixPrint plugin module
   (`helix_plr.py`).
2. Cadence: 1 Hz plus event saves.
3. The snapshot (save side) is ON by default ONLY on printers we KNOW have eMMC or similar
   wear-tolerant storage. It's OFF on Pi/SD-card hosts, since we can't be sure there. "Known" is
   decided by a printer DB storage field and/or runtime detection (`/sys/block/mmcblk*/device/type`
   reading `MMC` vs `SD`), which lets a CM4 or eMMC Pi qualify (§ 7). The settings toggle is
   always available.
4. Resume is always an explicit, confirmed user action, never automatic. The default-on part is
   observe-only saving; resume is opt-in per use. That meets the umbrella spec's
   observe-on / act-opt-in rule.
5. The PLUGIN renders the resume gcode. This is an intentional exception to the umbrella spec's
   Phase 3 "app renders" ruling (§ 3.2 says why; do not "fix" it back).
6. Newer QIDI (/home/qidi): feed QIDI's `printer.print.power_off_print` endpoint. The plugin
   writes the `print_state` rows, adds our bed wait first, and applies the +4096 correction. This
   fixes #1716. Re-evaluate the generic path there after a Q2 power-cut test.
7. Unlisted printers: OFF until listed in the printer DB, so there's no implicit tier-1 default.
   Tier 1 (assisted Z) stays as a tier a DB entry or the user can select.
8. Keep the vendor backends (U1, K1/K2, old QIDI) as they are.

### Defaults, not yet ruled

These stand as the design's defaults until Preston rules otherwise:

| ID | Default | Alternative |
|---|---|---|
| D1 | No offer after `klippy_shutdown` / `error` in v1 (power loss only) | Offer with a warning |
| D2 | Adaptive-mesh carry-over needs an opt-in `[include helix-plr/*.cfg]` line in printer.cfg (§ 6.4) | Always add it at install; or no mesh carry-over |
| D3 | Multi-material: refuse when the scan's active tool or the filament backend's loaded/mounted tool does not match | Route a tool change through the filament backend |
| D4 | No Klipper extra in v1 | Ship one for exact executed-line tracking and mesh restore |

---

## 2. Goals and non-goals

### Goals

- Resume after a power cut on any Klipper printer listed for it, whichever client started the
  print, whether or not the screen was running during the print.
- Resume at the move that was actually executing, not at a guessed byte offset.
- Never move the machine without a confirmed tap on a screen; never heat the nozzle on the part.
- Fix #1716 on newer QIDI firmware through QIDI's own endpoint.

### Non-goals

- Replacing Snapmaker, Creality or old-QIDI recovery.
- Hardware power-fail detection as a requirement (an optional input later, § 13).
- Resume after a `klippy_shutdown` or error (D1).
- Toolchangers and MMU tool restore in v1 (D3).
- Printing on a delta or any printer with no XY-only homing.

---

## 3. Architecture

### 3.1 Where each piece runs

| Piece | Runs in | Why |
|---|---|---|
| Snapshot loop (save side) | Plugin `helix_plr.py` | Runs whenever Moonraker runs: independent of the UI process, of which client started the print, and of remote-only screens (Plus 4, K-Touch). |
| Storage-safety verdict | Plugin | Reads `/sys` on the host that owns the disk. |
| Offer availability | Plugin | Server truth: history status, snapshot, file identity, layer. |
| Offer timing (idle, latch, wizard) | App | Existing `plr_should_offer()` rules (`docs/devel/POWER_LOSS_RECOVERY.md` § Normalization). |
| File scan and resume point | Plugin | Needs a pass over the G-code; the host has the file locally. |
| Resume gcode | Plugin renders and sends | § 3.2 |
| Confirmations (part attached, Z) | App | Only a screen can ask. |
| PLR profile (tier, storage, homing, purge) | App resolves from the printer DB, writes it to `helix-screen` / `plr_profile`; plugin reads it | The printer DB and detection live in the app; the record persists, so the plugin acts on it with the app down. |
| Klipper code | None in v1 (D4) | Mainline has `M23`/`M26`/`M24`, `SET_KINEMATIC_POSITION`, `motion_report`, `EXCLUDE_OBJECT_DEFINE`, `BED_MESH_PROFILE LOAD` [src: `~/Code/Printing/kalico/klippy/extras/virtual_sdcard.py#cmd_M26`]. |

### 3.2 Why the plugin renders the resume gcode

Umbrella Phase 3 rules that the app renders gcode and the plugin only sends it, so no backend's
gcode is reimplemented in Python. PLR is the exception (ruling 5) because:

- The resume script is built from the file scan (modal state, defined objects, E, layer Z), and
  the scan runs in the plugin. Shipping scan output to the app so the app can render text the
  plugin then sends adds a round trip and a second consumer of the scan format, and buys nothing.
- The K-Touch (ESP32) cannot parse G-code files at all (umbrella Phase 1 ruling), so an
  app-rendered resume would be impossible there.
- One endpoint resumes from any client: the app, and later a Fluidd/Mainsail macro.

The DRY concern behind the Phase 3 rule does not apply: no vendor backend's gcode is duplicated.
Vendor resumes (U1, K1/K2, old QIDI) keep their app-side plans; the QIDI-new adapter calls QIDI's
endpoint rather than re-rendering QIDI's sequence. The umbrella spec records this exception.

### 3.3 Dependencies

- **Umbrella Phase 0 (required):** the `features` map with per-feature `version` the app gates on
  via `plugin_feature("plr", min_version)`, and opt-in read from the `helix-screen` namespace.
- **Umbrella Phase 1 (optional):** if its `layer_index` sidecar exists for the file, the scan
  starts at the current layer instead of byte 0. v1 does not require it.
- **Compatibility floor:** Python 3.8 to 3.13 (no `match`, no `removeprefix`, no dict `|`); 5 of
  10 fleet Moonrakers report API 1.4.0, so nothing uses history `auxiliary_data`; all PLR state
  lives in the plugin's own store keyed by history job id.

---

## 4. Snapshot record

One record per job. The fast block changes every save; the slow block on change; identity and
mesh once per job.

### 4.1 Schema (`schema: 1`)

| Field | Type | Source (Klipper status unless noted) | Block |
|---|---|---|---|
| `schema` | int | constant 1 | identity |
| `job_id` | str | Moonraker history current job | identity |
| `filename` | str | `print_stats.filename` (relative to gcodes root) | identity |
| `file_size`, `file_mtime` | int, float | `os.stat` | identity |
| `file_hash` | str | sha1 of first 64 KiB + last 64 KiB | identity |
| `started_at` | float | wall clock at job start | identity |
| `seq` | int | monotonic per write | fast |
| `saved_at` | float | wall clock | fast |
| `file_position` | int | `virtual_sdcard.file_position` (dispatch head) | fast |
| `toolhead_pos` | [4] float | `toolhead.position` (end of queue, kinematic frame) | fast |
| `live_pos` | [4] float | `motion_report.live_position` (executing, kinematic frame) | fast |
| `gcode_pos` | [4] float | `gcode_move.gcode_position` | fast |
| `layer`, `total_layers` | int\|null | `print_stats.info.current_layer`, `.total_layer` | fast |
| `print_duration`, `filament_used` | float | `print_stats` | fast |
| `gcode_offset` | [4] float | `gcode_move.homing_origin` | slow |
| `abs_coord`, `abs_extrude` | bool | `gcode_move.absolute_coordinates`, `.absolute_extrude` | slow |
| `speed`, `speed_factor`, `extrude_factor` | float | `gcode_move` (public factors, 1.0 = 100 %) | slow |
| `heaters` | {name: {target, temp}} | every `extruder*`, `heater_bed`, `heater_generic *`, `temperature_fan *` | slow |
| `fans` | {name: speed} | `fan`, every `fan_generic *` | slow |
| `active_extruder` | str | `toolhead.extruder` | slow |
| `limits` | {velocity, accel, scv, min_cruise_ratio} | `toolhead` | slow |
| `pa` | {extruder: {advance, smooth_time}} | each extruder | slow |
| `retraction` | {length, speed, extra, unretract_speed}\|null | `firmware_retraction` | slow |
| `objects` | list | `exclude_object.objects` (no cap) | slow |
| `excluded` | [str] | `exclude_object.excluded_objects` | slow |
| `mesh` | {profile, saved: bool, min, max, params}\|null | `bed_mesh` | once (layer 1) |
| `macro_vars` | {macro: {var: value}} | user list in `plr_profile.macro_vars` | slow |
| `power_fail_seen` | bool | reserved for § 13 | fast |

`saved` in `mesh` is true when `profile_name` names a stored profile; false for an adaptive or
unsaved mesh, which is also written to `helix-plr/mesh.cfg` (§ 6.4).

### 4.2 Store

Ruled cadence (1 Hz) makes the write path the main wear question (§ 5). Proposal:

- **Fast and slow blocks: a plugin-owned slot file** `<data>/helix-plr/snapshot.bin`, two fixed
  4 KiB slots written alternately with `pwrite` + `fdatasync`. Each slot: magic, `seq`, length,
  JSON payload, CRC32. Reader takes the valid slot with the higher `seq`. Preallocated, never
  resized, so `fdatasync` writes one data block and no inode metadata. A cut mid-write corrupts
  only the slot being written; the other slot is the previous snapshot (Marlin's
  `valid_head`/`valid_foot` idea [web: `Marlin/src/feature/powerloss.h`]).
- **Identity and mesh**: Moonraker namespace `helix_plr`, key `job`, written once per job.
- The payload is capped at 4 KiB minus header; objects and macro vars that do not fit move to the
  namespace record (written on change, rarely).

Open question O1 is whether to use Moonraker's SQLite for everything instead.

### 4.3 Lifecycle

- Created at job start (`job_state` `started`), identity written once.
- Fast block persisted while `print_stats.state == "printing"` only; nothing while idle or paused.
- Cleared on `complete` and `cancelled`; kept on `error`, `klippy_shutdown` and across boots.
- Superseded by the next job start.

---

## 5. Cadence and wear

### 5.1 Cadence (ruling 2)

In memory on every status frame (Moonraker pushes about 4 Hz). Persist:

- at most once per second while `file_position` changed;
- immediately on a layer change, an exclude, a change to factors, offset, targets or fans, and on
  pause;
- identity at job start; mesh once at layer 1 complete.

### 5.2 Write cost, with the math

Moonraker opens its SQLite with Python defaults and sets no pragma
(`~/Code/Printing/moonraker/moonraker/components/database.py`, both `sqlite3.connect` calls),
and `_insert_record` commits each upsert in its own `with conn:` transaction [src]. SQLite's
defaults are a rollback journal (`journal_mode=DELETE`, not WAL), `synchronous=FULL`, 4 KiB
pages. One upsert of a ~1.5 KB record into `namespace_store` [inf, from SQLite's documented
commit sequence]:

| Write | Bytes |
|---|---|
| Journal: header (one sector, 512) + original page 1 (change counter) + original leaf page, 2 x (4096 + 8) | ~8.7 KB |
| Journal header rewrite before commit (FULL mode) | 0.5 KB |
| DB: page 1 + leaf page | 8 KB |
| Journal create + delete: ext4 jbd2 commit of inode, directory block, bitmaps (2-3 fsyncs) | ~12-24 KB |
| **Total per commit** | **~30-40 KB** |

At 1 Hz: 3600 x 35 KB = **~125 MB/hour, ~1.25 GB per 10 h print** written by the host. The
study's ~1 GB/10 h was low. eMMC then multiplies small synchronous writes internally (partial-page
programming, FTL merges); a factor of 2-4 is typical [inf], so **~2.5-5 GB of NAND writes per
10 h print**. WAL mode would cut the journal half but Moonraker owns the connection, so the plugin
cannot switch it.

The slot file (§ 4.2): one 4 KiB data block per `fdatasync`, no metadata journal commit because
the file is preallocated and `fdatasync` skips mtime: **~4-8 KB per write, ~15-30 MB/hour, ~0.2-0.3
GB per 10 h print**, about five times less [inf; to measure, § 11.3].

Endurance check: an 8 GB TLC eMMC at ~1000 P/E cycles takes ~8 TB of NAND writes with wear
levelling. At 5 GB per 10 h (SQLite, worst case) that is ~16,000 print hours; at 0.6 GB per 10 h
(slot file, WA 2-3) ~130,000. Either is acceptable on eMMC. On an SD card with poor wear levelling,
erase-block amplification can be 10-100x [inf], which is why ruling 3 keeps saving off there.

---

## 6. Resume

### 6.1 The lead it has to correct

`virtual_sdcard` advances `file_position` when a line's move enters the toolhead lookahead. With
`BUFFER_TIME_HIGH = 2.0` as both the lookahead flush time and the queue-ahead limit
(`kalico/klippy/toolhead.py#ToolHead.__init__`, `#ToolHead._check_pause`) [src], the dispatch head
leads the nozzle by about 2 s, up to ~4 s, of motion [inf]. In bytes that is 0.2 KB on long infill
lines to 120 KB on fine curved walls, so no byte constant (QIDI's 4096) is right. Klipper publishes
both ends in one status frame: `toolhead.position` (end of queue) and
`motion_report.live_position` (executing; refreshed every 0.25 s, `STATUS_REFRESH_TIME`) [src:
`kalico/klippy/extras/motion_report.py#PrinterMotionReport.get_status`].

### 6.2 Resume-point algorithm

Inputs: the snapshot's fast block, the file. Output: `ResumePoint`.

1. **Identity.** `file_size`, `file_mtime`, `file_hash` match; else no offer.
2. **Scan** from byte 0 (or the current layer's start from `layer_index`) to `file_position`,
   reducing modal state the way `gcode_move` does: G90/G91, M82/M83, `G92`, logical E and
   toolhead E (scaled by the extrude factor in effect), X/Y/Z/F, `T<n>`, in-file heater and fan
   commands, `SET_PRESSURE_ADVANCE`, `SET_VELOCITY_LIMIT`, `M220`/`M221`, `G10`/`G11` retract state,
   `EXCLUDE_OBJECT_DEFINE`, `SET_PRINT_STATS_INFO`, layer markers (`;LAYER_CHANGE`, `;LAYER:`,
   `SET_PRINT_STATS_INFO CURRENT_LAYER=`). Keep a ring of per-line records for the last
   `kWindowBytes` (256 KiB) behind the head: offset, start and end position in the kinematic XY
   frame, toolhead-E delta, Z, layer.
3. **Walk back by the measured lead.** `dE = toolhead_pos.e - live_pos.e`. Walk the ring
   backwards subtracting each line's toolhead-E delta (negative for retracts, including firmware
   retract) until `dE` is consumed. Inside a travel run (zero E), consume XY path length toward
   `live_pos.xy` instead.
4. **Check.** The chosen line's XY segment passes within `kMatchTolMm` (0.5 mm) of `live_pos.xy`,
   and its gcode Z is consistent with the layer. Arcs (`G2`/`G3`) are checked with arc geometry.
5. **Snapshot age.** The nozzle kept going for up to one save interval after the snapshot; the
   resume point is the snapshot's executing line, so up to ~1 s of path prints twice (a small
   ridge, never a gap). Reported as `retrace_estimate_mm`.
6. **Fallback.** If the walk does not converge or the check fails: the start of the current
   layer, `method: "layer_start"`, with a warning in the plan. Never a byte rewind.

Signatures (Python, `helix_plr_scan.py`, pure, no Moonraker imports):

```python
kWindowBytes = 256 * 1024
kMatchTolMm = 0.5

class ModalState:            # dataclass-like; fields named for the gcode_move state above
    ...

class LineRecord(NamedTuple):
    offset: int; end_offset: int
    start_xy: Tuple[float, float]; end_xy: Tuple[float, float]
    z: float; toolhead_de: float; layer: Optional[int]; is_arc: bool

class ScanResult(NamedTuple):
    state_at_head: ModalState
    window: List[LineRecord]           # oldest first, ends at the head
    object_defines: List[str]          # verbatim EXCLUDE_OBJECT_DEFINE lines
    layer_starts: Dict[int, Tuple[int, ModalState]]   # layer -> (offset, state at that offset)

class ResumePoint(NamedTuple):
    offset: int                        # byte offset of the first line to print, at a line boundary
    state: ModalState                  # modal state just before that line
    method: str                        # "measured" | "layer_start"
    retrace_estimate_mm: float
    warnings: List[str]

def scan_to_offset(stream: BinaryIO, end_offset: int, start_offset: int = 0,
                   start_state: Optional[ModalState] = None,
                   cancel: Optional[threading.Event] = None) -> ScanResult: ...
def locate_executed_line(scan: ScanResult, toolhead_pos: Sequence[float],
                         live_pos: Sequence[float], extrude_factor: float,
                         retract_length: float) -> Optional[ResumePoint]: ...
def layer_start_fallback(scan: ScanResult, layer: Optional[int]) -> Optional[ResumePoint]: ...
```

The scan runs in a worker thread, cancellable, when the plan is requested; its cost is measured
on the slowest listed host before v1 ships (O3).

### 6.3 Resume sequence (generic backend)

`Zs` = `live_pos.z` (physical, kinematic frame, includes mesh and gcode offset). Rendered by
`render_prepare_script` and `render_commit_script` (§ 6.5). Phases match the endpoints (§ 9).

**prepare**

1. Pre-checks: Klipper ready, printer idle, plan valid, profile tier >= 1.
2. Bed and chamber first, nozzle cold: `M140 S<bed>`, `M190 S<bed>`; chamber `M141 S<t>` plus
   `TEMPERATURE_WAIT` with `chamber_wait_timeout_s`, skippable.
3. Declare and lift cold: `SET_KINEMATIC_POSITION Z=<Zs>`, then `G91`, `G1 Z<lift> F300`, `G90`,
   with `lift = min(profile.z_lift_mm, position_max_z - Zs - kSagAllowanceMm)`.
4. XY home: `profile.xy_home_gcode` (default `G28 X Y`; § 7.3 decides when the default is safe).
5. Tier 1 only: move to the resume line's start XY at the lifted height and stop; the app runs
   the jog step.

**commit** (after the app's confirmations)

6. Z reference: tier 2 keeps the declared frame; tier 1 reads the current toolhead Z as the part
   top and re-declares `SET_KINEMATIC_POSITION Z=<gcode_z_at_resume + gcode_offset_z + mesh(x,y)>`
   at that height, then lifts again.
7. Restore state: mesh (§ 6.4), `SET_GCODE_OFFSET`, `SET_PRESSURE_ADVANCE` per extruder,
   `SET_VELOCITY_LIMIT`, `SET_RETRACTION`, `M220`/`M221`, heater targets other than bed,
   `EXCLUDE_OBJECT_DEFINE` lines from the scan then `EXCLUDE_OBJECT NAME=` per exclusion,
   `SET_PRINT_STATS_INFO`, `SET_GCODE_VARIABLE` per saved macro var.
8. Nozzle: move to the purge location, `M109`, purge (`profile.purge_gcode` or the generic purge at
   a part-free bed edge computed from object polygons and bed limits), wipe.
9. Return: travel at the lifted height to the resume line's start XY, lower to its Z at `F300`,
   restore G90/G91, M82/M83, `G92 E<logical E>`, F, fans, retract state.
10. Start: `M23 <file>`, `M26 S<offset>`, `M24`. Mainline `M23` does not search subdirectories
    (Kalico has `with_subdirs`) [src], so for a file in a subfolder the plugin points a root-level
    symlink at it and attributes the history entry back to the original name with the existing
    `helix_print.py#_patch_history_entry` path.

### 6.4 Mesh carry-over

- `mesh.saved`: `BED_MESH_PROFILE LOAD=<profile>`.
- Unsaved/adaptive: at layer 1 the plugin writes `config/helix-plr/mesh.cfg` with one
  `[bed_mesh helix_plr]` section in the autosave format (`version`, `points`, `min_x`, `max_x`,
  `min_y`, `max_y`, `x_count`, `y_count`, `mesh_x_pps`, `mesh_y_pps`, `algo`, `tension`). Klipper
  loads every `bed_mesh <name>` section from any included file
  (`kalico/klippy/extras/bed_mesh.py#ProfileManager.__init__`, `get_prefix_sections`), and a
  wildcard include with no match is legal (`kalico/klippy/configfile.py`, the `has_magic` check)
  [src]. So after the reboot Klipper knows `helix_plr` and the resume loads it.
- Needs `[include helix-plr/*.cfg]` in printer.cfg (D2). Without it, an unsaved mesh makes the
  plan carry warning `mesh_unavailable`, and resume needs the extra confirmation `accept_no_mesh`.

### 6.5 Render functions

```python
def render_prepare_script(plan: "ResumePlan", profile: "PlrProfile") -> List[str]: ...
def render_commit_script(plan: "ResumePlan", profile: "PlrProfile",
                         z_reference: "ZReference") -> List[str]: ...
def generic_purge_xy(objects: List[dict], bed_min: Tuple[float, float],
                     bed_max: Tuple[float, float], margin_mm: float) -> Optional[Tuple[float, float, float, float]]: ...
```

Pure; golden-file tested (§ 11.1). Gcode parameters built from file content go through one
escaping helper (object names, filename), the same rule the app applies
(`plr_is_safe_recovery_filename()` in `include/plr_backend.h`).

---

## 7. Tiers, the printer DB block, storage safety

### 7.1 Tiers

| Tier | Z handling | Who can select it |
|---|---|---|
| 0 | Not offered; snapshot off by default | Default for every unlisted printer (ruling 7) |
| 1 | Assisted: user jogs the nozzle to touch the top of the print and confirms | DB entry, or the user on any printer that passes § 7.3 |
| 2 | Trusted: `Zs` from the snapshot, one visual confirm | DB entry with `z_holds_unpowered: true`, or the user |
| 3 | Re-referenced: probe at a part-free spot, or home Z to max | Reserved; not in v1 |

Precedent: tier 2 is what Prusa Power Panic, RepRapFirmware and Marlin's default do; tier 3 is
Marlin's `POWER_LOSS_RECOVER_ZHOME` / `Z_HOME_TO_MAX` and Forge-X on the AD5M; tier 1 is
plr-klipper's "place the nozzle on the highest point" [web: Marlin `powerloss.cpp`, Prusa
`power_panic.cpp`, github.com/The--Captain/plr-klipper; src: `ad5m-forgex/.py/klipper/plugins/resurrection.py`].

### 7.2 Printer DB `plr` block

In `assets/config/printer_database.json`, per printer. Absent block = tier 0.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tier` | int 0-3 | required | Highest tier this model supports |
| `backend` | `"generic"` \| `"qidi_print_state"` \| `"vendor"` | `"generic"` | `vendor` leaves it to the existing backend (U1, K1/K2, old QIDI) |
| `storage` | `"emmc"` \| `"sd"` \| `"nand"` \| `"ssd"` \| `"unknown"` | `"unknown"` | Stock storage; input to § 7.4 |
| `z_holds_unpowered` | bool | false | Required for tier 2 |
| `xy_home_gcode` | string | `"G28 X Y"` | XY-only homing command |
| `z_lift_mm` | float | 5.0 | Cold lift before XY homing |
| `purge_gcode` | string \| null | null | Purge/wipe macro; null = generic purge |
| `z_rehome` | `"none"` \| `"probe_clear_spot"` \| `"z_max"` | `"none"` | Tier 3, reserved |

User overrides (settings, `helix-screen` / `plr_user`): `enabled` (bool), `tier` (int, may lower or
raise to 1/2 on any printer passing § 7.3), `xy_home_gcode`, `purge_gcode`.

The app resolves DB + user into `plr_profile` and writes it whenever an input changes:

```cpp
struct PlrDbBlock { int tier = 0; PlrBackendKind backend = PlrBackendKind::Generic;
                    StorageHint storage = StorageHint::Unknown; bool z_holds_unpowered = false;
                    std::string xy_home_gcode = "G28 X Y"; float z_lift_mm = 5.0f;
                    std::optional<std::string> purge_gcode; };
struct PlrUserOverride { std::optional<bool> enabled; std::optional<int> tier;
                         std::optional<std::string> xy_home_gcode, purge_gcode; };
struct PlrProfile { int tier; PlrBackendKind backend; StorageHint storage_hint;
                    std::string xy_home_gcode; float z_lift_mm;
                    std::optional<std::string> purge_gcode; std::optional<bool> user_enabled; };

std::optional<PlrDbBlock> plr_parse_db_block(const nlohmann::json& printer_entry);
PlrProfile plr_resolve_profile(const std::optional<PlrDbBlock>& db, const PlrUserOverride& user,
                               const PlrHomingCheck& homing);
```

### 7.3 XY-homing safety check

`G28 X Y` is unsafe when a `[homing_override]` would run for X/Y and its gcode homes Z (for example
a `G28 Z` at a `safe_z_home` centre over the part). The plugin reads
`configfile.settings.homing_override` (`axes`, `gcode`):

```python
class HomingCheck(NamedTuple):
    xy_home_safe: bool
    reason: Optional[str]     # "override_homes_z" | "delta_kinematics" | None

def check_xy_homing(configfile_settings: dict, kinematics: str) -> HomingCheck: ...
```

Unsafe and no `xy_home_gcode` in the DB or user settings: effective tier 0 with that reason.

### 7.4 Storage safety (ruling 3)

The plugin finds the block device behind its data directory and classifies it:

1. `os.stat(data_dir).st_dev` -> `major:minor` -> `/sys/dev/block/<maj>:<min>`; a partition
   resolves to its parent disk (`../`).
2. Classify:

| Device | Evidence | Class |
|---|---|---|
| `mmcblk*` | `device/type` reads `MMC` | `emmc` |
| `mmcblk*` | `device/type` reads `SD` | `sd` |
| `nvme*` | name | `ssd` |
| `sd*` | `removable` = 0 and `queue/rotational` = 0 | `ssd` |
| `sd*` | `removable` = 1 (USB stick) | `unknown` |
| `ubi*`, `mtdblock*` | name | `nand` |
| overlay/unresolvable | | `unknown` |

```python
class StorageVerdict(NamedTuple):
    storage_class: str        # "emmc" | "ssd" | "nand" | "sd" | "unknown"
    wear_tolerant: bool
    source: str               # "runtime" | "printer_db" | "none"
    device: Optional[str]     # e.g. "mmcblk0"

def classify_block_device(sys_root: Path, st_dev: int) -> Tuple[str, Optional[str]]: ...
def storage_verdict(runtime_class: str, runtime_device: Optional[str],
                    db_hint: str) -> StorageVerdict: ...
def saving_enabled(verdict: StorageVerdict, tier: int, user_enabled: Optional[bool]) -> bool: ...
```

Rules:

- `wear_tolerant` for `emmc` and `ssd`. `nand` (raw NAND with UBIFS) counts only when the DB hint
  says so (O4). `sd` and `unknown` are not.
- Runtime evidence wins over the DB hint when it is definite (`emmc`, `sd`, `ssd`): a CM4 with eMMC
  qualifies whatever its listing says; a listed eMMC model running from an SD card does not.
  `unknown` at runtime falls back to the DB hint.
- `saving_enabled = user_enabled if set, else (verdict.wear_tolerant and tier >= 1)`.
- The settings toggle is always shown; on a non-tolerant verdict it carries a one-line note.

---

## 8. Offer rules

```python
class OfferInputs(NamedTuple):
    newest_job_status: Optional[str]   # Moonraker history
    snapshot_job_id: Optional[str]
    newest_job_id: Optional[str]
    identity_ok: bool
    layer: Optional[int]
    tier: int
    klippy_ready: bool

class OfferVerdict(NamedTuple):
    available: bool
    blocked_by: List[str]

def offer_verdict(i: OfferInputs) -> OfferVerdict: ...
```

Available when all hold, else `blocked_by` lists each failing rule:

| Rule | `blocked_by` value |
|---|---|
| newest job status is `interrupted` (D1: not `klippy_shutdown`/`error`) | `not_interrupted` |
| snapshot exists for that job id | `no_snapshot` |
| file identity holds | `file_changed` |
| `layer >= 2` (unknown layer: `print_duration >= 120` s) | `too_early` |
| tier >= 1 | `not_supported` |
| Klipper ready | `klippy_not_ready` |

The app adds its existing idle, latch and wizard rules (`plr_should_offer()`).

---

## 9. Plugin endpoints and feature entry

Feature entry in `server.helix.status` `features` (umbrella Phase 0):

```json
"plr": {"version": 1, "available": true, "enabled": true, "reason": null}
```

`enabled` is `saving_enabled(...)`. `reason` when not available: `"no_virtual_sdcard"`,
`"python_too_old"`, `"store_unwritable"`.

| Method (JSON-RPC / HTTP) | Params | Reply |
|---|---|---|
| `server.helix.plr.status` / `GET /server/helix/plr/status` | none | `{"saving": bool, "storage": StorageVerdict, "tier": int, "backend": "generic"\|"qidi_print_state"\|"none", "snapshot": {"job_id", "filename", "layer", "z", "saved_at", "age_s"} \| null, "offer": OfferVerdict}` |
| `server.helix.plr.plan` / `POST /server/helix/plr/plan` | none | `{"plan_id": str, "filename", "method": "measured"\|"layer_start", "resume_offset": int, "layer": int, "z_gcode": float, "z_physical": float, "retrace_estimate_mm": float, "bed": {"target", "current"}, "restores": [str], "warnings": [str], "requires": [Confirmation]}`; long scan replies `{"pending": true}` and emits `helix:plr_plan_ready {plan_id}` |
| `server.helix.plr.prepare` / `POST .../prepare` | `{"plan_id": str, "skip_chamber_wait": bool}` | `{"ok": true}` when steps 1-5 are done; progress via `helix:plr_progress {plan_id, step, state}` |
| `server.helix.plr.commit` / `POST .../commit` | `{"plan_id": str, "confirmations": {Confirmation: true}, "z_reference": "trusted"\|"part_top"}` | `{"ok": true, "job_started": true}` after `M24` |
| `server.helix.plr.abort` / `POST .../abort` | `{"plan_id": str}` | `{"ok": true}`; stops the sequence, heaters off, keeps the snapshot |
| `server.helix.plr.discard` / `POST .../discard` | none | `{"ok": true}`; deletes snapshot and mesh file |

`Confirmation` values: `part_attached`, `nozzle_clear`, `z_confirmed` (tier 2), `z_jogged`
(tier 1), `accept_no_mesh`, `accept_cold_bed`. `commit` refuses unless every entry of the plan's
`requires` is present, and refuses a `plan_id` that is not the latest or whose snapshot changed.

For `backend: "qidi_print_state"`, `prepare` runs only the bed/chamber wait and `commit` calls
QIDI's endpoint (§ 10); the reply shapes are the same.

Errors: Moonraker errors with codes `plr_no_plan`, `plr_stale_plan`, `plr_missing_confirmation`,
`plr_busy`, `plr_scan_failed`, `plr_klippy_error` (message carries Klipper's text).

---

## 10. QIDI-new adapter (ruling 6)

Selected when the plugin sees `SDCARD_POWER_OFF_CONTINUES_PINT_FILE` in `/printer/gcode/help` or
`speed_factor_2` in `gcode_move` status (QIDI report §4; not by path, since `/home/mks` symlinks
to `/home/qidi` there).

**Save side.** From the same snapshot loop, write QIDI's `print_state` rows in QIDI's formats
(`file_position` int string, `gcode_position`/`homing_position`/`base_position` JSON array strings,
`speed_factor` from `speed_factor_2`, never the public 1.0 value, `%f` floats, fan 0..1, objects as
JSON strings; table in the #1716 report). Every key once at job start, then on change at most 1 Hz.
One transaction for all changed keys when QIDI's database provider exposes it, else one
`config.update` per key. QIDI's own Moonraker deletes the section on complete/cancel/standby.

**Resume side.**

1. `prepare`: `M140` + `M190 S<heater_bed_target>`, chamber wait as § 6.3; nozzle untouched.
2. `commit`: compute our measured resume offset (§ 6.2) from our snapshot, then rewrite the
   `file_position` row to `min(offset + 4096, file_size)` so QIDI's `max(file_position - 4096, 0)`
   lands on our point; then call `printer.print.power_off_print`.

**Residual, not fixable through QIDI's contract:** QIDI declares `SET_KINEMATIC_POSITION
Z={gcode_position.z}` and later commands `G1 Z{gcode_position.z}` with the offset and mesh active,
so the nozzle lands off by `gcode_offset_z + mesh(x,y)` (typically 0.05-0.3 mm) [inf from
`qidi-src/klipper/virtual_sdcard.py#VirtualSD._set_printer_state_internal`]. One field feeds both
the declaration and the final move, so it cannot be corrected without changing QIDI's sequence.
Measured in the Q2 power-cut test (§ 11.3); if it matters, the generic path is the fix.

**Offer.** `offer_verdict` as § 8 plus "the `print_state` section has a `file_position` row".
The app's legacy QIDI backend (`DETECT_INTERRUPTION` + `was_interrupted`) must not select on this
layout: the plugin's `backend: "qidi_print_state"` outranks it (§ 12.1).

---

## 11. Test plan

Verification cadence per root CLAUDE.md: tags and pytest files while building; one review,
scoped mutation and one land-gate at the finish.

### 11.1 pytest (plugin)

Fixtures in `moonraker-plugin/tests/fixtures/plr/`, each with a JSON sidecar giving the snapshot
inputs and the expected resume offset.

| Fixture | Case | Expected |
|---|---|---|
| `orca_walls_relative_e.gcode` | curved outer wall, 0.5 mm segments, M83; `dE` 18 mm | `measured`, offset of the line whose segment contains `live_pos.xy` |
| `orca_infill_absolute_e.gcode` | long infill, M82, G92 E0 at layer start | `measured`; E state equals file E at that line |
| `prusa_travel_run.gcode` | `live_pos` inside a run of travels (zero E) | XY-length walk picks the right travel |
| `retract_mid_window.gcode` | retract and unretract inside the lead window | walk sums signed E; same line as without the retract pair moved |
| `fw_retract_g10.gcode` | `G10`/`G11`, `retract_length` 0.8 | toolhead E accounts for firmware retracts; `state.retracted` correct |
| `arcs_g2g3.gcode` | `live_pos` on an arc | arc geometry match |
| `vase_mode.gcode` | every move carries Z | `measured`; Z from the line |
| `zhop_layer_cross.gcode` | lead window crosses a layer change with Z-hop travel | resume line in the current layer; no previous-layer Z |
| `extrude_factor_95.gcode` | M221 S95 mid-file | toolhead-E scaling applied |
| `objects_200.gcode` | 200 `EXCLUDE_OBJECT_DEFINE` | all 200 defines rendered, no cap |
| `unicode_names.gcode` | non-ASCII object names and comments | offsets are byte offsets, not characters |
| `no_converge.gcode` | `live_pos` far from every window line | `layer_start` fallback with warning |
| `head_at_file_start.gcode` | `file_position` < window | handles a short window |
| `edited_file` | size or hash differs | offer blocked `file_changed` |

Other pytest:

- `classify_block_device` over a fake `/sys` tree: each row of § 7.4's table, partition-to-disk
  resolution, unreadable `type` file -> `unknown`.
- `storage_verdict` / `saving_enabled`: runtime `emmc` + DB `sd` -> on; runtime `sd` + DB `emmc`
  -> off; runtime `unknown` + DB `emmc` -> on; user false overrides everything; tier 0 -> off.
- `check_xy_homing`: no override; override `axes: z`; override homing Z unconditionally; delta.
- Slot file: torn write of the newer slot (truncated payload, bad CRC) -> older slot read;
  both invalid -> no snapshot; `seq` wrap.
- `offer_verdict`: one case per `blocked_by` value plus the all-pass case.
- `render_prepare_script` / `render_commit_script`: golden files for tier 1, tier 2, saved mesh,
  include-file mesh, no mesh, purge macro vs generic purge, subfolder file (symlink path).
- `generic_purge_xy`: objects covering the front edge pick another edge; full-bed objects -> None.
- QIDI adapter: row formats against the #1716 table (`speed_factor` 0.016667 at 100 %), +4096 with
  clamp at file end, offset < 4096.
- Lifecycle: cleared on complete/cancel, kept on error and restart, no writes while paused.
- `commit` refuses missing confirmations, stale `plan_id`.
- Python 3.8 job runs these (umbrella Phase 0 adds it).

### 11.2 C++ (app), tag `[plr]`

`plr_select_backend` with the new signal:

| pl_env_valid | qidi plugin backend | legacy QIDI | creality field | plugin generic offer | Expected |
|---|---|---|---|---|---|
| true | any | any | any | any | SNAPMAKER |
| false | true | true | false | false | GENERIC (QIDI adapter) |
| false | false | true | false | false | QIDI |
| false | false | false | true | false | CREALITY |
| false | false | false | false | true | GENERIC |
| false | false | false | false | false | NONE |
| false | false | false | false | true, plugin feature absent | NONE |

Wizard and decisions (signatures in § 12.2), each a table test:

- `plr_bed_decision`: target 60 / current 60 -> `Ok`; 60 / 50 with max diff 5 -> `NeedsWait`;
  60 / 22 -> `ColdWarn` (adds `accept_cold_bed`); target 0 -> `Ok`.
- `plr_required_confirmations`: tier 1 -> `part_attached, nozzle_clear, z_jogged`; tier 2 ->
  `part_attached, z_confirmed`; plan warning `mesh_unavailable` adds `accept_no_mesh`.
- `plr_next_wizard_step`: every transition in § 12.2, including abort from each step and a
  disconnect mid-prepare (-> `Failed`, never auto-commit).
- `plr_resolve_profile` / `plr_parse_db_block`: absent block -> tier 0; user raises to 1 on an
  unlisted printer with safe homing -> 1; user raises with unsafe homing and no override -> 0;
  bad types -> defaults.
- `plugin_feature("plr", 1)` absent / old plugin / disabled / ready (umbrella Phase 0 states).

### 11.3 Hardware matrix

| Device | Test | Settles |
|---|---|---|
| QIDI Q2 (.85) | Stock screen: short print, copy `moonraker-sql.db` mid-print, `select * from config where section='print_state'` | QIDI row formats (#1716 report open question 4) |
| QIDI Q2 | HelixScreen + plugin: start print, cut power at the wall at layer ~10, resume via adapter | #1716 fix; Z residual measured with a feeler gauge on the first resumed layer; bed wait |
| QIDI Q2 | Same, generic path | Ruling 6 re-evaluation |
| Q2, K1C, Voron V0 | Log `toolhead.position` vs `live_position` 4 Hz for 5 min of walls and infill | The lead in § 6.1, and the walk's line choice offline against the logged frames |
| K1C, CB1 Voron, Pi 4 (SD), CM4 eMMC | `storage_verdict` | § 7.4 on real `/sys` |
| K1C (slowest listed host) | Scan of a 100 MB file | O3 budget |
| One tier-2 model | Power cut after 10, 30, 60 min, PLA and PETG | Z holds; adhesion; cold-bed threshold |
| Any eMMC host | `iostat`/`/sys/block/*/stat` sectors written over 1 h of printing, slot file vs SQLite | § 5.2 numbers |

---

## 12. App side

### 12.1 Backend selection

`PlrBackendType::GENERIC` added in `include/plr_backend.h`. GENERIC covers both plugin-driven
backends; the plan's `backend` field says which. `PlrCapabilitySignals` gains:

```cpp
enum class PlrPluginBackend { None, Generic, QidiPrintState };
struct PlrCapabilitySignals {
    // ... existing fields ...
    PlrPluginBackend plugin_backend = PlrPluginBackend::None; // server.helix.plr.status "backend"
    bool plugin_offer_available = false;                      // status "offer.available"
    PluginFeatureState plr_feature = PluginFeatureState::Absent; // plugin_feature("plr", 1)
};
```

Order: SNAPMAKER, GENERIC when `plugin_backend == QidiPrintState`, QIDI, CREALITY, GENERIC when
`plugin_backend == Generic`. GENERIC needs `plr_feature == Ready`. The plan, the modal and the
latch reuse `PlrOfferController` (`src/ui/ui_plr_offer_controller.cpp`); GENERIC's
`PlrRecoveryPlan` carries a `plan_id` and RPC names instead of `resume_gcode`.

### 12.2 Wizard

Steps: `Offer` -> `Checks` (part attached, nozzle clear; shows bed temp and outage estimate from
`snapshot.saved_at`) -> `Preparing` (calls `prepare`, shows `helix:plr_progress`) -> `ZStep`
(tier 2: confirm; tier 1: jog with the existing motion controls, 0.05/0.1/1 mm, until the nozzle
touches the top of the print) -> `Committing` -> `Done`; `Failed` and `Aborted` from any step.
Discard from `Offer` calls `discard`.

```cpp
enum class PlrWizardStep { Offer, Checks, Preparing, ZStep, Committing, Done, Failed, Aborted };
enum class PlrConfirmation { PartAttached, NozzleClear, ZConfirmed, ZJogged, AcceptNoMesh, AcceptColdBed };
enum class PlrBedState { Ok, NeedsWait, ColdWarn };

struct PlrPlanView { int tier; std::vector<std::string> warnings; float bed_target; float bed_current;
                     std::string plan_id; PlrPluginBackend backend; };
struct PlrWizardInputs { PlrWizardStep step; bool rpc_ok; bool rpc_failed; bool connected;
                         bool user_abort; const PlrPlanView* plan;
                         std::vector<PlrConfirmation> given; };

PlrBedState plr_bed_decision(float target_c, float current_c, float max_diff_c);
std::vector<PlrConfirmation> plr_required_confirmations(const PlrPlanView& plan,
                                                        PlrBedState bed);
bool plr_commit_allowed(const PlrPlanView& plan, const std::vector<PlrConfirmation>& given,
                        PlrBedState bed);
PlrWizardStep plr_next_wizard_step(const PlrWizardInputs& in);
```

`kPlrColdBedDiffC` = 10 (Prusa's `POWER_PANIC_MAX_BED_DIFF` idea [web]; value to settle in the
tier-2 adhesion test). No step sends motion gcode itself except the tier-1 jog, which uses the
existing manual-move path; prepare/commit are plugin RPCs. Threading: RPC callbacks wrapped in
`AsyncLifetimeGuard::bg_cb()`, as the Creality probe is today.

### 12.3 Settings

Settings > Printer > Power-loss recovery: on/off (always shown), tier selector (0, 1, 2; 2 only
with a confirmation that Z holds unpowered), XY-home and purge macro fields, the storage verdict
line, and the mesh include status with an "add include" button (D2). Writes `plr_user`; the app
rewrites `plr_profile`.

---

## 13. Phasing

**v1**

- `helix_plr.py`, `helix_plr_scan.py`: snapshot loop and slot store, storage verdict, offer,
  scan and measured resume point with layer-start fallback, generic sequence for tiers 1 and 2,
  single extruder, saved and include-file mesh, all endpoints in § 9, the `plr` feature entry.
- QIDI-new adapter.
- App: GENERIC backend, wizard, settings, profile resolution, DB `plr` blocks for the models
  tested in § 11.3.

**Later**

1. Tier 3: probe at a part-free spot (`SET_KINEMATIC_POSITION` high, move, `PROBE`, correct the
   frame; Klipper refuses `PROBE` until Z is homed [src: `kalico/klippy/extras/probe.py`]); Z-max
   homing.
2. Power-fail input: a `gcode_button` on a PSU/UPS detect pin triggers an immediate save.
3. Multi-material and toolchangers through the filament backends (D3).
4. Resume from layer N after a shutdown or a failed print (D1), reusing scan and sequence.
5. `layer_index` from umbrella Phase 1, or a follower scan during the print, to skip the
   start-of-file scan (O3).
6. A Klipper extra, only if the measured walk fails on real files (D4).

---

## 14. Open questions

| ID | Question | Recommendation |
|---|---|---|
| O1 | Store: plugin slot file (≈5x fewer writes) or Moonraker SQLite for the fast block? | Slot file; identity and mesh in the Moonraker namespace |
| O2 | Inside the ≤1 s snapshot-age window: re-print it (ridge) or skip half an interval (possible gap)? | Re-print |
| O3 | Scan at resume time from byte 0, or a follower scan during the print that checkpoints modal state per layer? | Scan at resume in v1 if the K1C 100 MB budget is under ~30 s; else follower |
| O4 | Raw NAND (UBIFS) hosts: wear-tolerant only when the DB says so, or never? | Only when the DB says so |
| O5 | Unknown layer count (no `SET_PRINT_STATS_INFO`): gate on 120 s of print time? | Yes |
| O6 | Cold-bed threshold `kPlrColdBedDiffC` | 10 °C until the adhesion test says otherwise |
| O7 | Offer a resume from Fluidd/Mainsail via a `HELIX_PLR_RESUME` macro that calls the plugin? | Later; v1 is screen-only so every resume has the wizard's confirmations |
| O8 | QIDI Z residual (offset + mesh): accept, or switch QIDI to the generic path if the Q2 test shows > 0.1 mm? | Decide from the test (ruling 6 already schedules it) |
| O9 | Who adds the mesh include line (D2): installer prompt, settings button, or both? | Both, opt-in |
