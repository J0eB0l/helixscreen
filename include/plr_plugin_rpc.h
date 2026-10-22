// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The HelixPrint plugin's power-loss endpoints (server.helix.plr.*) as calls.
// Each reads its reply with the plr_wizard.h readers and answers on whatever
// thread the client delivers on: callers marshal to the main thread.

#include "moonraker_error.h"
#include "plr_wizard.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

class IMoonrakerAPI;

namespace plr_rpc {

using ErrorCallback = std::function<void(const MoonrakerError&)>;

/// A plan scan runs in the plugin's worker; a long one answers `pending` well
/// inside this and follows with notify_plr_plan_ready.
inline constexpr uint32_t kPlanTimeoutMs = 120000;
/// prepare answers once the bed (and chamber) reached temperature, the head
/// lifted and X/Y homed: a cold bed on a large printer takes many minutes.
inline constexpr uint32_t kPrepareTimeoutMs = 45U * 60U * 1000U;
/// commit answers after the nozzle heated, purged and the print started.
inline constexpr uint32_t kCommitTimeoutMs = 30U * 60U * 1000U;

void request_status(IMoonrakerAPI& api, std::function<void(const PlrPluginStatus&)> on_ok,
                    ErrorCallback on_error);

/// A reply that is neither pending nor a plan reaches @p on_error.
void request_plan(IMoonrakerAPI& api, std::function<void(const PlrPlanReply&)> on_ok,
                  ErrorCallback on_error);

void prepare(IMoonrakerAPI& api, const std::string& plan_id, bool skip_chamber_wait,
             std::function<void()> on_ok, ErrorCallback on_error);

void commit(IMoonrakerAPI& api, const std::string& plan_id,
            const std::vector<PlrConfirmation>& given, int tier, std::function<void()> on_ok,
            ErrorCallback on_error);

void abort(IMoonrakerAPI& api, const std::string& plan_id, std::function<void()> on_ok,
           ErrorCallback on_error);

void discard(IMoonrakerAPI& api, std::function<void()> on_ok, ErrorCallback on_error);

/// Write `helix-screen` / `plr_profile`.
void write_profile(IMoonrakerAPI& api, const nlohmann::json& profile, std::function<void()> on_ok,
                   ErrorCallback on_error);

/// Write `helix-screen` / `plr_user`, the plugin's saving switch.
void write_user(IMoonrakerAPI& api, bool enabled, std::function<void()> on_ok,
                ErrorCallback on_error);

/// Re-read the plugin's features map into PrinterState, so a switch or profile
/// the plugin just read shows up as the `plr` feature's new state.
void refresh_features(IMoonrakerAPI& api);

} // namespace plr_rpc
} // namespace helix
