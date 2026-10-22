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

### Rulings on the design review (Preston, 2026-10-21)

Accepted as recommended in the first draft, or decided on review:

| ID | Ruling |
|---|---|
| O1 | The fast and slow blocks live in the plugin's two-slot file (§ 4.2). Job identity and mesh stay in the Moonraker namespace. Why SQLite was avoided is recorded in § 4.2. |
| O2 | Decided by test. Initial default: skip ahead half a snapshot interval past the executed point (`resume_skip_fraction = 0.5`, § 6.2), one named tunable. Task T2 (§ 11.4) compares re-trace and skip-half on real prints. |
| O3 | Scan at resume time. Measured first (task T1, § 11.4): a 100 MB file on a K1C-class host must scan in 30 s or less. Over 30 s, v1 switches to background per-layer checkpoints during the print (§ 6.2). |
| D1 | No offer after `klippy_shutdown` / `error` in v1 (power loss only). |
| D2 | Adaptive-mesh carry-over needs an opt-in `[include helix-plr/*.cfg]` line in printer.cfg (§ 6.4). |
| D3 | Multi-material: refuse when the scan's active tool or the filament backend's loaded/mounted tool does not match. |
| D4 | No Klipper extra in v1. |
| O5 | Unknown layer count (no `SET_PRINT_STATS_INFO`): the offer gates on 120 s of print time instead of layer >= 2. |
| O6 | Cold-bed threshold `kPlrColdBedDiffC` = 10 °C until the adhesion test (§ 11.4) says otherwise. |
| O7 | No Fluidd/Mainsail resume macro in v1; every resume goes through the screen's confirmations. Later phase. |
| O9 | The mesh include line is added opt-in from both an installer prompt and a settings button. |

Still open: O4 and O8 (§ 14).

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

- **Umbrella Phase 0 (required; implemented as v1's first task, § 13):** the `features` map with
  per-feature `version` the app gates on via `plugin_feature("plr", min_version)`, and the
  helix-initiated job marker. PLR owns its switch: `helix-screen` / `plr_user.enabled` is its only
  opt-in store, its `plugin_features` key is unused, and the `features` map reports
  `saving_enabled(...)` as `plr.enabled` (umbrella Phase 0, "One switch per feature").
- **Shared G-code reader:** PLR builds `helix_gcode_reader.py` (tokenizer, layer markers,
  `EXCLUDE_OBJECT_DEFINE`, `T<n>`, the modal-state reducer, the throttle policy). `helix_plr_scan.py`
  and umbrella Phase 1's `helix_analysis.py` both use it; neither carries its own parser.
- **Umbrella Phase 1 (optional):** its `layer_index` sidecar stores the modal state at each layer
  start. If it exists for the file, the scan starts at the current layer instead of byte 0. v1 does
  not require it.
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

Ruled cadence (1 Hz) makes the write path the main wear question (§ 5). Ruled (O1):

- **Fast and slow blocks: a plugin-owned slot file** `<data>/helix-plr/snapshot.bin`, two fixed
  4 KiB slots written alternately with `pwrite` + `fdatasync`. Each slot: magic, `seq`, length,
  JSON payload, CRC32. Reader takes the valid slot with the higher `seq`. Preallocated, never
  resized, so `fdatasync` writes one data block and no inode metadata. A cut mid-write corrupts
  only the slot being written; the other slot is the previous snapshot (Marlin's
  `valid_head`/`valid_foot` idea [web: `Marlin/src/feature/powerloss.h`]).
- **Identity and mesh**: Moonraker namespace `helix_plr`, key `job`, written once per job.
- The payload is capped at 4 KiB minus header; objects and macro vars that do not fit move to the
  namespace record (written on change, rarely).

**Why not Moonraker's SQLite for the 1 Hz record.** Moonraker opens its database with no
pragmas, so SQLite runs its rollback journal (`journal_mode=DELETE`, `synchronous=FULL`), and
`_insert_record` commits every upsert in its own transaction [src]. Each 1 Hz save would then
journal and rewrite two 4 KiB pages, create and delete the journal file and pay two or three
fsyncs with their filesystem metadata: ~30-40 KB per save, ~1.25 GB per 10 h print, and 2-4x
that in NAND writes after eMMC's internal amplification of small synchronous writes (§ 5.2).
Moonraker owns the connection, so the plugin cannot move it to WAL. The slot file writes one
4 KiB block per save with no metadata commit, about a fifth of that. Records written once per
job (identity, mesh) have no such cost and stay in the Moonraker namespace, where other tools
can read them.

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
GB per 10 h print**, about five times less [inf; to measure, § 11.4].

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
   `WINDOW_BYTES` (256 KiB) behind the head: offset, start and end position in the kinematic XY
   frame, toolhead-E delta, Z, layer.
3. **Walk back by the measured lead.** `dE = toolhead_pos.e - live_pos.e`. Walk the ring
   backwards subtracting each line's toolhead-E delta (negative for retracts, including firmware
   retract) until `dE` is consumed. Inside a travel run (zero E), consume XY path length toward
   `live_pos.xy` instead.
4. **Check.** The chosen line's XY segment passes within `MATCH_TOL_MM` (0.5 mm) of `live_pos.xy`,
   and its gcode Z is consistent with the layer. Arcs (`G2`/`G3`) are checked with arc geometry.
