// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <string>

namespace helix::ui {

/// What the Belt Tension screens call one path, translated. Until the user
/// says which belt is which, both forms are "Path A" / "Path B".
struct BeltPathName {
    std::string title;  ///< "Left belt": labels, headers, the start of a sentence
    std::string phrase; ///< "the left belt": inside a sentence
};

/// The one source of every user-visible path name on the Belt Tension screens.
[[nodiscard]] BeltPathName belt_path_name(helix::calibration::BeltPath path,
                                          helix::calibration::BeltLayout layout);

/// belt_path_name() under the active printer's saved layout.
[[nodiscard]] BeltPathName belt_path_name(helix::calibration::BeltPath path);

} // namespace helix::ui
