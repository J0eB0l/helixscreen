# Vented spool flanges and a spool icon: implementation plan (#1817)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. In this repo, § "Verification Cadence" in the root `CLAUDE.md` overrides those skills: no per-task review, only the task's tags while building.

**Goal:** Draw every spool with a vented front flange (six radial slots, vent dots on large spools) that shows the filament behind it, move the flat AMS style onto the same renderer, and replace the nozzle "faucet" Filament icon with a custom vented-reel glyph.

**Architecture:** One pure module (`spool_vents`) owns the vent geometry and the detail-by-size rule and answers per-pixel coverage for a face of any skew. `ui_spool_canvas` composites it two ways: the existing side view (vents show what is behind the flange) and a new front view (smoked flange over the filament, vents clear). The flat lane style becomes a spool canvas in the front view, so caching, finishes, multi-colour and glow come with it. The icons are SVGs compiled into the existing custom icon TTF and merged into every `mdi_icons_*` face.

**Tech Stack:** C++17, LVGL 9.5 (ARGB8888 draw buffers, pixel writes), Catch2, Python (fontTools + skia-pathops, only to rebuild the custom TTF), `lv_font_conv` via `scripts/regen_mdi_fonts.sh`.

**Spec:** `docs/devel/plans/2026-10-21-spool-flange-icon-design.md` (mockups and the throwaway Python that drew them in `docs/devel/plans/2026-10-21-spool-flange-icon/`). Read it first; this plan does not repeat its rulings.

## Global Constraints

- Branch `feature/spool-vents`, worktree via `scripts/setup-worktree.sh feature/spool-vents`. MAJOR work; hold `worktree:spool-vents` from the first edit.
- Circle and chip swatches do not change (`draw_swatch`, `<color_swatch>`, `<color_dot>`, `<swatch_paint>`).
- Six slots, first straight up. Slots from 32 px canvas size, vent dots from 96 px, both inclusive; below 32 px the flange is solid.
- Geometry in fractions of canvas size S on the unskewed face: slot centre radius 0.165 S to 0.335 S, half-width 0.05 S; dot pitch 0.03 S on a square lattice centred on the face, dot radius 0.007 S, dots only where 0.15 S < r < 0.39 S and at least 0.025 S clear of every slot's edge.
- Smoked flat flange: 55% flange over 45% filament inside the filament's radius; opaque outside it.
- No per-frame work, no allocation per redraw beyond what the canvas does today; nothing large in `.bss` (ESP32). Draw-task counts in existing tests stay as they are.
- Theme colours from tokens (`spool_body`, `spool_body_shade`, `spool_hub_top`, `spool_hub_bottom`); no new hex literals. A new token needs `make regen-tokens` + the theme_const golden regen in the same commit.
- Custom glyph codepoints: `spool` U+FE001, `spool_outline` U+FE002, `spool_alert` U+FE003. Names `filament`, `filament_outline`, `filament_alert` point at them; `nozzle`, `nozzle_outline`, `nozzle_alert` take F0E5B, F0E5C, F11C0.
- Code standards: spdlog only, `kCamelCase` constants, no comment archaeology, SPDX headers on new source files, `// DECLARATIVE_OK:` already covers the canvas painter.
- Headless only: `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy` on every app, screenshot and gallery run. Plain `make`, never `-jN`.
- While building, each task runs only its own tags (`make t F='...'`). No sweeps, gates, mutation or self-review; the orchestrator runs the finish once.

## Review Focus

1. **A fill change on a lane spool in the flat style** must update the visible filament (through the smoke and the slots) and hit the cache on the second identical render. Pinned in Task 3.
2. **A multi-colour or silk filament in the front view** still shows its split and finish under the smoke. Pinned in Task 3.
3. **The ghosted/empty lane in the flat style** (dimmed spool, dashed placeholder) still hides and dims the right object now that the three rings are one canvas. Pinned in Task 4.
4. **A spool at exactly 31, 32, 95 and 96 px** gets solid, slots, slots, slots + dots. Pinned in Task 1.
5. **A nozzle-meaning site** (purge, nozzle clean, purge line, nozzle interval, extrude speed) still shows the nozzle after the swap. Pinned in Task 5.

---

### Task 1: Vent geometry module

**Files:**
- Create: `include/spool_vents.h`, `src/ui/spool_vents.cpp`
- Test: `tests/unit/test_spool_vents.cpp` (tags `[spool_vents]`)