5. **Snapshot age (O2).** The nozzle kept going for an unknown time of up to one save interval
   after the snapshot. The resume point is the executing point advanced by
   `resume_skip_fraction x save_interval` seconds of motion, walking forward through the window
   at each line's feedrate (F times the speed factor, capped by the velocity limit). At 0 the
   resume re-traces up to one interval (a ridge, never a gap); at 1 it skips the whole interval
   (no ridge, possibly a gap). Initial default 0.5, settled by task T2. Reported as
   `retrace_estimate_mm` and `skip_estimate_mm`.
6. **Fallback.** If the walk does not converge or the check fails: the start of the current
   layer, `method: "layer_start"`, with a warning in the plan. Never a byte rewind.

Signatures (Python, pure, no Moonraker imports). `ModalState`, `LineRecord`, `ScanResult` and
`scan_to_offset` are built and live in `helix_gcode_reader.py` (the plugin's constants are
`UPPER_SNAKE`, as in the rest of the plugin); the window walk lives in `helix_plr_scan.py`:

```python
WINDOW_BYTES = 256 * 1024                  # helix_gcode_reader.py
MATCH_TOL_MM = 0.5                         # helix_plr_scan.py from here down
RESUME_SKIP_FRACTION_DEFAULT = 0.5         # tunable: plr_profile.resume_skip_fraction overrides

class ModalState:            # __slots__ class; gcode_move fields plus tool, heaters, fans, PA,
    ...                      # limits, retract, layer; copy(), to_dict()/from_dict() for layer_index

class LineRecord(NamedTuple):              # positions in the toolhead (kinematic) frame
    offset: int; end_offset: int
    start_xy: Tuple[float, float]; end_xy: Tuple[float, float]
    z: float; toolhead_de: float; layer: Optional[int]; is_arc: bool
    arc_center: Optional[Tuple[float, float]]; arc_cw: bool   # I/J arcs
    speed: float                           # F in mm/s before speed_factor (for advance_by_time)
    retract: int                           # -1 G10, +1 G11 (length is Klipper config), else 0

class ScanResult(NamedTuple):
    state_at_head: ModalState
    window: List[LineRecord]           # oldest first, ends at the head
    object_defines: List[str]          # verbatim EXCLUDE_OBJECT_DEFINE lines
    layer_starts: Dict[int, Tuple[int, ModalState]]   # layers starting in the window, plus the one
                                                      # it begins in: (offset, state before that line)
    offset: int                        # line-aligned offset reached

class ResumePoint(NamedTuple):
    offset: int                        # byte offset of the first line to print, at a line boundary
    state: ModalState                  # modal state just before that line
    method: str                        # "measured" | "layer_start"
    retrace_estimate_mm: float
    skip_estimate_mm: float
    warnings: List[str]

def scan_to_offset(stream: BinaryIO, end_offset: int, start_offset: int = 0,
                   start_state: Optional[ModalState] = None,
                   cancel: Optional[threading.Event] = None,
                   throttle: Optional[Callable[[int], None]] = None,     # RateLimiter.pace
                   on_layer_start: Optional[Callable[[int, int, ModalState], None]] = None,
                   ) -> ScanResult: ...      # on_layer_start feeds the layer_index sidecar
def locate_executed_line(scan: ScanResult, toolhead_pos: Sequence[float],
                         live_pos: Sequence[float], extrude_factor: float,
                         retract_length: float,
                         gcode_offset: Sequence[float] = (0, 0, 0, 0)) -> Optional[ExecutedLine]: ...
def advance_by_time(scan: ScanResult, executed: ExecutedLine, seconds: float,
                    speed_factor: float, max_velocity: float
                    ) -> Tuple[int, float, float]: ...   # (window index, retrace_mm, skip_mm)
def resume_point(stream: BinaryIO, scan: ScanResult, snapshot: dict, skip_fraction: float,
                 save_interval_s: float) -> Optional[ResumePoint]: ...   # measured, else layer start
def layer_start_fallback(scan: ScanResult, layer: Optional[int]) -> Optional[ResumePoint]: ...
```

The scan runs in a worker thread, cancellable, when the plan is requested (O3). Task T1 measures it
first: a 100 MB Orca file on a K1C-class host (the slowest listed) must finish in **30 s or
less**. **T1 failed (2026-10-21): 394 s median on the K1C** (§ 11.5), so v1 adds background
per-layer checkpoints: during the print, a low-priority
follower reads the file up to `file_position` and, at each layer start, records the byte offset
and modal state. Those checkpoints ARE umbrella Phase 1's `layer_index` sidecar, written in its
format through `helix_gcode_reader.py`; when Phase 1 has already indexed the file, the follower
does nothing. One throttle policy, in the reader, governs both (umbrella Phase 1 "Cost guard"):
the follower runs on any host while printing, reads no further than `file_position` and is
rate-capped; whole-file analysis runs while printing only on multi-core hosts. The resume scan
then starts at the current layer's entry via `scan_to_offset`'s `start_offset`/`start_state`.

