# Vented spool flanges and a spool icon (#1817)

Two parts with one look: the drawn spools get a vented front flange, and the
Filament icon stops being a nozzle and becomes the same vented reel, front-on.

## Why

The drawn spool's front flange is a solid grey disc, so at a glance it reads as
a wheel with a coloured drum behind it, not as a spool. Real spools are vented,
and the vents show the filament wound inside. The Filament icon is MDI's
`printer-3d-nozzle`, which most people read as a water faucet. It names the
panel where people manage spools, so it should look like one.

Reference Preston gave: a vented reusable spool (Marswork) with radial
stadium-shaped slots around the hub and a field of small perforations between
them. The design simplifies that.

## Goals

- Every drawn spool reads as a spool at the size it is drawn, down to the AMS
  overview's ~32 px.
- The vents double as a fill gauge: a slot shows the filament where filament is
  wound behind it and the dark inside of the spool where it is not.
- The Filament icon (filled, outline and alert) is a front-on vented reel that
  sits with MDI glyphs at 24 px in the navbar.

## Non-goals

- No setting or toggle. One look.
- No change to the circle and chip swatches. Preston ruled those fine on
  2026-10-17 and they stay untouched.
- No change to finish textures, multi-colour winding or matte on the filament
  body. They keep working under the new flange.
- No air holes on the back flange. In the 3D view it is mostly hidden behind
  the filament and the front flange.

## Anti-goals

- Noise. A pattern that turns to speckle at small sizes is worse than a solid
  flange. Detail drops out by size; it never shrinks into mush.
- A flat-style lane whose colour you can no longer read at a glance.
- An icon that reads as a wheel, fan, film reel or steering wheel next to
  home/tune/cog.
- Frame-time cost on the lane spool's glow animation, or per-frame allocation.

## The 3D spool (`src/ui/ui_spool_canvas.cpp`)

Used by the Active Spool home widget, Spoolman list rows and edit, the AMS
loaded card, and the AMS 3D lane spool (via `ui_ams_lane_spool.cpp`).

Decided by Preston:

- **Six radial slots** (stadium shapes: a straight radial segment with round
  ends) evenly around the hub on the front flange, first one straight up.
- **A field of small round vent dots** between the slots, **only at large
  sizes**. Smaller spools draw the slots alone.
- Slots **show what is behind them**: the wound filament where the filament's
  radius reaches the slot, in a shadowed version of its colour (darker toward
  the bottom, as the body is). Elsewhere they show the dark inner face of the
  back flange. Dots behave the same way.
- Slots stay **readable down to ~32 px** (wider and fewer is fine at small
  sizes). Below ~32 px the flange draws solid, as today.

Reference geometry from the approved mockup (fractions of the canvas size S,
measured on the unskewed face, then skewed by `tray::kDepthSkew` like the
flange):

| Element | Value |
|---|---|
| Slot inner/outer centre radius | 0.165 S / 0.335 S |
| Slot half-width | 0.05 S |
| Dot pitch / radius | 0.03 S / 0.007 S, square grid |
| Dot band | 0.15 S < r < 0.39 S, clear of every slot by 0.025 S |
| Dots shown from | 96 px canvas size |
| Hole lip | a darker band along the upper edge of each cut (flange thickness) |

The mockup images and the throwaway Python that drew them sit beside this
file in `2026-10-21-spool-flange-icon/` (`r4-dark.png` row 1,
`slots_mock.py#render`, style `slots:6:dots`) and are deleted with it. They are
a target to match by eye, not a spec to copy pixel-for-pixel.

**Detail by size** (my call; one pure function serves both styles, its cases
are the boundary test): `spool_vent_detail(size_px)` returns None below 32 px,
Slots for 32 to 95 px, SlotsAndDots from 96 px. 32 and 96 are inclusive. If
the flat style reads differently front-on, that is tuned once against
screenshots, not given its own threshold up front.

Constraints:

