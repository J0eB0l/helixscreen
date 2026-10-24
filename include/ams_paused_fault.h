// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When a filament system's standing fault is raised as a dialog: the backend
// reports one while the print is paused. AFC prints its `!!`, pauses, and may
// have held error_state since an earlier fault, so neither the ERROR action edge
// nor the pause alone is a reliable trigger; this is fed on every change of
// either. Shared by AmsErrorBridge (desktop) and the firmware's fault dialog.
//
// Compiled into the ESP32 firmware.

#include <string>

namespace helix {

class PausedFaultTrigger {
  public:
    /// True when the fault named @p key should be raised now: @p fault and
    /// @p paused both hold and this pause has not raised @p key yet. A fault
    /// that clears, or a print that leaves the pause, arms it again.
    bool update(bool fault, bool paused, const std::string& key) {
        if (!fault || !paused) {
            raised_.clear();
            return false;
        }
        if (key == raised_) {
            return false;
        }
        raised_ = key;
        return true;
    }

    /// True while a raised fault still stands.
    [[nodiscard]] bool raised() const {
        return !raised_.empty();
    }

  private:
    std::string raised_;
};

} // namespace helix