**Checkpoint sidecar (contract).** One file per printed file at
`<gcodes>/.helix_index/<relpath>.layers.jsonl`; a printed file outside the gcodes root (a
firmware's cache copy) indexes under `<gcodes>/.helix_index/.external/<name>.layers.jsonl`. JSON
lines, append-only: a header `{"format": 1, "file_size", "file_mtime", "file_hash"}` naming the
file version it indexes (a mismatch starts the index over), one line per layer start
`{"layer", "offset", "z", "state": ModalState.to_dict()}`, and `{"complete": true, "end_offset"}`
once a completed print was followed to the end (a complete index is not followed again). The
reader and writer are `helix_gcode_reader.read_layer_index` / `LayerIndexWriter`; a line cut short
by a crash is skipped. The follower runs only while PLR saving is enabled (it is started from the
save path) and is held to `FOLLOWER_CPU_FRACTION` (15 %) of one core plus the byte cap.
Visibility: Moonraker's gcodes file list only lists G-code extensions
(`file_manager.py#FileManager.get_file_list`, `VALID_GCODE_EXTS`), so the `.jsonl` files never
appear as files; the `.helix_index` directory appears in a directory listing the way
`.helix_temp`, `.helix_print` and `.thumbs` already do, and the app and Mainsail/Fluidd hide
dot-directories (`ui_print_select_file_provider.cpp`). Checked in mainline Moonraker's source; the
QIDI fork's `file_manager.py` is not in the copies we hold, so it is unverified there.

**The file that is scanned.** `file_position` counts bytes of the file Klipper is reading,
`virtual_sdcard.file_path`, which on QIDI is a `.cache` copy, or for a `.3mf` the extracted
`plate_<n>.gcode`. The snapshot loop records that path and its identity in the job record
(`print_path`, `print_identity`) on the first save, and the follower, the resume scan and QIDI's
`file_size` all use it; the gcodes file is the fallback before it is known.

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
    `helix_print.py#_patch_history_entry` path. Immediately before sending the start (`M23`,
    `M26`, `M24` as one command, after steps 6-9 have finished, never ahead of the heating and
    purge), the plugin records the helix-initiated marker with kind `plr_resume` and, for the
    root-level symlink, the original name as `history_name` (umbrella Phase 0; it waits only
    60 s for history to add the job) and withdraws it with
    `unmark_helix_initiated` if the start fails, so start hooks (Phase 3: U1
    end-unload, AMS pre-print gcode, timelapse setup), per-job spool records (Phase 5) and the
    layer-2 photo (Phase 7) do not run again for the resumed job. The marker is keyed by filename
    (the root-level symlink's name when there is one, since that is what history records) and
    binds to the job id on history's `added` event; those consumers act from that event, not
    from the `job_state` start, where `helix_initiated()` can still be None.

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
| `resume_skip_fraction` | float 0-1 | 0.5 | Fraction of the save interval skipped past the executed point (§ 6.2); the user may override |

User overrides (settings, `helix-screen` / `plr_user`): `enabled` (bool), `tier` (int, may lower or
raise to 1/2 on any printer passing § 7.3), `xy_home_gcode`, `purge_gcode`.

**Contract with the plugin.** The app folds the user overrides into `helix-screen` / `plr_profile`,
a JSON object of the `PlrProfile` fields in snake_case: `tier` (int 0-3), `backend`
(`"generic"`|`"qidi_print_state"`|`"vendor"`), `storage_hint` (`"emmc"`|`"sd"`|`"nand"`|`"ssd"`|
`"unknown"`), `z_holds_unpowered` (bool), `xy_home_gcode` (str), `z_lift_mm` (float), `purge_gcode`
(str|null), `resume_skip_fraction` (float 0-1), `macro_vars` ({macro: [variable, ...]}). A missing
record is tier 0; a missing or wrongly typed key takes the default above. The plugin reads only
`enabled` from `plr_user`; it is PLR's single switch.

The app resolves DB + user into `plr_profile` and writes it whenever an input changes:

```cpp
enum class PlrBackendKind { Generic, QidiPrintState, Vendor };
enum class StorageHint { Emmc, Sd, Nand, Ssd, Unknown };
struct PlrHomingCheck { bool xy_home_safe = false; bool has_xy_home_override = false; };

struct PlrDbBlock { int tier = 0; PlrBackendKind backend = PlrBackendKind::Generic;
                    StorageHint storage = StorageHint::Unknown; bool z_holds_unpowered = false;
                    std::string xy_home_gcode = "G28 X Y"; float z_lift_mm = 5.0f;
                    std::optional<std::string> purge_gcode; float resume_skip_fraction = 0.5f; };
struct PlrUserOverride { std::optional<bool> enabled; std::optional<int> tier;
                         std::optional<std::string> xy_home_gcode, purge_gcode;
                         std::optional<float> resume_skip_fraction; };
struct PlrProfile { int tier; PlrBackendKind backend; StorageHint storage_hint;
                    std::string xy_home_gcode; float z_lift_mm;
                    std::optional<std::string> purge_gcode; std::optional<bool> user_enabled;
                    float resume_skip_fraction; };

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
    print_duration: Optional[float] = None   # O5

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

`enabled` is `saving_enabled(...)`, from PLR's own switch `plr_user.enabled` (§ 3.3); the
`plugin_features` key `plr` is unused. `reason` when not available: `"no_virtual_sdcard"`,
`"python_too_old"`, `"store_unwritable"`.

| Method (JSON-RPC / HTTP) | Params | Reply |
|---|---|---|
| `server.helix.plr.status` / `GET /server/helix/plr/status` | none | `{"saving": bool, "storage": StorageVerdict, "tier": int, "backend": "generic"\|"qidi_print_state"\|"none", "snapshot": {"job_id", "filename", "layer", "z", "saved_at", "age_s"} \| null, "offer": OfferVerdict}` |
| `server.helix.plr.plan` / `POST /server/helix/plr/plan` | none | `{"plan_id": str, "tier": int, "filename", "method": "measured"\|"layer_start", "resume_offset": int, "layer": int, "z_gcode": float, "z_physical": float, "retrace_estimate_mm": float, "bed": {"target", "current"}, "restores": [str], "warnings": [str], "requires": [Confirmation]}`; long scan replies `{"pending": true}` and emits `helix:plr_plan_ready {plan_id}` |
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

As built in the v1 plugin core (`helix_plr.py`), with the shapes above unchanged:

- An error's message is `"<code>: <detail>"`, e.g. `plr_no_plan: not_interrupted,too_early` (the
  detail of `plr_no_plan` is the offer's `blocked_by`), `plr_missing_confirmation: z_confirmed`.
  A multi-extruder snapshot or a `T<n>` other than 0 at the resume point is refused with
  `plr_scan_failed: multi_material: ...` (D3, single extruder in v1).
- `helix:plr_progress` and `helix:plr_plan_ready` are the Moonraker events
  `helix_print:plr_progress` / `helix_print:plr_plan_ready`, which clients receive as
  `notify_plr_progress` / `notify_plr_plan_ready`. Steps: `bed`, `chamber`, `lift`, `home_xy`,
  `position` (tier 1) for prepare; `z_reference` (tier 1), `restore`, `purge`, `return`, `start` for
  commit; states `running`, `done`, `failed`.
- `commit` takes `z_reference: "part_top"` at tier 1 and `"trusted"` at tier 2 and refuses the other.
- `warnings` values: `layer_start`, `mesh_unavailable`, `cold_bed` (bed more than
  `COLD_BED_DIFF_C` under target at plan time, which also adds `accept_cold_bed` to `requires`),
  `no_purge_location` (no objects known, so the nozzle heats and primes at the XY home position).
- `restores` values: `bed_mesh`, `gcode_offset`, `pressure_advance`, `velocity_limits`,
  `retraction`, `speed_factor`, `extrude_factor`, `objects`, `excluded_objects`, `fans`, `macro_vars`.
- A file outside the gcodes root starts through the root-level link `helix_plr_resume.gcode`; the
  finished history entry is renamed back by `_patch_history_entry`, and the link is removed when
  the resumed job completes or is cancelled.
- The plan reply carries `tier` (as status does), so the app need not read status again.
- Events are sent as `helix_print:plr_progress` / `helix_print:plr_plan_ready`; Moonraker delivers
  them to clients as `notify_plr_progress` / `notify_plr_plan_ready`.
- `backend`: `qidi_print_state` when QIDI's fork is detected (`qidi_print_state_present` AND
  `moonraker_resumes_print_state`) and the profile lists the printer (`backend:
  "qidi_print_state"` or tier >= 1), at tier 2 since QIDI's sequence declares the snapshot's Z;
  otherwise `generic` at the effective tier, or `none` at tier 0. For `qidi_print_state` the
  offer also needs QIDI's `file_position` row (else `no_snapshot`), the plan skips purge and mesh
  (QIDI's sequence does both), `prepare` runs only the heat wait, `commit` hands the heat wait to
  the adapter's `commit`, `abort` keeps the rows (the plugin turns the heaters off), and `discard`
  deletes the rows as well as the snapshot and mesh file.
- `plr_profile` and the switch: § 7.2. The checkpoint sidecar: § 6.2.

---

## 10. QIDI-new adapter (ruling 6)

Lives in its own module, `helix_plr_qidi.py`, per umbrella Phase 11's one-module-per-vendor rule;
`helix_plr.py` stays vendor-free and asks it capability questions. Sources below are QIDI's own,
copied from the Q2 on 01.01.02.01 (`qidi-src/`).

**Selection.** Both must hold:

- `qidi_print_state_present(gcode_help, gcode_move_status)`: `SDCARD_POWER_OFF_CONTINUES_PINT_FILE`
  in `/printer/gcode/help` (QIDI registers it with `desc=`) or `speed_factor_2` in `gcode_move`
  status (QIDI report §4; not by path, since `/home/mks` symlinks to `/home/qidi` there).
- `moonraker_resumes_print_state(klippy_apis, database)`: Moonraker carries `power_off_print` and
  the config-table methods. `speed_factor_2` alone may also appear on older QIDI firmware without
  the endpoint.

**Save side.** From the same snapshot loop, write QIDI's `print_state` rows, every value a string
(Moonraker binds it straight into SQLite). Formats follow the parser
(`database.py#MoonrakerDatabase.generate_power_off_print_gcode`,
`virtual_sdcard.py#VirtualSD._set_printer_state_internal`):

| Row | Written as | QIDI reads it as |
|---|---|---|
| `file_position` | integer string | `int(float(v))`, then `max(v - 4096, 0)` |
| `gcode_position`, `homing_position`, `base_position` | JSON 4-vector | `json.loads`, `float` per element |
| `speed` | `%f`, gcode_move's internal speed (`speed_2`) | assigned to `gcode_move.speed` |
| `speed_factor` | `%f` of `speed_factor_2` (public / 60), 0.016667 at 100 %; never the public 1.0 | `M220 S{int(v * 60 * 100)}` |
| `extrude_factor` | `%f` | `M221 S{int(v * 100)}` |
| `absolute_coord`, `absolute_extrude` | `"1"` / `"0"` | true for `"1"`, `"true"`, `"yes"` |
| `extruder_target`, `heater_bed_target`, `chamber_target` | integer string, `%f` when fractional | `M104`/`M140`/`M141 S{:.2f}` |
| `cooling_fan_speed`, `auxiliary_cooling_fan_speed`, `chamber_circulation_fan_speed` | `%f`, 0..1 | `M106 [P2/P3] S{int(v * 255)}` |
| `all_objects`, `exclude_objects` | JSON of `exclude_object.objects` / `.excluded_objects` | `json.loads` |
| `plate_index` | `virtual_sdcard.plate_index`, default `"1"` | 3MF plate to extract |
| `current_layer` | integer string | not read by the resume |

**Rounding.** The parser truncates with `int()`, so a plain `%f` sends the wrong integer for many
values: `int(float("0.015000") * 60 * 100)` is 89, not 90. With `%f`, 119 of the M220 percentages
10..300, 7 of the M221 percentages 10..200 and 125 of the 256 fan values come out one low; QIDI's
own screen has the same fault. The adapter writes `%f` raised by the fewest millionths that make
the parser send `round(value x scale)`.

**Cadence.** Every key at job start, then changed keys at most 1 Hz; event saves bypass the
throttle. One transaction for all changed keys through QIDI's in-process
`update_value_from_config_batch` (`executemany` in one `with conn:`), else one
`update_value_from_config` per key. QIDI's Moonraker deletes the section on complete, cancel and
standby, and on a job start while history still holds a current job
(`database.py#MoonrakerDatabase._on_job_state_changed`); the order of that handler against ours is
not fixed, so the job's first full write waits 2 s after the start (`FIRST_WRITE_DELAY_S`).

**Resume side** (§ 9 steps for `backend: "qidi_print_state"`).

1. `prepare`: the bed and chamber wait of § 6.3 step 2; nozzle untouched.
2. `commit`: the bed and chamber wait again (QIDI sends `M140` without `M190`, and the bed may have
   cooled during the confirmations; at temperature it returns at once), then rewrite every row in
   one transaction for the measured resume point (§ 6.2), then the helix-initiated marker, then
   `printer.print.power_off_print` (in-process `klippy_apis.power_off_print`); the marker is
   withdrawn if the call fails. Rows from the resume point's modal state: `gcode_position` X, Y and
   E (where QIDI travels and what it `G92 E`s), `absolute_coord`, `absolute_extrude`, `speed`.
3. `abort`: the rows stay, so the offer stands. `discard`: delete the `print_state` section.

**+4096 rule.** `file_position` row = `clamp(offset, 0, file_size) + 4096`. QIDI never checks the
row against the file size, so the row may exceed it, and QIDI's `max(row - 4096, 0)` lands exactly
on the offset, including in the file's last 4 KiB (a `min(offset + 4096, file_size)` clamp would
land up to 4 KiB early there).