- Draw into the existing `SpoolLayer` / `draw_buf` pixel path inside
  `render_spool_pixels`, after the front flange and before the hub. The render
  is already cached per key. No per-frame work, no new allocation per redraw.
  ESP32: nothing large in `.bss`.
- Size thresholds follow the `kSwatchFinishMinPx` pattern in `include/ui_swatch.h`:
  named constants with a one-line reason.
- The glow silhouette (`silhouette_mask`) keeps the solid outline. Vents do not
  punch holes in the glow.

## The flat AMS style (`src/ui/ui_ams_lane_spool.cpp`)

Today the flat style is three stacked `lv_obj` circles: a dark flange ring, the
filament disc sized by fill, and the hub.

Decided by Preston:

- **Same pattern, front-on**: six slots, dots at large sizes, same thresholds.
- **Smoked flange**: the slotted flange is drawn semi-transparent over the
  filament, so the filament colour shows across the whole wound area and
  brightest through the slots. Outside the filament's radius the flange is
  opaque grey and the slots show the dark inside. Mockup:
  `2026-10-21-spool-flange-icon/r4-dark.png`, row 2 (about 55% flange over 45%
  filament).

**Shared decision, separate compositing** (my call). One function owns the
vent geometry: given the face's centre, radii (the skew is `kDepthSkew` for
the 3D flange and 1.0 front-on) and the detail level, it answers per pixel
"how much of this pixel is a vent" (a coverage value, the same shape as
`helix::ui::SwatchCoverage`). Each style composites its own way: the 3D
flange paints vents with what is behind them, the flat style alpha-blends a
smoked flange and leaves the vents clear. Neither style draws slots itself.

**Flat redraws** (my call): the flat spool's pixels re-render when its
filament, fill or size changes. Those are discrete events, not animation
(`spool_visual_set_fill` sets a size, nothing tweens), so cache the flat
render the way the 3D one is cached: keyed by size, colour, fill bucket and
style.

Builder's call: how the flat style draws it. It needs real pixels now (a canvas
or a draw hook), not stacked rects. Reusing the 3D renderer's slot/dot code for
the front-on face is the expected route; one shared function, two
projections (the skew is 1.0 front-on). Keep the existing names `spool_outer`,
`spool_graphic` and `spool_hub` resolvable or update every lookup and test
that uses them (`ctl`, `inspect_spool_state()`, tests).

## The icon

Decided by Preston:

- **Front-on vented reel**, MDI style, 24x24 grid, single-path glyphs in
  `assets/fonts/custom_icons/` built by `scripts/build_custom_icon_font.py`
  (the Spoolman logo is the precedent; next codepoints in U+FE000).
- **Filled** (`filament`): solid disc r=10, hub hole r~2.6, six slot cut-outs
  (centre radius 5.0 to 7.6, width ~2.2), first slot straight up.
- **Outline** (`filament_outline`, the inactive navbar tab): **O3**, a thin rim
  (~1.6) around a smaller filled reel (r~6.8, hub hole ~2.2, six short slot
  cut-outs). Hollow-outlined slots were tried and rejected.
- **Alert** (`filament_alert`): MDI's `-alert` layout, the reel shifted left
  and a `!` bar on the right, as `printer-3d-nozzle-alert` does.
- The navbar's active/inactive pair is drawn by cross-fading two icons
  (`ui_xml/navigation_bar.xml`, `nav_icon_fade` / `nav_icon_fade_on`, the
  inactive at `#icon_size_nav_inactive`). The outline and filled glyphs must
  share a centre and outer diameter so the grow + highlight reads as one icon
  filling in.

Mockups: `2026-10-21-spool-flange-icon/outline-opts.png` row O3 and
`r4-dark.png` row 3.

### Where it applies

