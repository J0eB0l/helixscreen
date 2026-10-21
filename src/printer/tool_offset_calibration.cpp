// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offset_calibration.h"

#include "indx_contract.h"
#include "printer_discovery.h"
#include "translation_loader.h"

#if HELIX_HAS_SNAPMAKER
#include "snapmaker_nozzle_offset.h"
#endif

namespace helix::tool_offset_calibration {

namespace {

bool has_macro_procedure(const PrinterDiscovery& hw) {
    return hw.has_tool_changer() && hw.has_macro(kMacro);
}

} // namespace

bool supported(const PrinterDiscovery& hw) {
    return has_macro_procedure(hw) || !guided_flow(hw).empty();
}

std::string guided_flow(const PrinterDiscovery& hw) {
#if HELIX_HAS_SNAPMAKER
    if (snapmaker::nozzle_offset::supported(hw)) {
        return snapmaker::nozzle_offset::kGuidedFlowId;
    }
#endif
    if (indx::supports_seat_by_hand(hw)) {
        return indx::kSeatByHandFlowId;
    }
    return {};
}

const char* entry_description(const PrinterDiscovery& hw) {
    if (guided_flow(hw) == indx::kSeatByHandFlowId) {
        return TR_NOOP("Seat a tool by hand for the first Z home");
    }
    return TR_NOOP("Measure every tool's XYZ offset automatically");
}

std::string calibrate_all_gcode(const PrinterDiscovery& hw) {
    if (!has_macro_procedure(hw)) {
        return {};
    }
    // The command alias is always the uppercased name whatever case printer.cfg
    // spells the section in (klippy/extras/gcode_macro.py), so the constant is
    // the right spelling here - unlike a SET_GCODE_VARIABLE MACRO= mux key.
    return kMacro;
}

} // namespace helix::tool_offset_calibration