**What the `gcode_position.z` row can and cannot fix (O8).** QIDI declares
`SET_KINEMATIC_POSITION Z=<row z>`, applies `SET_GCODE_OFFSET` and the mesh, and lands with
`G1 Z<row z>`. The declared value becomes the frame every later line prints in, so the rest of the
print is off by `(physical Z at the cut) - (row z)`.

- Fixed: the adapter writes the physical (kinematic) Z, `live_pos.z`, else `toolhead_pos.z`, else
  `gcode_pos.z + gcode_offset.z`. From the file's next Z move on, the print is in the right frame.
  The gcode-frame Z QIDI's own screen stores leaves the whole remaining print off by
  `gcode_offset_z + mesh(x, y at the cut)`.
- Not fixable through the row: the declaration and the landing move share it, so the rest of the
  resumed layer, until the file's next Z move, prints `gcode_offset_z + mesh(x, y at the cut)` high
  (typically 0.05-0.3 mm) whatever the row holds. QIDI also loads
  `BED_MESH_PROFILE LOAD={save_variables.profile_name}`, not a mesh from the rows, so an adaptive or
  different mesh adds its own error. A cut mid Z-hop declares the hop height.
- Measured in the Q2 M1 test (§ 11.4); if the remainder-of-layer error matters, the generic path is
  the fix.

