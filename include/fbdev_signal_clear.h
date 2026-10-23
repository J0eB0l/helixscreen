// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace helix {

/// Snapshot the framebuffer device @p path and its @p byte_len (smem_len) into
/// static storage so a signal handler can blank it without constructing
/// anything. Main thread only, before the handler can fire. An empty path, a
/// zero length or a path that does not fit clears the registration instead.
/// @return true if the framebuffer was registered.
bool cache_framebuffer_for_signal(const std::string& path, size_t byte_len);

/// Write zeros over the registered framebuffer using only async-signal-safe
/// calls (open, write, close), so the last frame is not left on the panel for
/// whatever starts next. A no-op when nothing is registered.
void clear_framebuffer_signal_safe();

} // namespace helix
