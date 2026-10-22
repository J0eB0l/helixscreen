# Power-Loss Recovery (PLR)

HelixScreen offers to resume an interrupted print at connect time. Three printer
firmware families expose a recovery mechanism, and they work in fundamentally
different ways: two are **passive** (a status field tells us a snapshot exists),
one is **active** (we have to ask, and asking has side effects). A fourth backend,
**GENERIC**, is the HelixPrint plugin's own recovery for any Klipper printer listed
for it: the plugin keeps the snapshot and renders the resume, and the app walks
the user through a guided wizard.

Code map:

| File | Role |
|------|------|
| `include/plr_backend.h`, `src/printer/plr_backend.cpp` | Pure backend strategy: capability selection, probe-response parsing, resume/discard plan building. No LVGL, no network. |
| `include/plr_offer.h`, `src/printer/plr_offer.cpp` | Pure offer decision (`plr_should_offer`) and latch re-arm rule (`plr_should_rearm`). |
| `include/plr_offer_controller.h`, `src/ui/ui_plr_offer_controller.cpp` | App-lifetime controller. Observers, one-shot latch, the Creality probe, normalization of the backends into one "recovery available" signal. |
| `src/ui/ui_plr_prompt.{h,cpp}` | The modal and its two button actions. Backend-agnostic — it executes a `PlrRecoveryPlan`. |
| `src/printer/printer_print_state.cpp` | Parses the capability markers out of the status payload. |
| `src/api/moonraker_api_print.cpp` | `check_continue_print_state()` / `cancel_continue_print()` JSON-RPC calls. |
| `include/plr_wizard.h`, `src/printer/plr_wizard.cpp` | GENERIC: readers for the plugin's `server.helix.plr.*` replies and the wizard's pure decisions (bed state, confirmations, commit gate, step machine). |
| `include/plr_plugin_rpc.h`, `src/printer/plr_plugin_rpc.cpp` | GENERIC: the plugin's endpoints as calls. |
| `include/ui_plr_wizard_overlay.h`, `src/ui/ui_plr_wizard_overlay.cpp`, `ui_xml/plr_wizard_overlay.xml` | GENERIC: the resume wizard. |
| `include/plr_profile.h`, `src/printer/plr_profile.cpp` | GENERIC: the printer database `plr` block, its resolution with the user's overrides, and the `plr_profile` / `plr_user` records. |
| `src/api/moonraker_client_mock_plr.cpp` | The mock plugin's endpoints (`HELIX_MOCK_PLR*`, `MOCK_ENVIRONMENT_VARIABLES.md`). |

Tests: `tests/unit/test_plr_backend.cpp`, `test_plr_offer.cpp`, `test_plr_state.cpp`,
`test_plr_prompt.cpp`, `test_firmware_error_envelope.cpp`, and for GENERIC
`test_plr_wizard.cpp`, `test_plr_wizard_overlay.cpp`, `test_mock_plr.cpp` and the
plugin cases in `test_plr_offer_preparing.cpp` (tag `[plr]`).

Backend precedence in `plr_select_backend()`: SNAPMAKER first, then GENERIC for
the plugin's QIDI `print_state` adapter, then QIDI, then CREALITY, then GENERIC
for the plugin's own recovery. No firmware carries more than one marker set, so
among the three firmware backends the order only makes a hypothetical tie
deterministic - and it puts both passive signals ahead of the probe-based one, so
a marker chosen by mistake never costs a side-effectful probe. Both GENERIC rows
need the plugin's `plr` feature Ready; see "Generic (HelixPrint plugin)" for why
they sit where they do.

---

## The three backends

### Snapmaker U1 — passive

Snapmaker's Klipper fork adds `virtual_sdcard.pl_env_valid`. The firmware
validates a coherent power-loss snapshot against MCU flash during boot and sets
the flag itself. Mainline Klipper has no such field, and our parser only accepts
a JSON boolean, so the flag is **self-gating**: `pl_env_valid == true` already
means "Snapmaker firmware with a valid recovery snapshot".

No printer-model or AMS-backend gate is needed, and none should be added — an
earlier redundant "AMS backend == SNAPMAKER" gate wrongly suppressed the offer
on an AFC-modded U1 (bundle UDZJQVQZ) whose backend is not the Snapmaker one.
`tests/unit/test_plr_offer.cpp` pins that regression.