**Limits of QIDI's endpoint.**

- Objects: with more than 64 objects QIDI clears all objects AND exclusions before resuming, so
  excluded objects print again. Object names go into `EXCLUDE_OBJECT_DEFINE NAME=<name>` unquoted.
- The file QIDI prints: every job runs from QIDI's `.cache` copy
  (`klippy_apis.py#KlippyAPI.start_print`), and a `.3mf` from the extracted
  `Metadata/plate_<n>.gcode` (`virtual_sdcard.py#VirtualSD._load_file`). `file_position` counts
  bytes of that file. For `.gcode` it matches the gcodes file; for `.3mf` the scan must run over
  plate `<n>` extracted from the 3MF (`.temp` is wiped on every Klipper start), and its size is the
  one the +4096 rule clamps to.
- The resumed job's name: QIDI opens `glob(".cache/*")[0]` and passes that absolute path to
  `print_stats`, so history records `/home/qidi/printer_data/.cache/<name>`. The marker is keyed on
  that path; history is not rewritten back to the gcodes name, and snapshot identity for the
  resumed job does not resolve under the gcodes root.

**Q2 checks still owed** (§ 11.4): the stock-screen row capture (formats here come from the
parser, not stored rows); the O8 first-layer height check with zero and non-zero Z offset (expect
only the remainder-of-layer error); the 2 s first-write delay against the start-time delete; the
resumed job's history filename; in-process behaviour of `update_value_from_config_batch` and
`power_off_print`.

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
- QIDI adapter: row formats against the #1716 table (`speed_factor` 0.016667 at 100 %), the +4096
  rule (clamp the offset into the file, then add 4096), offset < 4096.
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

