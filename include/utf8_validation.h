// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace helix::text_io {

/// Whether `s` is well-formed UTF-8: no stray continuation byte, no truncated,
/// overlong or surrogate sequence, nothing past U+10FFFF.
inline bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        size_t length = 0;
        uint32_t cp = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            cp = lead & 0x07;
        } else {
            return false;
        }
        if (i + length > s.size()) {
            return false;
        }
        for (size_t k = 1; k < length; ++k) {
            const auto cont = static_cast<unsigned char>(s[i + k]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        static constexpr uint32_t kMinForLength[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMinForLength[length] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += length;
    }
    return true;
}

} // namespace helix::text_io
