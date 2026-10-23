// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_start_phase.h"

namespace helix {

/// Short translated step name for a pre-print checklist row, no ellipsis.
/// "" for IDLE, INITIALIZING and COMPLETE.
[[nodiscard]] const char* print_start_phase_step_label(PrintStartPhase phase);

} // namespace helix