### 11.3 Interruption methods

Most of what a power cut exercises can be produced without pulling a plug. Three methods, then
what only a real cut adds. Every run of M1 or M2 heats and moves a printer, so each run needs
Preston's OK, even when scripted over SSH.

**M1: host-stack kill.** SIGKILL Moonraker, then klippy, mid-print, in one SSH command
(`kill -9 <moonraker>; kill -9 <klippy>`), then restart both.

- *The MCU shuts down on host loss.* Heaters are PWM outputs with
  `setup_max_duration(MAX_HEAT_TIME)` (3 s) (`kalico/klippy/extras/heaters.py#Heater.__init__`).
  Once klippy stops scheduling updates, the MCU's own timer trips
  `shutdown("Missed scheduling of next ...")` (`kalico/src/gpiocmds.c`, `kalico/src/pwmcmds.c`).
  The shutdown handlers then drive every digital output to its default, which is off
  (`gpiocmds.c#digital_out_shutdown`), and stop stepping (`kalico/src/stepper.c#stepper_shutdown`).
  A dedicated stepper enable pin is a plain digital output (`kalico/klippy/extras/stepper_enable.py#setup_enable_pin`),
  so the driver is disabled and the motor stops holding [src].
- *MCU-config caveat: some steppers stay energized.* With no `enable_pin`, the stepper is "always
  enabled" (`setup_enable_pin`, `pin is None`). TMC drivers that enable through `toff` register
  writes need the host for that write (`kalico/klippy/extras/tmc.py#TMCCommandHelper._do_disable`).
  In both cases the driver keeps holding current after an M1 kill. **M1 says nothing about Z
  holding unpowered** unless every Z stepper in that printer's config has a dedicated `enable_pin`.
  Check `configfile.settings` before using a run for that question.
- *Moonraker marks the job interrupted, but only if Moonraker dies first.* On startup,
  `history.py#History.component_init` rewrites every `in_progress` job to `interrupted`. If klippy
  dies while Moonraker is still up, `History._handle_disconnect` finishes the job as
  `klippy_disconnect`, and the offer never fires (D1) [src: `~/Code/Printing/moonraker/moonraker/components/history.py`].
  So the kill order is Moonraker, then klippy. The plugin dies with Moonraker, so the snapshot is
  as stale as the gap between the two kills; keep that gap well under one save interval.
