// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_error.h"

#include <functional>
#include <string>

#include "hv/json.hpp"

namespace helix {

class IMoonrakerAPI;

/// Write a Moonraker database item unless the stored value already equals
/// @p value, so several screens on one printer do not rewrite it in turn.
/// @p on_written runs only after an actual write. Callbacks arrive on the
/// client's thread. @p api must outlive the reply, as the API object does.
void post_item_if_changed(IMoonrakerAPI& api, const std::string& namespace_name,
                          const std::string& key, const nlohmann::json& value,
                          std::function<void()> on_written,
                          std::function<void(const MoonrakerError&)> on_error);

} // namespace helix
