// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "helix_plugin_features.h"

#include <functional>

namespace helix {

/// This instance's fallback_sender_role(): at once on the printer's host, else
/// after reading the local screen's presence record from Moonraker. @p then
/// runs on the main thread while @p token is alive. Main thread only.
void resolve_fallback_sender_role(const LifetimeToken& token,
                                  std::function<void(FallbackSenderRole)> then);

} // namespace helix