| | |
|---|---|
| Capability + availability | `virtual_sdcard.pl_env_valid == true` |
| Recovery filename | `virtual_sdcard.file_path` |
| Resume | gcode `SDCARD_PRINT_PL_RESTORE` |
| Discard | gcode `SDCARD_PRINT_PL_CLEAR_ENV` |
| Errors | JSON-coded (`{"coded": ..., "msg": ...}`); `MoonrakerError::from_json_rpc` decodes it through `include/firmware_error_envelope.h` |

### Qidi Q2 / Q1 Pro / Plus 4 - passive

Qidi's stock firmware keeps its recovery state in a save variable. The stock
macros maintain it: `PRINT_START` runs `save_last_file`, which sets
`SAVE_VARIABLE VARIABLE=was_interrupted VALUE=True`; a normal end or cancel runs
`CLEAR_LAST_FILE`, which sets it back to False. After power loss the print never
reaches either, so `save_variables.variables.was_interrupted` reads true at boot.
That is the whole signal - nothing is probed and nothing is asked.

Two halves gate the backend, because the variable name alone proves nothing:

* **Capability**: discovery found the `DETECT_INTERRUPTION` or `RESUME_INTERRUPTED`
  macro (`plr_resume_macro_present()`, `PrinterDiscovery::has_macro`). Newer
  firmware (Q2 01.01.02.03 at least) registers `RESUME_INTERRUPTED` and `CLEAR_LAST_FILE` as Python
  commands, invisible to discovery, so `DETECT_INTERRUPTION` is the macro every
  generation carries. Either identifies Qidi's stock firmware; `was_interrupted` is a user-writable name on ANY
  Klipper, so the variable alone must never select the backend.
* **Availability**: `save_variables.variables.was_interrupted` arrived as a JSON
  boolean and is true.

| | |
|---|---|
| Capability | `DETECT_INTERRUPTION` or `RESUME_INTERRUPTED` macro present (discovery) |
| Availability | `save_variables.variables.was_interrupted` is JSON boolean `true` |
| Recovery filename | `virtual_sdcard.file_path` when non-empty; the prompt falls back to its generic body when empty (no filesystem scan) |
| Resume | gcode `RESUME_INTERRUPTED` (rebuilds the resume gcode from the `.temp/` backup, lifts Z clear of the part before homing X/Y, then prints it) |
| Discard | gcode `CLEAR_LAST_FILE` |
| Errors | plain Klipper error strings; the prompt surfaces the message unchanged |

Two disciplines the parser owes the delta protocol, same shape as the Creality
capability latch:

1. **Booleans only.** `save_variables` values are Python literals Klipper
   re-parses; another type under this name is not our signal. A non-boolean
   value is not-available, and also never a clear: after a `true`, a `1` or
   `"True"` arriving later must not drop the flag.
2. **Deltas must not clear.** Moonraker notifies at top-level-field
   granularity, so a frame without `save_variables`, or a `variables` dict
   without the key, leaves the flag alone. Only a boolean `false` (the
   `CLEAR_LAST_FILE` discard path) or the disconnect edge (the offer controller
   forces the subjects to 0, like `pl_env_valid`) resets it.

The flag is ALSO true during every normal print - it stays true from
`PRINT_START` until the job ends. That is by design of the stock macros, and the
offer decision's `printer_idle` input is what scopes it to a boot after power
loss: mid-print the printer is not idle, so no offer fires. The subscription
(`save_variables` when the macro exists) is added in
`MoonrakerDiscoverySequence::build_subscription_objects()` via the
`plr_required_status_objects()` capability question - no vendor name reaches the
subscription builder.

### Creality K1 / K1C / K1 Max / K2 Plus / Ender 3 V3 / Hi / i7 — active

Verified by reading the Klipper source on a physical K1C and K2 Plus and by
running the endpoint live against an idle K1C.

Creality's fork registers a Klipper **webhook endpoint**,
`pause_resume/check_continue_print_state`. Moonraker auto-registers every
non-reserved Klipper webhook endpoint (klippy_connection.py:387-396), so it is
reachable both ways:

