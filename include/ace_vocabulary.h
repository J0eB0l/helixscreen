// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

/**
 * @file ace_vocabulary.h
 * @brief Anycubic ACE Pro names that discovery and the backends share.
 *
 * Kept free of LVGL and the AMS types: printer discovery compiles into the
 * ESP32 firmware, which has neither.
 */
namespace helix::ace {

/// What the ACEPRO driver names each unit's Klipper object, before its
/// instance number (`ace_instance_0`, `ace_instance_1`, ...).
inline constexpr std::string_view kInstanceObjectPrefix = "ace_instance_";

} // namespace helix::ace