- *After the restart*, klippy refuses to configure an MCU that is shut down ("Can not update MCU
  ... config as it is shutdown", `kalico/klippy/mcu.py#MCU._send_get_config`) and needs a
  `FIRMWARE_RESTART`. That moves nothing. A real power cut resets the MCU on its own, so the
  procedure sends it before the offer.
- *Covers:* the offer path end to end, the whole resume sequence, Z holding unpowered (with the
  caveat above), adhesion after a wait, and T2.

**M2: host hard reboot without sync.** `echo b > /proc/sysrq-trigger` as root (may first need
`echo 1 > /proc/sys/kernel/sysrq`). The kernel reboots at once and drops dirty pages, the way a cut
does on the host side.

- Tests the slot file's durability and atomicity on a real filesystem: after the reboot, a valid
  slot exists and is no older than one save interval plus the reboot gap. Torn-slot handling
  itself is proven in pytest with injected truncation (§ 11.1), because a reboot rarely lands mid-write.
- Usually resets a USB-powered MCU as the bus drops. An MCU powered from the 24 V rail with
  USB data only may not reset; it then shuts down on host loss as in M1. The run log records which happened.
- Use sparingly: a few runs per storage class (eMMC, SD), never as the routine resume test.

**M3: no interruption.**

- The lead measurement (§ 6.1, O3): log `virtual_sdcard.file_position`, `toolhead.position` and
  `motion_report.live_position` at 4 Hz during a normal print, then run `locate_executed_line`
  offline against the logged frames.
- The scan and resume-point algorithm: the pytest fixtures (§ 11.1).
- T1 scan timing on an idle printer.
- The app offer and wizard in the `--test` mock, with an injected interrupted job. New mock knobs,
  documented in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` when built (part of the v1 app work):

| Knob | Values | Effect |
|---|---|---|
| `HELIX_MOCK_PLR` | `generic`, `qidi_print_state`; unset = off | Requires `HELIX_MOCK_HELIX_PLUGIN=1`. The mock's `server.helix.status` reports `plr` ready; the newest history job reads `interrupted`; `server.helix.plr.status` returns a snapshot and `offer.available: true`; `plan`/`prepare`/`commit` answer per § 9 and emit `helix:plr_progress`; `commit` starts the mock print at the plan's file fraction |
| `HELIX_MOCK_PLR_TIER` | `1`, `2` (default `2`) | Tier in the plan, so the wizard shows the jog step or the confirm step |
| `HELIX_MOCK_PLR_WARNINGS` | comma list: `mesh_unavailable`, `cold_bed`, `layer_start` | Plan warnings; `cold_bed` sets the bed current temperature 30 °C under target |
| `HELIX_MOCK_PLR_FAIL` | `plan`, `prepare`, `commit` | That RPC fails with `plr_klippy_error`, to drive the wizard's `Failed` step |

**What only a real cut adds**

- PSU hold-up and power-fail pin timing (the later power-fail input, § 13).
- Vendor firmware we do not own, e.g. the #1716 reporter's QIDI 01.01.02.03.
- A long outage's bed cool-down is approximated by a long wait after M1 (the MCU has already
  turned the heaters off), so a real cut is not needed for adhesion.
- Final confirmation: one or two real pulls on the Q2 once everything else passes.

### 11.4 Hardware matrix

| Device | Test | Method | Settles |
|---|---|---|---|
| QIDI Q2 (.85) | Stock screen: short print, copy `moonraker-sql.db` mid-print, `select * from config where section='print_state'` | M3 | QIDI row formats (#1716 report open question 4) |
| QIDI Q2 | HelixScreen + plugin: print, kill at layer ~10 during a wall, resume via the adapter | M1 | #1716 fix, offer path, bed wait |
| QIDI Q2 | **O8**: same run, measure the first resumed layer's height error with a feeler gauge and calipers against the layer below; repeat with a non-zero Z offset | M1 | O8: switch QIDI to generic if the error exceeds 0.1 mm |
| QIDI Q2 | Same, generic path | M1 | Ruling 6 re-evaluation |
| QIDI Q2 | Final: one or two wall-plug pulls via the chosen path | real cut | End-to-end confirmation |
| Q2, K1C, Voron V0 | Lead logging, 5 min of walls and infill | M3 | § 6.1 lead; walk line choice offline |
| K1C, CB1 Voron, Pi 4 (SD), CM4 eMMC | `storage_verdict` on real `/sys` | M3 | § 7.4 |
| eMMC host and SD host | 3 reboots each mid-print, slot file read back | M2 | Slot durability on real filesystems |
| K1C (slowest listed host) | **T1, first task of v1**: `scan_to_offset` over a 100 MB Orca file to its last byte, 3 runs, idle printer, plugin's real interpreter | M3 | **Done 2026-10-21: FAIL, 394 s median (§ 11.5).** Background per-layer checkpoints join v1 (O3) |
| One listed printer whose Z steppers all have dedicated `enable_pin`s, PLA, 0.2 mm layers | **T2**: 6 kills per setting, half during an outer wall and half during sparse infill, resumed with `resume_skip_fraction` 0 (re-trace) and 0.5 (skip-half); inspect each seam with a loupe and calipers for a ridge (height over the wall) or a gap (missing length) | M1 | O2 default: the setting with no gaps and the smaller ridge wins; a gap on any wall run rules that setting out |
| One tier-2 model (dedicated Z `enable_pin`s) | Z hold: kill, wait 10 / 30 / 60 min, measure Z drift with a dial indicator before resume; adhesion: same waits, PLA and PETG | M1 + wait | Z holds unpowered; adhesion; cold-bed threshold (O6) |
| Any eMMC host | `/sys/block/*/stat` sectors written over 1 h of printing, slot file vs SQLite | M3 | § 5.2 numbers |
| `--test` mock | Offer, wizard steps, tier 1 and 2, every `HELIX_MOCK_PLR_WARNINGS` and `_FAIL` value | M3 | App flow before any hardware run |

### 11.5 T1 result (2026-10-21)

**Verdict: fail.** Median 394 s against the 30 s bar, so background per-layer checkpoints join v1
(O3). Scan at resume time stays, but only from the current layer's checkpoint.

| Host | Interpreter | Runs (wall s) | Median wall | Median CPU | Peak RSS |
|---|---|---|---|---|---|
| K1C (Ingenic XBurst2 MIPS, 2 cores, 214 MB RAM, Buildroot 2020.02, eMMC `/usr/data`), printer idle in `standby` | Moonraker's own `/usr/data/moonraker-env/bin/python3`, CPython 3.8.2 | 395.0, 393.6, 394.1 | **394.1 s** | 388.1 s | 8.7 MiB (7.0-7.7 before the scan) |
| K1C, page cache dropped first (as after a reboot) | same | 403.1 | 403.1 s | 394.8 s | 8.7 MiB |
| thelio (Ryzen 9 9950X) | CPython 3.8.20 | 4.12, 4.31, 4.33 | 4.31 s | 4.31 s | 18.6 MiB |
| thelio | CPython 3.12.3 | 4.92, 4.83, 4.80 | 4.83 s | 4.82 s | 16.9 MiB |

The K1C is ~90x slower than thelio and CPU-bound (wall within 2 % of CPU; cold cache adds ~2 %).
No reducer change closes that gap: on the K1C, merely iterating the file's lines in Python takes
13.4 s, splitting every line 20.4 s, and testing every line for a layer marker 28.8 s. A follower
that runs the full reducer over a print costs the same ~390 s of CPU, spread over the print's hours.

File: `t1_eiffel_100mb.gcode`, 104,815,389 bytes, 5,162,780 lines, 6048 layers, sha1
`23014ecdacee3b915a7e9479f61b323bc8e7a4a0`. Built by `moonraker-plugin/tools/make_t1_gcode.py
assets/test_gcodes/eiffel_final_PLA_2h42m.gcode OUT --size-mb 100` (OrcaSlicer 2.4.2, M83,
`SET_PRINT_STATS_INFO`, no arcs): the start and config blocks once, the print body stacked 8 times
with Z raised and layer numbers continued, so the line mix is Orca's own (94 % `G1`, plus
`SET_VELOCITY_LIMIT`, `M106`, `;TYPE`/`;WIDTH`/`;WIPE` comments). Timed with
`moonraker-plugin/tools/plr_t1_scan_timing.py FILE --runs 3`, each run a fresh child process,
scanning to 1 KiB before the end.

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
line, and the mesh include status with an "add include" button (D2). The button, and the
installer prompt (O9), write the include line through the existing `KlipperConfigEditor`
(`include/klipper_config_editor.h`, umbrella Phase 10's fallback), not a new writer. Writes
`plr_user`; the app rewrites `plr_profile`.

---

## 13. Phasing

**v1**

- First: implement umbrella Phase 0 (features map, `plugin_feature()`, own-switch rule, the
  helix-initiated job marker, the Python 3.8 CI job).
- T1 (§ 11.4): measured 2026-10-21, failed (§ 11.5), so background per-layer checkpoints are in
  v1: the follower writes the layer_index sidecar through `helix_gcode_reader.py`
  (`scan_to_offset(..., throttle=RateLimiter(...).pace, on_layer_start=...)`).
- `helix_gcode_reader.py`: the shared reader (§ 3.3), built here and reused by umbrella Phase 1
  (built 2026-10-21; `moonraker-plugin/tests/test_helix_gcode_reader.py`).
- `helix_plr.py`, `helix_plr_scan.py`: snapshot loop and slot store, storage verdict, offer,
  scan and measured resume point with layer-start fallback, generic sequence for tiers 1 and 2,
  single extruder, saved and include-file mesh, all endpoints in § 9, the `plr` feature entry.
- QIDI-new adapter, `helix_plr_qidi.py`.
- T2 before the `resume_skip_fraction` default is final.
- App: GENERIC backend, wizard, settings, profile resolution, DB `plr` blocks for the models
  tested in § 11.4.

**Later**

1. Tier 3: probe at a part-free spot (`SET_KINEMATIC_POSITION` high, move, `PROBE`, correct the
   frame; Klipper refuses `PROBE` until Z is homed [src: `kalico/klippy/extras/probe.py`]); Z-max
   homing.
2. Power-fail input: a `gcode_button` on a PSU/UPS detect pin triggers an immediate save.
3. Multi-material and toolchangers through the filament backends (D3).
4. Resume from layer N after a shutdown or a failed print (D1), reusing scan and sequence.
5. Umbrella Phase 1's `layer_index` (built on the same reader) to skip the start-of-file scan
   when T1 passed and no follower runs (O3).
6. A Klipper extra, only if the measured walk fails on real files (D4).

---

## 14. Open questions

| ID | Question | Recommendation |
|---|---|---|
| O4 | Raw NAND (UBIFS) hosts: wear-tolerant only when the DB says so, or never? | Only when the DB says so |
| O8 | QIDI Z residual (offset + mesh): accept, or switch QIDI to the generic path? | Decided by the Q2 M1 test (§ 11.4): switch if the first resumed layer is off by more than 0.1 mm |

## 15. Preconditions before execution

- Umbrella Phase 0 implemented (v1's first task): `PluginFeatureState` and `plugin_feature()`
  exist, the `features` map is served, the helix-initiated marker exists, the Python 3.8 CI job
  exists.
- The § 12.1 C++ block passes `scripts/syntax_check.py` once Phase 0's `PluginFeatureState` exists
  (it cannot be compiled before). The § 7.2 and § 12.2 blocks and the Python blocks in § 6.2,
  § 6.5, § 7.3, § 7.4 and § 8 were checked on 2026-10-21: the two C++ blocks compiled together
  with `scripts/syntax_check.py`; the Python blocks parsed with `ast.parse(feature_version=(3, 8))`
  and `py_compile`. That checks 3.8 grammar only; no 3.8 interpreter was available, so stdlib
  API use is left to the 3.8 CI job.
- T1 measured: done 2026-10-21, K1C median 394 s, fail; checkpoints join v1 (§ 11.5).