* HTTP: `POST /printer/pause_resume/check_continue_print_state`
* JSON-RPC: `printer.pause_resume.check_continue_print_state`

HelixScreen uses the JSON-RPC form — we already hold the WebSocket.

Live response on an idle K1C:

```json
{"result": {"file_state": false, "eeprom_state": false}}
```

A recovery is available **only when both `file_state` and `eeprom_state` are
true**.

| | |
|---|---|
| Capability | presence of `print_stats.power_loss` (see below) |
| Availability | one-shot probe returns `file_state && eeprom_state` |
| Recovery filename | sidecar JSON, `<base>/creality/userdata/config/print_file_name.json`, key `file_path` |
| Resume | gcode `SDCARD_PRINT_FILE FILENAME="<file>" ISCONTINUEPRINT=1` — quoted, and `<file>` **relative to the sdcard root** (both are load-bearing; see "Filename safety") |
| Discard | JSON-RPC `printer.pause_resume.cancel_continue_print` |

`<base>` is `/usr/data` on K1-class and `/mnt/UDISK` on the OpenWrt-class K2.
HelixScreen runs on the printer, so the read normally succeeds; it is
best-effort and **read-only** — never write to that path.

### Generic (HelixPrint plugin) - asked

The HelixPrint plugin (`moonraker-plugin/`, design in
`docs/devel/plans/2026-10-21-plugin-power-loss-recovery-design.md`) saves its own
snapshot during every print on printers listed for it, decides itself whether a
resume can be offered, and renders and sends the resume. The app gates on the
plugin's `plr` feature (`plugin_feature("plr", 1)` Ready), asks for the plugin's
verdict, and owns only the confirmations a screen can ask.

| | |
|---|---|
| Capability | `server.helix.status` `features.plr` at version 1 or newer, available and enabled |
| Availability | `server.helix.plr.status` `offer.available` (the plugin checks the newest history job is `interrupted`, a snapshot exists for it, the file is unchanged, the job got far enough, the tier allows it, Klipper is ready); `backend` says which flavor |
| Recovery filename | `snapshot.filename` |
| Resume | the guided wizard (below); nothing is sent at the offer |
| Discard | JSON-RPC `server.helix.plr.discard` |
| Errors | Moonraker errors whose message leads with the plugin's code (`plr_stale_plan`, `plr_missing_confirmation`, `plr_klippy_error`, ...); the wizard shows the message |

**Where it sits in the precedence.** The plugin's `qidi_print_state` backend serves
the QIDI firmware layout the legacy QIDI macros also live on, and its resume
corrects the firmware's file offset where the legacy one does not, so it outranks
QIDI. The `generic` backend comes last, so it never displaces a firmware's own
recovery. Both select `PlrBackendType::GENERIC`; the plan's `backend` field says
which one is running.

**When the status is asked.** `server.helix.plr.status` is side-effect free. The
offer controller asks it once per connection, when the link is up, Klipper reports
`ready` and the feature is Ready; the plugin-installed subject (set after the
features map), the Klipper state and the connection state each trigger the check,
so whichever arrives last asks. An answer blocked by `klippy_not_ready` is not
kept, so the next Klipper-ready edge asks again. The disconnect edge drops the
answer with the other latches.

**The profile and the switch.** The plugin acts on two Moonraker database records
in the `helix-screen` namespace, so it has them with the screen off:

- `plr_profile`, which the app resolves and writes: `{"tier", "backend",
  "storage_hint", "z_holds_unpowered", "xy_home_gcode", "z_lift_mm",
  "purge_gcode", "resume_skip_fraction", "macro_vars"}`. `plr_resolve_profile()`
  folds the user's overrides into the printer database's `plr` block
  (`PrinterDetector::get_plr_block()`). A printer with no block is tier 0, which
  offers nothing (ruling 7). Tier 2 needs the database's `z_holds_unpowered`
  whoever asks for it, so without it the tier is at most 1, a user override
  included. A user may lower the tier, or raise it only where X/Y homing is known
  safe or overridden; the app does not read `homing_override`, so it passes the
  homing check as unknown and no raise passes it (the plugin checks the database
  tier against configfile itself). `PlrOfferController::publish_profile()` writes
  the record once per connection and printer type while the `plr` feature is
  available at all (Disabled counts: a missing profile is what keeps saving off),
  and only after this connection's plugin probe has answered: discovery resolves
  the printer type before that probe, so the type is this printer's, never the
  previous one's. It reads the stored record first and writes only when it
  differs, so several screens on one printer do not rewrite it in turn. After a
  write it re-reads the plugin's features map, since the plugin's saving verdict
  follows the profile.
