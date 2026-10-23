# Print Status Redesign: Printer Picture + Two Phases

Design for reworking the print status panel. Two layout files exist and both get rewritten:
`ui_xml/print_status_panel.xml` and `ui_xml/portrait/print_status_panel.xml`. 480x272 and
272x480 have no variant file of their own. They are handled inside those two by measured
tile size, not by new `micro/` files. Mockups for every size live in the
brainstorm companion (`.superpowers/brainstorm/3044032-1792793919/content/d3-lines-v2.html`,
local, gitignored); the chosen direction is called **D3** there.

## Why

The right column above the buttons is crowded and inconsistent: an outlined temp card,
then loose text rows (speed/flow, tool changes + filament, three fans) with no shared
visual language, plus a dead gap that changes with the breakpoint. Mid-print the temp card
spends its room confirming numbers that stopped changing minutes ago.

What the user actually does on this screen: **verify everything is doing what it should, and
drill in to override or see detail.** They want to see at a glance when speed or flow has
been tuned without opening Tune. Inspiration is Bambu's printer-picture-with-temps layout,
and our own home-page printer image widget, which already does callouts.

## Goals

- One visual language: tiles with chips, no mix of outlined card and bare text rows.
- The printer picture carries the live state: every heater, fan, the chamber and the light is
  a chip placed where it is on the printer, with an active-region glow.
- Two phases. **Pre-print** shows the start sequence and the heat-up. **Printing** shows the
  live preview and the steady-state readouts.
- Lose no information the current screen shows (list under "Information inventory").
- Every chip is a tap target that drills into its detail/override.
- Works at every supported size, landscape and portrait, down to 480x272 and 272x480.

## Non-goals

- No change to the Tune overlay, the temperature/fan detail overlays, or the preview card's
  gcode render, 3D toggle and exclude-object button. The one change inside the preview card:
  its own `preparing_visible` bindings (the dim and `preparing_overlay`) give way to the
  checklist tile.
- No new printer art. Untagged images degrade (see "Fallbacks").
- No change to what Pause/Cancel/Reprint do.

## Anti-goals (a failure even if it works)

- Information the user can see today disappearing with no route to it.
- Chips that are unreadable over the picture.
- Leader lines that tangle on small screens.
- A second, hand-written copy of the home widget's callout layout logic.

## Layout

Three regions on every size:

1. **Job tile** (left in landscape, top in 480x800 portrait).
2. **Printer tile** (right in landscape, middle in 480x800 portrait).
3. **Strip**, full width at the bottom: progress text on the left (or on top when stacked),
   then the buttons **Pause, Tune, More, Cancel**. Cancel becomes **Reprint** when the print
   has an outcome, as today.

Per size (content boxes measured with `ctl geom` from the running app):

| Size | Content box | Arrangement |
|---|---|---|
| 1024x600, 800x480 | 920x528, 724x414 | tiles side by side; strip ~128 px tall at 800x480, buttons right |
| 480x320 | 426x279 | side by side; strip ~78 px |
| 480x272 | 438x224 | side by side; strip ~66 px; checklist compact form |
| 480x800 | 480x610 | stacked: job ~236, printer ~196, strip with progress above a row of 4 buttons |
| 320x480, 272x480 | 304/256 x 346 | tiles side by side (~150x190 each); strip stacks progress over buttons; checklist compact form |

Ultrawide (1920x480 / 1920x440) is not mocked. Builder's call, keeping the three regions;
the natural shape is job tile, printer tile and strip as three columns.

## Phase 1: pre-print

Shown while the existing `preparing_visible` is 1.

**Job tile = start-sequence checklist** over the slicer thumbnail. The thumbnail sits large
on the right of the tile and fades under the list through a left-to-right gradient. Rows:

- finished steps: green check, name, real duration
- current step: amber, bold, spinner, countdown
- upcoming steps: muted outline circle, estimated duration

