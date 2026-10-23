// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_db_item.h"

#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

namespace helix {

void post_item_if_changed(IMoonrakerAPI& api, const std::string& namespace_name,
                          const std::string& key, const nlohmann::json& value,
                          std::function<void()> on_written,
                          std::function<void(const MoonrakerError&)> on_error) {
    auto post = [&api, namespace_name, key, value, on_written, on_error]() {
        api.database_post_item(namespace_name, key, value, on_written, on_error);
    };
    api.database_get_item(
        namespace_name, key,
        [post, value, namespace_name, key](const nlohmann::json& stored) {
            if (stored == value) {
                spdlog::debug("[Moonraker DB] {}/{} already current", namespace_name, key);
                return;
            }
            post();
        },
        // No record yet (Moonraker answers a missing key with an error).
        [post](const MoonrakerError&) { post(); });
}

} // namespace helix