- `plr_user` = `{"enabled": bool}`, the only record the plugin reads for its
  saving switch. Settings > Printing > Extras > **Power-loss recovery** writes it;
  the row shows while the feature is available (`plr_feature_state` subject, 0
  Absent / 1 Disabled / 2 Ready) and its switch follows `plr_saving_enabled`, the
  plugin's own report re-read after each write: the `plr` entry's `saving`, or
  its `enabled` from a plugin that reports no `saving`. The entry's `enabled`
  also holds while a saved snapshot waits, so the offer (gated on Ready) survives
  saving being switched off.

Listed printers: the Qidi Q2 (`backend: qidi_print_state`, eMMC, tier 2). Tier 2
replicates QIDI's own stock resume, which already trusts the Z the steppers held;
it is unverified until the Q2 power-cut test in the design's § 11.4. The K1 line stays unlisted: Creality's
own recovery serves it and outranks the generic backend (ruling 8).

**The layer index.** The plugin's follower (`helix_plr.py#LayerFollower`) builds the
printing file's layer index while the print runs, so a resume scan starts at a layer
instead of byte 0. It is the same `.helix_index/<path>.layers.jsonl` file that file
analysis (`helix_analysis.py`) writes for every G-code file on upload, in the format
`helix_gcode_reader.py` documents; whichever writes first, the other uses it:

- **One identity.** `helix_plr.file_identity` / `file_hash` are
  `helix_gcode_reader.file_identity` / `file_hash` (size, mtime, sha1 of the first and
  last 64 KB): the snapshot, the index header and the screen's preview check one rule.
- **One writer per index.** Analysis writes a whole index to a `.tmp` file and renames it
  over the old one. The follower appends, so a power cut leaves a valid prefix; its
  `LayerIndexWriter` writes nothing once a complete index for the same identity exists,
  and a follower already appending when analysis's rename lands keeps writing to the file
  it opened, so a complete index is never touched.
- **One prune rule.** `helix_gcode_reader.prune_layer_indexes(root, protected)` removes an
  index whose G-code is gone or no longer matches the header's size or mtime, or that
  indexes a file outside gcodes (`.external`), with no count cap. `protected` comes from
  PLR's state, never from file times: `HelixPlr.protected_index_paths()` names the
  follower's index during a print and the index of the job a waiting snapshot resumes.
  PLR prunes at each job start; analysis prunes at startup and asks PLR for the same paths.
- **The worker runner.** `helix_gcode_reader.WorkerProcess` runs a whole-file reader in a
  niced child process (Moonraker's own interpreter, `ionice -c 3` where present, a
  timeout, kill on shutdown or when a print starts on a one-core host). Analysis uses it;
  the follower still runs on its own duty-cycled thread.

**The wizard.** Resume on the offer opens `PlrWizardOverlay`, whose steps are
`plr_next_wizard_step()` (`include/plr_wizard.h`):

