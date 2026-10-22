// SPDX-License-Identifier: GPL-3.0-or-later

#include "fallback_sender_role_resolver.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "printer_state.h"

#include <ctime>
#include <optional>

namespace helix {

void resolve_fallback_sender_role(const LifetimeToken& token,
                                  std::function<void(FallbackSenderRole)> then) {
    if (get_printer_state().plugin_status_state().screen_local()) {
        then(fallback_sender_role(true, std::nullopt));
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        then(fallback_sender_role(false, std::nullopt));
        return;
    }
    api->database_get_item(
        kPluginFeaturesNamespace, kLocalScreenPresenceKey,
        [then, token](const nlohmann::json& record) {
            const std::optional<bool> present =
                local_screen_present(record, static_cast<int64_t>(std::time(nullptr)));
            token.defer("FallbackSenderRole::presence",
                        [then, present] { then(fallback_sender_role(false, present)); });
        },
        [then, token](const MoonrakerError& err) {
            // No record: no screen on the printer's host has ever announced
            // itself. Any other failure cannot rule one out.
            const std::optional<bool> present =
                err.is_not_found() ? std::optional<bool>(false) : std::nullopt;
            token.defer("FallbackSenderRole::presence_absent",
                        [then, present] { then(fallback_sender_role(false, present)); });
        });
}

} // namespace helix