The steps and estimates come from what already exists: `scan_start_block()` /
`StartBlockFollower` for start sequences inline in the file, `print_start_profile` for
macro-driven ones, `PreprintPredictor` history for durations. When nothing predicts the
upcoming steps (a first print through an unknown macro), show the finished steps and the
current one only. Never invent future steps, on any path. *(my call)* If no single model
merges steps, status and durations today, the builder adds one as a pure function feeding
subjects. The list has a fixed maximum number of rows declared in XML (8), each bound to
per-row subjects and hidden past the count, so no C++ row creation. `preparing_progress`,
`print_start_message` and `print_start_time_left` keep working and feed the strip and the
compact form.

**Compact form** for tiles too small for the list (480x272, 320x480, 272x480): file name +
"step N of M", the current step name large in amber, its countdown, and a row of dots (green
done, amber current, grey upcoming), over the same faded thumbnail. A tile of about 150 px
truncates every step name, which is why the compact form exists. The builder chooses the
threshold by measured tile size, not by breakpoint name.

**Printer tile**: a heater still climbing gets a large chip (value, target, small progress bar,
amber border) and a strong glow at its region. A heater at target shows a check on its chip.
Fans that are off are hidden, as the home widget hides inactive chips.

**Strip**: "Preparing", time left, estimated first-layer time, progress bar
(`preparing_progress`), filament line (swatch, tool, name, estimated print time).

## Phase 2: printing

**Job tile = today's preview card**: the gcode render with its corner buttons, including the
**exclude-object button, which stays where it is** (ruling: exclude object must stay one tap
away). The footer shows the file name, layer N/M and Z. The 3D/thumbnail toggle stays.

**Printer tile chips** (each tappable, opening the same detail the current panel opens for
that item; *(my call)* the existing callbacks are kept and re-bound to the chips, not
rewritten):

| Chip | Anchor (regions.json) | Notes |
|---|---|---|
| Nozzle temp | `nozzle` | multi-tool: shows the active tool badge, as the home widget does |
| Part fan % | `part_fan` | |
| Hotend fan % | `nozzle` (mounted on the toolhead) | |
| Bed temp | `bed` (two points; anchored as the callout engine already does, the middle of the near edge) | |
| Chamber temp | `chamber` | |
| Light | `light` | tap toggles the light |
| Aux fan(s) % | none | no line; dimmed at 0% |
| Speed + flow | none | one chip; amber border and amber value when either is not 100%; tap opens Tune (ruling). The percent/mm/s choice it toggles today already lives in Tune (`ui_print_tune_overlay.cpp`) and the chip honors it |

A heater off its target mid-print turns its chip amber and shows `value / target` again.
That is today's `classify_heater_status` (Heating / Ready / Cooling, a ±2 °C band) moved
onto the chip.

**Heater power** (the `NN%` beside each heater today is duty, from
`get_heater_power_subject`) lives in the detail the chip opens. Ruling: on wide screens
where the chip has room, the heater chips also show it (`220° · 5%`). The builder decides
"has room" by measured chip fit in the BothSides/OneSide modes, not by breakpoint name.

**Strip**: big %, time left, ETA clock time, elapsed time, progress bar, filament line (swatch,
tool, name, tool changes N/M), Up next (when the queue has more).

## Chips, leader lines, glows

- **Chips are 50% transparent** (ruling), with a light backdrop blur if the hardware can
  afford it. Otherwise they keep 50% alone. The builder measures this on the slowest
  target and drops the blur if it costs frames.
- **Leader lines on every chip whose location is known**, i.e. every chip with an anchor in
  the table above, **but only when the chip ends up more than X px from its anchor**. On tight
  layouts the chips sit on or beside their part and need no line (ruling). X maps to the
  existing `min_line` in `CalloutLayoutInput`. The builder picks the value from measured
  layouts.
- **Glows only on active regions**: heating nozzle/bed (strong while climbing, softer once
  holding), spinning part fan, light on. Nothing glows for an idle item. Blend over the picture
  so the glow shows on the printer art, not only behind it.
- **A chip with no anchor on this image** (aux fan, speed/flow, and light or chamber on the
  many tagged images that lack those regions) docks to the tile edge with no line. *(my call,
  from the ruling that lines go on every chip whose location is known.)*