| Step | What happens |
|------|-------------|
| Offer | `status`, then `plan`. A long scan answers `pending` and follows with `helix:plr_plan_ready` (delivered as `notify_plr_plan_ready`); the wizard asks `plan` again then. A `plan_ready` carrying `error`, or a plan still `pending` after `plan_ready`, fails the wizard with that message instead of starting the scan over |
| Checks | File, layer, Z, outage age and bed reading from the plan; the confirmations asked before anything moves: `part_attached`, plus `nozzle_clear` at tier 1, `accept_no_mesh` for a `mesh_unavailable` warning, `accept_next_layer` for a `layer_start` warning, `accept_cold_bed` when the bed has cooled more than `kPlrColdBedDiffC` below target, and anything else in the plan's `requires` |
| Preparing | `prepare`: steps `bed_wait`, `chamber_wait`, `lift`, `home_xy`, and at tier 1 `position` (over the resume point). `helix:plr_progress` (`notify_plr_progress`, `{plan_id, step, state}` with state `running` / `done` / `failed`) names each; the wizard shows the `running` step and the live bed temperature |
| ZStep | Tier 2 confirms the height the steppers held (`z_confirmed`). Tier 1 jogs the nozzle down to touch the part (`z_jogged`), 0.05 / 0.1 / 1 mm steps through the motion panel's coalesced relative move; Resume stays disabled until a downward jog has landed (the part top is where the user lowered the nozzle to), and waits while a jog is still moving |
| Committing | `commit` with every confirmation given and `z_reference` `part_top` (tier 1) or `trusted` (tier 2): steps `z_reference`, `restore`, `heat_nozzle`, `move_to_resume`, `start`, with `bed_wait` / `chamber_wait` again when it re-heats; the print starts at the resume offset |
| Done / Failed / Aborted | Leaving the wizard after `prepare` started (Cancel, back, closing it, a failed RPC) sends `abort`: the plugin stops, turns the heaters off and keeps the snapshot. A screensaver, display sleep or a view pushed on top (Print Status opens as the print starts) is not leaving: the sequence keeps running. Entering any step drops queued jogs, and a jog ack after the Z step sends nothing more |

Two safety properties are pinned by the tests: the wizard never commits on its
own (Committing is reached only from ZStep with every required confirmation, and
a dropped connection fails the flow from every live step), and the only motion
the app sends itself is the tier-1 Z jog. A plan whose `requires` names a
confirmation this app does not know fails at the offer rather than at commit.

---

## Capability detection: `print_stats.power_loss`

`power_loss` is a `print_stats` field that exists **only** in Creality's fork.
Mainline Klipper has no such key. So the marker is the **presence** of the key,
not its value — it normally reads `0` and only becomes `1` after the probe.

Two traps:

1. **Presence must mean "present *and* numeric".** We subscribe to a narrowed
   field list, and Moonraker answers a subscribed-but-unpopulated field with an
   explicit `null`. So on mainline Klipper the key *is* present in the payload,
   as `null`. `is_number()` is what actually discriminates the fork.
2. **The capability latches up and never down.** Moonraker sends deltas; a later
   `print_stats` notification carrying only `print_duration` has no `power_loss`
   key at all. Clearing on absence would produce a spurious 1→0→1 edge and
   re-fire the side-effectful probe. Capability is reset only on the disconnect
   edge, by the offer controller.

Subscription: `power_loss` is in the `print_stats` field list in
`MoonrakerDiscoverySequence::build_subscription_objects()`.

---

## SAFETY: the probe must precede the resume

**This ordering is mandatory, not stylistic.**

The stock Creality sensorless-homing macro gates its pre-homing Z clearance lift
on:

```jinja
{% if printer.print_stats.z_pos|float <= 20.0 or printer.print_stats.power_loss == 1 %}
    ... big lift ...
{% else %}
    ... 0.1mm lift ...
{% endif %}
```

After a power loss high up in a tall print, `z_pos` correctly reads e.g. 150. So
**without** `power_loss == 1` the machine lifts only 0.1 mm and then
sensorless-homes X/Y — dragging the nozzle straight through the part.

Calling `check_continue_print_state` is what sets `print_stats.power_loss = 1`
(and only when both states are true and `print_stats.state == "standby"`).

> **Never issue the Creality resume gcode unless the probe has been made this
> connection and returned both states true.**

This is enforced in code, not by comment:

* `PlrDetectResult::completed` is set **only** by
  `plr_parse_check_continue_response()` on a well-formed
  `result.{file_state,eeprom_state}` boolean pair.
* `plr_build_plan(CREALITY, file, detect)` returns an **empty `resume_gcode`**
  unless `detect.completed && detect.file_state && detect.eeprom_state`.
* `PlrOfferController` refuses to show the prompt when
  `plan.resume_allowed()` is false, and does not latch the one-shot.
* The prompt's Resume handler re-checks `plan.resume_allowed()` before sending.

