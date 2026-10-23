// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_phase_label.h"

#include "lvgl/src/others/translation/lv_translation.h"

namespace helix {

const char* print_start_phase_step_label(PrintStartPhase phase) {
    switch (phase) {
    case PrintStartPhase::HOMING:
        return lv_tr("Homing");
    case PrintStartPhase::HEATING_BED:
        return lv_tr("Heating bed");
    case PrintStartPhase::SOAKING:
        return lv_tr("Heat soak");
    case PrintStartPhase::HEATING_NOZZLE:
        return lv_tr("Heating nozzle");
    case PrintStartPhase::QGL:
        return lv_tr("Quad gantry level");
    case PrintStartPhase::Z_TILT:
        return lv_tr("Z tilt");
    case PrintStartPhase::BED_MESH:
        return lv_tr("Bed mesh");
    case PrintStartPhase::CLEANING:
        return lv_tr("Cleaning nozzle");
    case PrintStartPhase::PURGING:
        return lv_tr("Purge");
    case PrintStartPhase::IDLE:
    case PrintStartPhase::INITIALIZING:
    case PrintStartPhase::COMPLETE:
        return "";
    }
    return "";
}

} // namespace helix