- **The mode is fixed per phase** *(my call)*: decided from that phase's worst-case chip
  set, as the home widget decides from its budget, so the picture never jumps when a heater
  arrives or a fan stops. It may change once, at the pre-print to printing switch.
- **Nozzle and hotend fan share the `nozzle` anchor.** The engine has to separate two chips
  on one point. Its Toolhead merge is the existing precedent.
- **Placement reuses `include/callout_layout.h`** (`compute_callout_layout`,
  `CalloutMode`, `leader_elbow`, `spread_1d`). That engine already chooses between
  BothSides / OneSide / Pinned / Docked by measured space, places chips near their anchors
  and suppresses short lines. Extend it (new chip kinds: part fan, hotend fan, aux fan,
  speed/flow) rather than writing a second layout. `PrinterImageWidget` keeps its callout
  subjects in file-static `lv_subject_t`s, so a second live instance needs per-instance
  state. Factoring that out is the builder's call, but the result must be one engine that
  both screens use.

## When chips do not fit

On the tightest tiles (about 150x190 at 320x480 and 272x480, about 150 px tall at 480x272)
not every chip fits. Ruling: **hide what is normal, keep what is unusual.**

1. First drop chips showing an expected value: fans at 0%, the hotend fan at its automatic
   value, the light.
2. Then merge nozzle + part fan into one toolhead chip (the engine's existing Toolhead merge).
3. Never drop a chip that is off target, tuned (speed/flow not 100%), heating, or faulted.

Everything dropped stays reachable through the tile's tap-through detail.

## Fallbacks

- **Image with no `regions.json` entry, or a custom photo**: Docked mode. Chips line the tile
  edges, no leader lines, no glows (ruling). Same tile, same chips.
- **No printer image at all**: builder's call within the same rule (Docked chips around a
  neutral placeholder or the printer-type icon).

## Buttons and "More"

- Strip: **Pause, Tune, More, Cancel/Reprint**.
- **More** opens a small menu holding **Camera**, **Timelapse toggle** and **Light toggle**
  (ruling). Light is also the tappable chip on the picture, and the More entry covers images
  whose light has no tagged spot. Each entry keeps today's visibility rule
  (`printer_has_webcam and ...`, `printer_has_timelapse`, `led_controllable`). More hides when
  all its entries are hidden.

## Information inventory (nothing dropped)

| Today | Goes to |
|---|---|
| Nozzle / bed / chamber temp + target + Heating/Ready/Cooling status | printer chips; target shown while heating or off target |
| Heater power % | temp detail overlay; on the chip too on wide screens |
| Speed %, flow % | speed/flow chip, amber when changed |
| Tool changes N/M, filament swatch + name, AMS current tool | strip filament line |
| Part / hotend / aux fans | printer chips |
| Light, timelapse, camera buttons | light chip + More |
| Pause, Tune, Cancel, Reprint | strip |
| Layers, Z height, file name, elapsed, remaining, ETA, % | preview footer + strip |
| Exclude objects, 3D toggle | preview corner buttons (unchanged) |
| Up next | strip |
| Preparing overlay (message, progress, ETA) | pre-print checklist + strip |
| Chamber fault banner | builder's call; must stay visible (e.g. across the top of the printer tile) |
| Print complete overlay | unchanged, on the job tile |

## Constraints

- Declarative UI rules (`.claude/rules/declarative-ui.md`): appearance in XML, data in
  subjects. Phase switching through bindings on existing subjects (`preparing_visible`,
  `print_outcome`), not C++ show/hide.
- Threading rules for any new observers (`.claude/rules/threading.md`).
- Design tokens for spacing, radius and fonts. No hard-coded colors (amber = the theme's
  warning token, glow colors from theme tokens).
- Vendor abstraction: no printer-brand logic in the panel. Regions and capabilities only.
- US English strings, translatable.

## Left to the builder

- Exact X for the leader-line cutoff, and the compact-checklist size threshold. Both measured.
- Ultrawide arrangement.
- Whether the blur stays (measured).
- Placement of the chamber fault banner.
- How the callout engine becomes per-instance.
- Chip font sizes per tier, through tokens.
