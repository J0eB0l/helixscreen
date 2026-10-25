// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What each chip on the print status printer tile says and how it looks: one
// pure decision per chip kind, so the panel only copies results into subjects.

#include "ui_temperature_utils.h"

#include "callout_layout.h"

#include <string>
#include <string_view>

namespace helix {

/// The chip's look; XML binds these ints. Normal = plain, Active = working
/// (heating, spinning, on), Warn = needs a look (off target, tuned), Dim = off.
enum class ChipTone { Normal = 0, Active = 1, Warn = 2, Dim = 3 };

struct ChipContent {
    bool shown = false;
    std::string text;
    ChipTone tone = ChipTone::Normal;
    /// Showing an expected value, so a tile too small for every chip drops it first.
    bool expendable = false;
    /// Its region on the picture glows.
    bool glow = false;
    /// 0-100 heat-up progress for a climbing heater, -1 for no bar.
    int progress_pct = -1;
    /// A heater's HeaterStatusState as an int, for its heater_status glyph; 0 for any other chip.
    int status_state = 0;
};

/// A heater chip. `status` is classify_heater_status() of the same reading.
/// Target shows while Heating or Cooling; duty shows only with `show_power`.
/// `printing`: past pre-print, so leaving the band is Warn rather than Active.
/// `badge` ("T1") prefixes the text on a multi-tool printer, empty otherwise.
/// `start_deci` is the reading when heating began, for the progress bar.
/// Not present, or off and cold: not shown.
[[nodiscard]] ChipContent heater_chip(bool present, int current_deci, int target_deci,
                                      const ui::temperature::HeaterStatus& status, bool printing,
                                      bool show_power, std::string_view badge, int start_deci);

/// The text a heater chip is measured at when the layout picks its mode: its
/// widest shape ("999 / 999°") after `badge` ("T1"), whatever it shows now, so
/// neither a reading's digits nor a heater crossing its tolerance edge moves
/// the picture.
[[nodiscard]] std::string heater_budget_text(std::string_view badge);

/// A fan chip. A part fan is Active while spinning; any fan at 0% is Dim and
/// expendable; a hotend fan at its automatic speed is expendable.
enum class FanRole { Part = 0, Hotend = 1, Aux = 2 };
/// `labeled` prefixes the role ("Part", "HE", "Aux") when the chip has room.
[[nodiscard]] ChipContent fan_chip(FanRole role, bool present, int pct, bool automatic,
                                   bool labeled);

/// The speed/flow chip: the live toolhead speed as its text. Warn whenever
/// either factor is not 100%, never expendable then; the factors themselves
/// are Tune's to show.
[[nodiscard]] ChipContent speed_flow_chip(int speed_pct, int flow_pct,
                                          const std::string& speed_text);

/// The speed/flow chip the layout measures: a three-digit toolhead speed.
[[nodiscard]] ChipContent speed_flow_budget_chip();

/// The light chip: Active when on, expendable when off.
[[nodiscard]] ChipContent light_chip(bool controllable, bool on);

/// Whether a heater chip carries its duty ("220° · 5%"). Only the line modes
/// set chips beside the picture with a column of their own, and the duty goes
/// on only while the chip, measured with it, still fits that column:
/// `column_w` is the widest budget chip, which sized the column.
[[nodiscard]] bool heater_power_fits(CalloutMode mode, int w_with_power, int column_w);

} // namespace helix
