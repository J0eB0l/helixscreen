// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_layout_fitter.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

namespace {

/// components/progress_bar.xml's height.
constexpr int32_t kProgressBarHeight = 12;

/// The strip's progress block (components/print_status_strip.xml), rows
/// flush: the percent line in font_body_bold, the bar, the filament line and
/// the up-next line.
int32_t strip_text_height() {
    const int32_t heading = lv_font_get_line_height(theme_manager_get_font("font_body_bold"));
    const int32_t small = lv_font_get_line_height(theme_manager_get_font("font_small"));
    return heading + kProgressBarHeight + 2 * small;
}

/// The strip is a ui_card: its padding and border on both sides.
int32_t strip_chrome() {
    return 2 * (theme_manager_get_spacing("space_sm") + theme_manager_get_spacing("border_width"));
}

} // namespace

PrintStatusLayoutFitter::PrintStatusLayoutFitter(std::string log_tag, Subjects subjects, Host host)
    : log_tag_(std::move(log_tag)), subjects_(subjects), host_(std::move(host)) {}

int32_t PrintStatusLayoutFitter::strip_row_height() {
    return std::max(theme_manager_get_spacing("ps_strip_button"), strip_text_height()) +
           strip_chrome();
}

int32_t PrintStatusLayoutFitter::strip_stacked_height() {
    return strip_text_height() + theme_manager_get_spacing("space_sm") +
           theme_manager_get_spacing("button_height") + strip_chrome();
}

const PrintStatusTileLayout& PrintStatusLayoutFitter::recompute() {
    lv_obj_t* root = host_.root ? host_.root() : nullptr;
    lv_obj_t* content = root ? lv_obj_find_by_name(root, "overlay_content") : nullptr;
    if (!content || !host_.subjects_ready()) {
        return last_;
    }
    const int32_t w = lv_obj_get_content_width(content);
    const int32_t h = lv_obj_get_content_height(content);
    if (w <= 0 || h <= 0) {
        return last_;
    }
    last_ = decide_print_status_tile_layout(w, h, theme_manager_get_spacing("space_md"),
                                            strip_row_height(), strip_stacked_height());
    const auto set = [](lv_subject_t* s, int v) {
        if (s && lv_subject_get_int(s) != v) {
            lv_subject_set_int(s, v);
        }
    };
    set(subjects_.arrangement, static_cast<int>(last_.arrangement));
    set(subjects_.strip_stacked, last_.strip_stacked ? 1 : 0);
    set(subjects_.checklist_compact, last_.checklist_compact ? 1 : 0);
    spdlog::debug("[{}] Content {}x{}: {} strip {} ({}px), job {}x{}, printer {}x{}, checklist {}",
                  log_tag_, w, h,
                  last_.arrangement == PrintStatusArrangement::Stacked ? "stacked" : "side by side",
                  last_.strip_stacked ? "stacked" : "row", last_.strip_h, last_.job_w, last_.job_h,
                  last_.printer_w, last_.printer_h, last_.checklist_compact ? "compact" : "list");
    return last_;
}

} // namespace helix::ui
