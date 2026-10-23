// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fbdev_signal_clear.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace helix {

namespace {

char s_fb_path[256] = {};
size_t s_fb_bytes = 0;
volatile sig_atomic_t s_fb_ready = 0;

constexpr size_t kZeroChunk = 4096;
const char kZeros[kZeroChunk] = {};

} // namespace

bool cache_framebuffer_for_signal(const std::string& path, size_t byte_len) {
    s_fb_ready = 0;
    if (path.empty() || byte_len == 0 || path.size() >= sizeof(s_fb_path)) {
        return false;
    }
    std::memcpy(s_fb_path, path.c_str(), path.size() + 1);
    s_fb_bytes = byte_len;
    s_fb_ready = 1;
    return true;
}

void clear_framebuffer_signal_safe() {
    if (s_fb_ready == 0) {
        return;
    }
    const int saved_errno = errno;
    const int fd = ::open(s_fb_path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t remaining = s_fb_bytes;
        while (remaining > 0) {
            const size_t chunk = remaining < kZeroChunk ? remaining : kZeroChunk;
            const ssize_t n = ::write(fd, kZeros, chunk);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            remaining -= static_cast<size_t>(n);
        }
        ::close(fd);
    }
    errno = saved_errno;
}

} // namespace helix
