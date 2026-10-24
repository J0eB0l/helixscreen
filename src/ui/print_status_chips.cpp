// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_chips.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widgets/callout_chip.h"
#include "tune_controller.h"

#include <algorithm>

namespace helix {

namespace {
/// The reading as every heater display prints it, without the unit letter:
/// "184 / 220°" with a target, "220°" without.
std::string heater_text(int current_deci, int target_deci) {
    return ui::compact_callout_text(
        ui::temperature::heater_display(current_deci, target_deci).temp);
}
} // namespace

ChipContent heater_chip(bool present, int current_deci, int target_deci,
                        const ui::temperature::HeaterStatus& status, bool printing, bool show_power,
                        std::string_view badge, int start_deci) {
    using ui::temperature::HeaterStatusState;
    ChipContent c;
    if (!present || (target_deci <= 0 && !ui::temperature::is_residual_hot(current_deci)))
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
    // The target shows only while the reading is off it; holding, the
    // reading alone says the heater is there.
    text += heater_text(current_deci, heating || cooling ? target_deci : 0);
    if (show_power && !status.duty.empty())
        text += " · " + status.duty;
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

std::string heater_budget_text(std::string_view badge) {
    std::string text(badge);
    if (!text.empty()) {
        text += ' ';
    }
    // The widest shape at the widest reading the home callout measures too.
    return text + heater_text(9990, 9990);
}

ChipContent speed_flow_chip(int speed_pct, int flow_pct, const std::string& speed_text,
                            const std::string& flow_text) {
    ChipContent c;
    c.shown = true;
    c.lead_text = speed_text;
    c.text = flow_text;
    if (speed_pct != 100 || flow_pct != 100) {
        c.tone = ChipTone::Warn;
    } else {
        c.expendable = true;
    }
    return c;
}

ChipContent speed_flow_budget_chip(float filament_diameter_mm) {
    const auto t = tune::live_rates_text(100, 100, 999, 4150, filament_diameter_mm);
    return speed_flow_chip(100, 100, t.speed, t.flow);
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
