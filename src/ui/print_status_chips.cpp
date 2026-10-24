// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_chips.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <algorithm>

namespace helix {

namespace {
constexpr int kColdHideDeci = 400;

std::string deg(int deci) {
    // Round half away from zero to whole degrees.
    int v = deci >= 0 ? (deci + 5) / 10 : -((-deci + 5) / 10);
    return std::to_string(v);
}
} // namespace

ChipContent heater_chip(bool present, int current_deci, int target_deci,
                        const ui::temperature::HeaterStatus& status, bool printing, bool show_power,
                        std::string_view badge, int start_deci) {
    using ui::temperature::HeaterStatusState;
    ChipContent c;
    if (!present || (target_deci <= 0 && current_deci < kColdHideDeci))
        return c;
    c.shown = true;
    c.glow = target_deci > 0;
    c.status_state = static_cast<int>(status.state);

    const bool heating = status.state == HeaterStatusState::Heating;
    const bool cooling = status.state == HeaterStatusState::Cooling;

    std::string text;
    if (!badge.empty()) {
        text.append(badge);
        text += ' ';
    }
    if (heating || cooling) {
        text += deg(current_deci) + " / " + deg(target_deci) + "°";
        if (show_power && !status.duty.empty())
            text += " · " + status.duty;
    } else {
        text += deg(current_deci) + "°";
        if (show_power && !status.duty.empty())
            text += " · " + status.duty;
    }
    c.text = std::move(text);

    if (printing) {
        if (heating || cooling)
            c.tone = ChipTone::Warn;
    } else if (heating) {
        c.tone = ChipTone::Active;
    }

    if (heating) {
        const int span = target_deci - start_deci;
        if (span > 0) {
            // Whole percent, rounded down.
            c.progress_pct = std::clamp((current_deci - start_deci) * 100 / span, 0, 100);
        } else {
            c.progress_pct = 0;
        }
    }
    return c;
}

ChipContent fan_chip(FanRole role, bool present, int pct, bool automatic, bool labeled) {
    ChipContent c;
    if (!present)
        return c;
    c.shown = true;
    std::string text;
    if (labeled) {
        switch (role) {
        case FanRole::Part:
            text = std::string(lv_tr("Part")) + " ";
            break;
        case FanRole::Hotend:
            text = std::string(lv_tr("HE")) + " ";
            break;
        case FanRole::Aux:
            text = std::string(lv_tr("Aux")) + " ";
            break;
        }
    }
    c.text = text + std::to_string(pct) + "%";
    if (pct <= 0) {
        c.tone = ChipTone::Dim;
        c.expendable = true;
    } else if (role == FanRole::Part) {
        c.tone = ChipTone::Active;
        c.glow = true;
    }
    if (role == FanRole::Hotend && automatic)
        c.expendable = true;
    return c;
}

std::string heater_budget_text(std::string_view now, std::string_view badge) {
    std::string text(badge);
    if (!text.empty()) {
        text += ' ';
    }
    text += now.find(" / ") != std::string_view::npos ? "220 / 220°" : "220°";
    return text;
}

ChipContent speed_flow_chip(int speed_pct, int flow_pct, const std::string& speed_text,
                            const std::string& flow_text) {
    ChipContent c;
    c.shown = true;
    c.text = speed_text + " · " + flow_text;
    if (speed_pct != 100 || flow_pct != 100) {
        c.tone = ChipTone::Warn;
    } else {
        c.expendable = true;
    }
    return c;
}

ChipContent light_chip(bool controllable, bool on) {
    ChipContent c;
    if (!controllable)
        return c;
    c.shown = true;
    c.text = "";
    if (on) {
        c.tone = ChipTone::Active;
        c.glow = true;
    } else {
        c.tone = ChipTone::Dim;
        c.expendable = true;
    }
    return c;
}

bool heater_power_fits(CalloutMode mode, int w_with_power, int column_w) {
    if (mode != CalloutMode::BothSides && mode != CalloutMode::OneSide)
        return false;
    return w_with_power > 0 && w_with_power <= column_w;
}

} // namespace helix