Pinned by `tests/unit/test_plr_backend.cpp` — "CREALITY refuses resume when the
probe never ran" and its half-confirmed siblings.

### Second footgun: the EEPROM gate

The Klipper-side resume branch requires the `bl24c16f` EEPROM object. If it is
absent, `ISCONTINUEPRINT=1` **silently starts the print from the beginning**
rather than failing. The `file_state && eeprom_state` gate covers this. Do not
weaken it to `file_state` alone.

### Probe discipline

`check_continue_print_state` is **side-effectful**:

* on JSON parse failure it **deletes** the recovery sidecar;
* it clears `exclude_object_info` when the state is false;
* it sets `print_stats.power_loss = 1` when both states are true.

Therefore the probe is issued:

* **at most once per connection** (latched by `creality_probed_this_connect_`,
  re-armed only on a CONNECTED → not-CONNECTED edge);
* **only when `print_stats.state == "standby"`**;
* **never polled**.

---

## Normalization and reuse

All four backends collapse into one internal signal so the existing latch /
re-arm / wizard logic is reused unchanged:

```
Snapmaker:  pl_env_valid subject ──────────────────────┐
Qidi:       DETECT_INTERRUPTION macro + was_interrupted ┤
Creality:   power_loss key ──> one-shot probe ──> both ─┤──> recovery_available ──> plr_should_offer()
                               (standby only)    states │
Plugin:     plr feature Ready ──> plr.status ──> offer ─┘
```

`PlrOfferSignals::recovery_available` is that normalized field.
`plr_should_offer()` stays pure — no LVGL, no threading, no singletons — and its
other three inputs (`printer_idle`, `already_prompted`, `wizard_active`) are
unchanged.

The `printer_idle` input comes from the derived lifecycle, not the wire: the
controller computes `idle = !job_holds_machine(get_print_lifecycle())`
(`src/ui/ui_plr_offer_controller.cpp#idle`). During a host-side pre-print block
`print_stats` still reads `standby` — it describes the previous job — so an
idle check derived from the wire would offer "Resume interrupted print?" on top
of a start the user has already committed to, and the Resume button would start
a different file than the one they chose. Anchoring idle to the lifecycle means
a committed start never gets the offer. (`PRINT_STATE_MACHINE.md` § "Asking
whether a job owns the machine" covers the predicate and why `print_active` is
the wrong signal for this question.)

The one-shot latch, the wizard-close re-evaluation, and the disconnect re-arm
are documented at the decision site, `PlrOfferController::evaluate_offer()`.

### Why the prompt carries a plan

`show_plr_recovery_prompt()` takes a `PlrRecoveryPlan` by value. The plan owns
its strings, so the button handlers never re-derive the backend or re-check the
invariant against live state that may have moved on between the offer and the
tap. The modal itself stays backend-agnostic: primary button runs
`plan.resume_gcode`, or opens the wizard when the plan carries a
`plan.resume_rpc` (GENERIC); secondary runs `plan.discard_gcode` **or**
`plan.discard_rpc_method`.

### Filename safety

The Creality resume gcode embeds the filename as an extended-command parameter,
and **it must be double-quoted**:

```
SDCARD_PRINT_FILE FILENAME="/usr/data/printer_data/gcodes/My Part_PLA.gcode" ISCONTINUEPRINT=1
```

Klipper tokenizes extended parameters with `shlex` in POSIX mode
(`whitespace_split=True`, `commenters="#;"`), then splits each token on the
first `=`. An unquoted filename containing a space therefore produces tokens
with no `=` at all, and the whole command is rejected before the file is ever
looked up:

```
{"code":"key514", "msg": "Malformed command args 'SDCARD_PRINT_FILE FILENAME=/usr/data/
 printer_data/gcodes/PTOP Phone Stand_Elegoo PLA Matte Slate Grey_1h55m.gcode
 ISCONTINUEPRINT=1 ; from helixscreen'", "values": ["not enough values to unpack
 (expected 2, got 1)"]}
```

Spaces are the common case, not the exotic one — slicers put the model and
filament names in the filename. Moonraker's own print-start path quotes it
identically (klippy_apis.py, `SDCARD_PRINT_FILE FILENAME="{filename}"`).