Preston chose "everywhere": the icon names change glyph, so every
`filament*` site follows. My call on top of that: about a third of the sites
use `filament` to mean the **nozzle or extruder**, not filament. Those move to
new `nozzle` / `nozzle_outline` names that keep today's MDI glyphs, so a purge
button does not turn into a spool. Classification at the time of writing:

| Keep the nozzle (rename to `nozzle*`) | Becomes the spool |
|---|---|
| `maintenance_overlay.xml` Nozzle interval | navbar (active, inactive, disabled) |
| `runout_guidance_modal.xml` btn_purge | `filament_sensor_indicator.xml`, `filament_sensor_row.xml` |
| `printer_manager_overlay.xml` nozzle_clean, purge_line | `printer_manager_overlay.xml` filament_sensors |
| `machine_limits_overlay.xml` Extrude Speed | `runout_guidance_modal.xml` icon_advisory |
| | `settings_printing_overlay.xml`, `wizard_preferences.xml`, `ams_device_operations.xml` rows |
| | `print_completion_modal.xml`, `print_file_detail.xml`, `print_status_preview_card.xml`, `filament_spool_card.xml` |
| | `settings_hardware_overlay.xml` Sensors |
| | `panel_widget_registry.cpp` filament sensor widget |

Re-grep before building (`grep -rnE '"filament(_outline|_alert)?"' ui_xml src`,
and `icon_filament` for text-glyph uses); the rule is "about the extruder or
nozzle keeps the nozzle". The purge-line chip uses `filament_outline` today and
moves to `nozzle_outline`.

My calls on the rest:

- **New names** `nozzle`, `nozzle_outline`, `nozzle_alert` point at today's
  MDI codepoints (F0E5B, F0E5C, F11C0). No existing name collides (`heater`
  is `printer-3d-nozzle-heat` and stays).
- **Text-glyph sites follow the glyph.** `ui_xml/globals.xml` `icon_filament*`
  strings point at the new codepoints. The clog bar (`clog_bar_body.xml`,
  `mdi_icons_16`) shows the spool alert: a clog is a filament-flow fault.
  `regen_mdi_fonts.sh` merges the custom TTF into every `mdi_icons_*` face, so
  16 px and every other size get the glyph.
- **Filament sensors** (presence, runout) are filament, so they become the
  spool.
- **Ambiguous sites:** the builder decides by the rule and lists every site it
  moved to `nozzle*` in the hand-back. Preston sees that list with the
  screenshots.

Icon plumbing (CLAUDE.md "Icon sync"): `include/ui_icon_codepoints.h`,
`scripts/regen_mdi_fonts.sh` (`CUSTOM_ICONS`), `make regen-fonts` (also
regenerates the ESP32 firmware's fonts), rebuild. `validate_icon_fonts.sh`
gates it.

## Acceptance

- Screenshots at 800x480 and 480x320, light and dark: home Active Spool, home
  AMS strip, AMS panel in both spool styles, Spoolman list, navbar with
  Filament active and inactive. Preston approves the pixels.
- The swatch spool gallery (`[swatch_gallery_spools]`) shows vents at 240 px
  and slots-only below the dot threshold (`HELIX_SWATCH_GALLERY_SPOOL`).
- Tests: the slot/dot decision (which size gets slots, dots, neither) as a pure
  function with its own cases; a canvas test that a slot pixel takes the
  filament colour when full and the dark inside when nearly empty; the flat
  style shows filament colour under the flange. Each new test is seen red once.
  The existing `[spool_canvas]`, `[swatch]`, `[active_spool]`, `[ams_state]`
  tags stay green, including the draw-task-count test.
- No measurable frame-time change on the lane spool glow animation.

## Left to the builder

- Exact anti-aliasing of slots and dots, lip strength, and the smoked opacity,
  tuned by eye against the mockups and shown to Preston.
- The dot threshold for the flat style if 96 px reads differently front-on.
- How the flat style gets pixels (canvas or draw hook), within the constraints
  above.
- Final glyph geometry, as long as the outline/filled pair shares a silhouette.
