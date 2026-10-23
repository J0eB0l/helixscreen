// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Puts one printer picture's callouts on screen: measures each chip, asks
// compute_callout_layout() where everything goes, then positions the image,
// chips, leader lines and glows. The home printer-image widget and the print
// status printer tile each own one, so both draw from the same engine.

#include "callout_layout.h"
#include "printer_image_regions.h"

#include <array>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

class CalloutPresenter {
  public:
    struct Chip {
        CalloutKind kind = CalloutKind::Nozzle;
        lv_obj_t* chip = nullptr; ///< an activity_chip; its width is measured here
        lv_obj_t* line = nullptr; ///< a leader_line, or nullptr for a chip that never has one
        lv_obj_t* glow = nullptr; ///< its region glow, or nullptr
        bool in_budget = false;   ///< this printer can ever show it: decides the mode
        bool active = false;      ///< showing now
        bool expendable = false;  ///< ChipContent::expendable
        bool glowing = false;     ///< ChipContent::glow
    };

    struct Parts {
        lv_obj_t* area = nullptr;  ///< the container whose content box the layout fills
        lv_obj_t* image = nullptr; ///< the printer picture
        std::vector<Chip> chips;
        std::optional<Chip>
            toolhead; ///< the merged nozzle + part-fan chip, when the screen has one
    };

    struct Config {
        bool single_cell = false;
        int min_line = 0;            ///< CalloutLayoutInput::min_line
        int leader_min_distance = 0; ///< CalloutLayoutInput::leader_min_distance
    };

    /// Measure, decide, position. `regions` is nullptr for an untagged image.
    /// Returns the layout so the owner can publish the mode subject.
    CalloutLayout apply(const Parts& parts, const ImageRegions* regions, int natural_w,
                        int natural_h, const Config& config);

  private:
    std::array<std::array<lv_point_precise_t, 3>, kCalloutKindCount> line_pts_{};
};

} // namespace helix::ui