**Interfaces:**
- Produces (exact; syntax-checked when this plan was written):

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace helix::ui {

/// How much vent a spool flange of a given size can show and still read.
enum class SpoolVentDetail : uint8_t { None, Slots, SlotsAndDots };

inline constexpr int32_t kSpoolVentSlotsMinPx = 32;
inline constexpr int32_t kSpoolVentDotsMinPx = 96;

/// The detail a spool canvas @p size_px square draws on its flange.
SpoolVentDetail spool_vent_detail(int32_t size_px);

/// One flange face in canvas pixels: centre, the canvas size the geometry is a
/// fraction of, and the horizontal squash of the projection (tray::kDepthSkew
/// for the side view, 1.0 face-on).
struct SpoolVentFace {
    float cx = 0.0f;
    float cy = 0.0f;
    float size = 0.0f;
    float skew = 1.0f;
    SpoolVentDetail detail = SpoolVentDetail::None;
};

/// How much of the pixel centred on (@p x, @p y) is open vent, 0..1, with an
/// anti-aliased edge. 0 everywhere when the face's detail is None.
float spool_vent_coverage(const SpoolVentFace& face, float x, float y);

/// Distance from the face centre in unskewed face units (pixels of size S),
/// for compositing what lies behind a vent at that radius.
float spool_vent_face_radius(const SpoolVentFace& face, float x, float y);

} // namespace helix::ui
```

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_spool_vents.cpp`, `TEST_CASE("spool_vents: ...", "[spool_vents]")`:
  - `spool_vent_detail`: 0 and 31 -> None; 32 and 95 -> Slots; 96 and 240 -> SlotsAndDots.
  - Face S=200, centre (100,100), skew 1, Slots: `coverage(100, 100 - 0.25*200) == 1` (top slot centre); `coverage(100,100) == 0` (hub); the point at radius 0.25 S and angle 0 (between the -30 and +30 slots) `== 0`; radius 0.42 S straight up `== 0` (past the slot end).
  - Same face with SlotsAndDots: face point (0.27 S, 0) is a dot centre -> `> 0.9`; with Slots it is `0`. A lattice point at radius 0.40 S (outside the dot band) -> `0`.
  - Skew 0.45: the -30 degree slot centre at radius 0.25 S lands at `x = cx + 0.25*S*cos(-30deg)*0.45`, `y = cy + 0.25*S*sin(-30deg)` -> `1`; the unskewed position `x = cx + 0.25*S*cos(-30deg)` -> `0`.
  - Anti-aliasing: some pixel across a slot's long edge has `0 < coverage < 1`.
  - Detail None -> `0` at the top slot centre.
  - `spool_vent_face_radius` at the skewed slot centre above == `0.25*S` within 0.5 px.