Inside the quotes, `\n`, `\r`, `;`, `#`, `*`, `=`, `"` and `\` are still
rejected: `"` closes the value early, `\` is shlex's POSIX escape, and the rest
are comment/terminator characters that older regex-path Klipper forks strip
before shlex ever runs. `plr_is_safe_recovery_filename()` enforces that;
`MoonrakerAPI::is_safe_gcode_param()` is the wrong helper here because it
rejects whitespace. `IMoonrakerAPI::is_safe_material_param()` allows whitespace
but is the wrong helper too, and in the other direction: it is an **allowlist**
(alphanumerics plus `+ - _ . ( ) /`), while a slicer filename legitimately
carries commas, brackets, percent signs and non-ASCII. `kGcodeParamBreakers`
is the **blocklist** form of the same idea - `\n\r;#*="\` - which is what a
free-form filename needs. Same shlex mechanism, three call-site-specific
charsets; the material one is in `FILAMENT_MANAGEMENT.md` § "Material names as
G-code parameter values".

> A trailing ` ; from helixscreen` comment would ALSO break this command, and
> for a non-obvious reason: Creality special-cases `SDCARD_PRINT_FILE` to use a
> second regex (`extended_r1`) whose terminators are `|` and `*`, *not* `;` and
> `#` — "Support filename contain '#'". So the `;` survives into
> `shlex.split()`, which is called without `comments=True` and therefore treats
> `;` as an ordinary character. HelixScreen no longer annotates outgoing G-code
> at all (`src/api/moonraker_gcode_guards.h`), so this is historical — but it is
> why the field report showed both faults in one line.

### Path form: relative, not absolute

The sidecar stores an **absolute** path (`/usr/data/printer_data/gcodes/…`),
because Klipper writes it from the opened file handle's `.name`.
`SDCARD_PRINT_FILE` cannot be given that path:

```python
if filename[0] == '/':
    filename = filename[1:]          # cmd_SDCARD_PRINT_FILE
self._load_file(gcmd, filename, check_subdirs=True)
```

`_load_file` looks the name up in `get_file_list()`, whose entries are relative
to `sdcard_dirname` (`/usr/data/printer_data/gcodes` on K1-class). A
de-slashed absolute path is not in that list, so the lookup raises and the user
gets `{"code":"key121", "msg": "Unable to open file"}`.

There is a second, quieter reason. Once the file is open, the resume branch
does:

```python
if result.get("file_path", "") == self.current_file.name:
    sameFileName = True
else:
    os.remove(self.print_file_name_path)   # recovery data discarded
```

`current_file.name` is `os.path.join(sdcard_dirname, <relative name>)`. Sending
the relative name is what makes that comparison succeed; anything else that
still manages to open would **delete the recovery snapshot and print from the
beginning**.

`plr_creality_sdcard_relative_name()` does the stripping — known data roots
first (`/usr/data`, `/mnt/UDISK`), then the first `/gcodes/` segment as a
fallback for a relocated `virtual_sdcard: path`.

Verified by reading klippy/extras/virtual_sdcard.py and klippy/gcode.py on a
physical K1C.

With no resolvable filename there is no safe command to send, so the offer is
suppressed entirely rather than showing a Resume button that cannot work.

---

## Threading

Status notifications arrive on the libhv WebSocket thread, but
`MoonrakerManager` queues them and its main-thread drain calls
`update_from_status()`, so the capability parsers (which only mutate an
already-initialized `lv_subject_t` int in place) execute on the main thread,
matching the surrounding code in `printer_print_state.cpp`.

`PlrOfferController`'s observers are registered with `observe<int>`, which
defers through `UpdateQueue`, so every callback body runs on the main thread.
The one place that genuinely crosses the boundary is the probe response — a
JSON-RPC callback that arrives on the WebSocket thread — so it is wrapped in
`AsyncLifetimeGuard::bg_cb()`. The plugin's status reply takes the same path,
and carries the connection generation it was asked in, so a reply from a dropped
link is ignored.

The wizard's replies, progress notifications and jog acks also return through its
`lifetime_.bg_cb()`, each tagged with the opening it was asked in: a reply that
lands after the wizard closed, or after it was reopened, is dropped.
