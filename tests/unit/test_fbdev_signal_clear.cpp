// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The SIGTERM fast exit blanks the framebuffer with clear_framebuffer_signal_safe():
 * open/write/close over a path and length the fbdev backend cached at startup.
 * A regular file stands in for /dev/fb0 here; write(2) from offset 0 behaves the
 * same on both.
 */

#include "fbdev_signal_clear.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

namespace {

struct FakeFramebuffer {
    std::string path;

    explicit FakeFramebuffer(size_t bytes) {
        path = (fs::temp_directory_path() / ("helix_fb_signal_clear_" + std::to_string(::getpid())))
                   .string();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::vector<char> ones(bytes, static_cast<char>(0xFF));
        out.write(ones.data(), static_cast<std::streamsize>(ones.size()));
    }
    ~FakeFramebuffer() {
        helix::cache_framebuffer_for_signal("", 0);
        std::error_code ec;
        fs::remove(path, ec);
    }

    std::vector<unsigned char> contents() const {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

} // namespace

TEST_CASE("clear_framebuffer_signal_safe zeroes the registered framebuffer",
          "[fbdev][signal_safe]") {
    // Not a multiple of the write chunk, so the short final write is exercised.
    constexpr size_t kBytes = 3 * 4096 + 123;
    FakeFramebuffer fb(kBytes);
    REQUIRE(helix::cache_framebuffer_for_signal(fb.path, kBytes));

    helix::clear_framebuffer_signal_safe();

    const auto bytes = fb.contents();
    REQUIRE(bytes.size() == kBytes);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; }));
}

TEST_CASE("clear_framebuffer_signal_safe does nothing with no framebuffer registered",
          "[fbdev][signal_safe]") {
    constexpr size_t kBytes = 4096;
    FakeFramebuffer fb(kBytes);
    REQUIRE(helix::cache_framebuffer_for_signal(fb.path, kBytes));
    CHECK_FALSE(helix::cache_framebuffer_for_signal("", kBytes));

    helix::clear_framebuffer_signal_safe();

    const auto bytes = fb.contents();
    REQUIRE(bytes.size() == kBytes);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0xFF; }));
}

TEST_CASE("cache_framebuffer_for_signal rejects a zero length and an oversized path",
          "[fbdev][signal_safe]") {
    CHECK_FALSE(helix::cache_framebuffer_for_signal("/dev/fb0", 0));
    CHECK_FALSE(helix::cache_framebuffer_for_signal(std::string(300, 'x'), 4096));
    CHECK(helix::cache_framebuffer_for_signal("/dev/fb0", 4096));
    helix::cache_framebuffer_for_signal("", 0);
}