- [ ] **Step 2:** `make t F='[spool_vents]'` fails (unresolved symbols).
- [ ] **Step 3: Implement** in `src/ui/spool_vents.cpp`: unskew x by `/ skew`, slot = distance to the radial segment (clamped projection onto each of the six unit directions, r from 0.165 S to 0.335 S) minus half-width; dot = nearest lattice point inside the band and clear of the slots; coverage `clamp(0.5 - signed_distance, 0, 1)` in screen pixels (scale the distance's x component back by skew for the AA width, or accept face-unit AA; tune by eye). Constants are named `k...` with a one-line reason each.
- [ ] **Step 4:** `make t F='[spool_vents]'` passes. Hand-break one line (for example the dot band's lower bound) once, see the right case go red, restore.
- [ ] **Step 5: Commit** `git add -N` the new files, then `git commit -m "feat(spool): vent geometry for spool flanges (prestonbrown/helixscreen#1817)" -- include/spool_vents.h src/ui/spool_vents.cpp tests/unit/test_spool_vents.cpp`.

### Task 2: Vents on the 3D spool

**Files:**
- Modify: `src/ui/ui_spool_canvas.cpp#render_spool_pixels` (between STEP 3 front flange and STEP 4 hub)
- Test: `tests/unit/test_spool_canvas_vents.cpp` (tags `[spool_canvas][spool_vents]`)

**Interfaces:**
- Consumes: Task 1 (`spool_vent_detail`, `SpoolVentFace`, `spool_vent_coverage`, `spool_vent_face_radius`).
- Produces: `static void paint_side_vents(SpoolLayer* layer, ...)` file-local; no public API change.

- [ ] **Step 1: Failing tests** (read pixels from `lv_canvas_get_draw_buf`, as `test_spool_canvas_filament.cpp` does):
  - `"spool_canvas: a full spool shows its filament through the flange slots"`: size 120, red 0xD02020, fill 1.0. The pixel at the top slot's centre on the front face (`right_x`, `cy - 0.25*120`) has `r > g + 40 && r > b + 40`.
  - `"spool_canvas: an empty spool's slots show the dark inside"`: same, fill 0.02. That pixel is grey (`|r-g| < 12`) and darker than the flange pixel at the same y, 0.30 S along the -30 degree direction between slots.
  - `"spool_canvas: a small spool keeps a solid flange"`: size 28; the top-slot pixel equals the flange gradient at that row within 3 per channel.
  - Vents blend opaque colour and never write alpha, so the glow silhouette (built from the canvas's alpha) stays solid; the existing `test_spool_canvas_glow.cpp` cases cover it and stay green.
- [ ] **Step 2:** `make t F='[spool_canvas]'` shows the three new cases failing, the existing ones green.
- [ ] **Step 3: Implement** `paint_side_vents`: face `{right_x, cy, size, tray::kDepthSkew, spool_vent_detail(size)}`; for each pixel in the front flange's bounding box with coverage > 0, blend over the flange: behind = the filament's front-face colour darkened (`ams_draw::darken_color(c, 30)` at the top to `110` at the bottom, as the mockup) when `fill > 0.01` and `spool_vent_face_radius <= filament_ry`, else `spool_body_shade` darkened 20 to 50; then a lip: darken the vent pixels whose pixel 3 rows up (scaled by size/240, minimum 1) is not vent, to 55%. Multi-colour filament: the behind colour is the band colour of the front face at that pixel (reuse the `FilamentBands` lookup for the front cap; if that cannot be reached cheaply, use the primary colour and say so in the hand-back).
- [ ] **Step 4:** `make t F='[spool_canvas],[active_spool],[spool_vents]'` passes, including the draw-task count test. Hand-break the fill check (always show filament) once, see the empty-spool case go red, restore.
- [ ] **Step 5:** Export the gallery (`HELIX_SWATCH_GALLERY_DIR=<scratch> ./build/bin/helix-tests '[swatch_gallery_spools]'`, then again with `HELIX_SWATCH_GALLERY_SPOOL=64`) and hand both PNG paths back. The orchestrator shows Preston; tuning by eye happens here.
- [ ] **Step 6: Commit** `feat(spool): vented front flange on the drawn spool (prestonbrown/helixscreen#1817)` with explicit pathspecs.

### Task 3: Front view on the spool canvas

**Files:**
- Modify: `include/ui_spool_canvas.h`, `src/ui/ui_spool_canvas.cpp` (data, cache key, render dispatch)
- Test: `tests/unit/test_spool_canvas_vents.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces:

```cpp
namespace helix::ui {
/// Which way a spool canvas looks at its spool.
enum class SpoolView : uint8_t { Side, Front };
void spool_canvas_set_view(lv_obj_t* canvas, SpoolView view); // redraws on change
SpoolView spool_canvas_get_view(lv_obj_t* canvas);
}
```

  `SpoolCacheKey` gains `SpoolView view` (in `==` and the hash). `SpoolCanvasData` gains `SpoolView view = SpoolView::Side`.

- [ ] **Step 1: Failing tests:**
  - `"spool_canvas: the front view smokes the flange over the filament"`: size 96, red, fill 1.0, Front. The pixel at radius 0.30 S on the 0 degree axis (between slots, over filament) is reddish but darker and greyer than the pixel at the top slot's centre (`r_slot > r_smoke`, and `r_smoke > g_smoke + 15`).
  - `"spool_canvas: the front view's slots go dark past the filament"`: fill 0.1: the top-slot pixel at radius 0.30 S is grey.
  - `"spool_canvas: a front view keeps the multi-colour split"`: colours "#FF0000,#0000FF", fill 1.0, Plain: a slot pixel in the top-left half is red-dominant, one in the bottom-right half blue-dominant.
  - `"spool_canvas: the same front spool renders once"`: render, change fill away and back to the same bucket, assert the cache hit count rises (use the counter the existing cache test uses; if none exists, add `size_t ui_spool_canvas_cache_hits_for_test()` beside `ui_spool_canvas_invalidate_cache`).
  - `"spool_canvas: view is part of the cache key"`: Side and Front at the same size, colour and fill give different pixels at the face centre's slot.
- [ ] **Step 2:** `make t F='[spool_canvas]'`: new cases fail.
- [ ] **Step 3: Implement** `static void render_front_pixels(SpoolCanvasData*)`: flange disc radius `size*kSpoolFlangeRadius` centred on the canvas, gradient from `spool_body` lightened 45 to darkened 10; filament disc radius `hub + (flange_r - edge - hub) * fill` (keep today's flat proportions: hub = size/3 diameter, 4 px rim at size 48 scaled); filament colour per pixel from `swatch_split_thresholds(disc_area, LV_RADIUS_CIRCLE, n)` along the TL-BR diagonal for multi-colour, matte via `swatch_matte_color`, finish via `swatch_paint_finish_pixels(buf, disc_area, LV_RADIUS_CIRCLE, finish)`; then the flange over it at 55% where `face_radius <= filament_r` and opaque elsewhere, skipping vent coverage (vents show filament, or `spool_body_shade` darkened 45 past the filament); hub gradient last. `render_spool_pixels` dispatches on `view`.
- [ ] **Step 4:** `make t F='[spool_canvas],[spool_vents]'` passes. Hand-break the smoke alpha (set it to 1.0) once, see the smoke test go red, restore.
- [ ] **Step 5: Commit** `feat(spool): front view on the spool canvas (prestonbrown/helixscreen#1817)`.

### Task 4: Flat lane style on the front view

**Files:**
- Modify: `src/ui/ui_ams_lane_spool.cpp#create_spool_visual`, `#spool_visual_set_filament`, `#spool_visual_set_fill`, `#spool_visual_set_empty`, the ghost/opacity block near `apply_presence`
- Test: `tests/unit/test_ams_lane_spool.cpp`

**Interfaces:**
- Consumes: Task 3 (`spool_canvas_set_view`, `spool_canvas_get_view`).
- Produces: `SpoolVisual` loses `spool_outer`, `color_swatch`, `spool_hub`; both styles keep `canvas` (named `spool_graphic`), `use_3d` only picks the view.

- [ ] **Step 1: Rewrite the test** `"ams_lane_spool: flat style renders concentric rings"` as `"ams_lane_spool: flat style draws the front-on spool"`: `part(spool,"spool_graphic")` is an `lv_canvas_class`, `spool_canvas_get_view == SpoolView::Front`, `ui_spool_canvas_get_color` is 0xFF0000, `part(spool,"spool_outer") == nullptr`, placeholder hidden. Add `"ams_lane_spool: a ghosted flat lane dims its spool canvas"`: Ghosted state in flat style -> the canvas is visible with `lv_obj_get_style_opa < LV_OPA_COVER`; Empty -> canvas hidden, placeholder shown.
- [ ] **Step 2:** `make t F='[lane_spool]'`: both fail.
- [ ] **Step 3: Implement**: the flat branch creates the same spool canvas as the 3D branch with `spool_canvas_set_view(canvas, SpoolView::Front)`, keeps the container's circle radius and shadow, drops the three ring objects and every branch that touched them. The glow keeps passing no spool for the disc shape (`kGlowDiscShape`).
- [ ] **Step 4:** `make t F='[lane_spool],[ams_state],[spool_canvas]'` passes. `grep -rn 'spool_outer\|spool_hub' src tests docs` finds nothing.
- [ ] **Step 5: Commit** `refactor(ams): flat lane spools draw on the spool canvas (prestonbrown/helixscreen#1817)`.

### Task 5: Spool icon glyphs

**Files:**
- Create: `assets/fonts/custom_icons/spool.svg`, `spool_outline.svg`, `spool_alert.svg`
- Modify: `scripts/build_custom_icon_font.py` (`GLYPHS`), `scripts/regen_mdi_fonts.sh` (`CUSTOM_ICONS`), `include/ui_icon_codepoints.h`, `ui_xml/globals.xml` (`icon_filament*` strings), the nozzle-meaning sites, regenerated `assets/fonts/helix-custom-icons.ttf` and every `mdi_icons_*` / ESP32 font output `make regen-fonts` rewrites
- Test: `tests/unit/test_ui_icon.cpp` (tags `[icon]`)

**Interfaces:**
- Produces: icon names `filament`, `filament_outline`, `filament_alert` -> U+FE001..FE003; `nozzle`, `nozzle_outline`, `nozzle_alert` -> F0E5B, F0E5C, F11C0.

- [ ] **Step 1: Failing test** `"icon: filament names are the spool glyphs, nozzle names the nozzle"` (`[icon]`): `lookup_codepoint("filament")` is `"\xF3\xBE\x80\x81"`, `_outline` `"\xF3\xBE\x80\x82"`, `_alert` `"\xF3\xBE\x80\x83"`; `lookup_codepoint("nozzle")` is `"\xF3\xB0\xB9\x9B"`, `nozzle_outline` `"\xF3\xB0\xB9\x9C"`, `nozzle_alert` `"\xF3\xB1\x87\x80"`. Add a second case that loads `ui_xml/maintenance_overlay.xml`'s nozzle-interval row, `printer_manager_overlay.xml`'s `pm_chip_purge_line` and `runout_guidance_modal.xml`'s `btn_purge` and asserts their icon text is the nozzle codepoint (or, if those components cannot be built in a test cheaply, a grep-style test over the XML files asserting `icon="nozzle` on those three named elements).
- [ ] **Step 2:** `make t F='[icon]'` fails.
- [ ] **Step 3: Draw the SVGs** on the 24x24 grid as single paths (geometry in the spec, § The icon; reference `docs/devel/plans/2026-10-21-spool-flange-icon/r4_mock.py#ic_fill`, `outline_mock.py#O3`, `r4_mock.py#ic_alert`). Filled and outline share centre (12,12) and outer radius 10.
- [ ] **Step 4: Build the font**: `pip install --user fonttools skia-pathops` if missing; add the three glyphs to `GLYPHS`, run `scripts/build_custom_icon_font.py`; add `0xFE001,0xFE002,0xFE003` to `CUSTOM_ICONS`; `make regen-fonts`. Then the names in `ui_icon_codepoints.h`, `globals.xml`, and the nozzle sites (re-grep `"filament(_outline|_alert)?"` and `icon_filament`; rule: about the extruder or nozzle keeps the nozzle). List every site moved to `nozzle*` in the hand-back.
- [ ] **Step 5:** `make t F='[icon],[font],[nav]'` passes and the build's `validate_icon_fonts.sh` step is clean. Hand-break one codepoint in `ui_icon_codepoints.h`, see Step 1's case go red, restore.
- [ ] **Step 6:** `scripts/screenshot.sh helix-screen spool-icon-nav home` and `... filament` (headless), hand both paths back for Preston (navbar inactive outline, active filled).
- [ ] **Step 7: Commit** `feat(icons): spool glyph for filament, nozzle keeps its own (prestonbrown/helixscreen#1817)`.

### Task 6: Gallery, docs, plan cleanup

**Files:**
- Modify: `tests/unit/test_swatch_gallery.cpp#swatch gallery spools` (a second row in the front view), `docs/devel/COLOR_SWATCHES.md` (spool section: vents, detail by size, the front view, the gallery row), `docs/devel/FILAMENT_MANAGEMENT.md` only if it describes the flat style's rings
- Delete: `docs/devel/plans/2026-10-21-spool-flange-icon-design.md`, `docs/devel/plans/2026-10-21-spool-flange-icon.md`, `docs/devel/plans/2026-10-21-spool-flange-icon/`

- [ ] **Step 1:** Gallery: render each filament a second time with `spool_canvas_set_view(..., SpoolView::Front)` in a row below, page height `2 * size`.
- [ ] **Step 2:** `HELIX_SWATCH_GALLERY_DIR=<scratch> ./build/bin/helix-tests '[swatch_gallery_spools]'` writes both PNGs; hand the paths back.
- [ ] **Step 3:** Docs as above, present tense, no history.
- [ ] **Step 4: Commit** `docs(spool): vented flanges and the front view; drop the #1817 plan (prestonbrown/helixscreen#1817)`. The plan files go in this commit, which is the last one on the branch.

## Finish (orchestrator, once)

1. Screenshots, 800x480 and 480x320, light and dark: home Active Spool, home AMS strip, AMS panel in `3d` and `flat` (`/ams/spool_style`), Spoolman list, navbar with Filament active and inactive. Preston approves pixels.
2. One review of the branch (Opus fork per memory), with `docs/devel/REVIEW_RUBRIC.md`.
3. `make mutate-diff MUTATE_ARGS="--tests '[spool_vents],[spool_canvas],[lane_spool],[icon]'"`.
4. Frame time: run the AMS panel with a lit lane (`--sim-speed 6`, `HELIX_MOCK_AMS=afc`) before and after; compare the render-time log line or `ctl` timing; report the numbers.
5. One `make land-gate`, then land and push under `worktree:main`, close #1817, tear down the worktree.
