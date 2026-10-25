// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The HelixPrint plugin's power-loss endpoints (server.helix.plr.*) as calls.
// Each reads its reply with the plr_wizard.h readers and answers on whatever
// thread the client delivers on: callers marshal to the main thread.

#include "helix_plugin_features.h"
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
/// prepare answers once the bed, chamber and nozzle reached temperature, the
/// head lifted and X/Y homed. The plugin's own waits allow bed 20 + chamber
/// 45 + nozzle 10 minutes, so this covers their sum plus the motion.
inline constexpr uint32_t kPrepareTimeoutMs = 90U * 60U * 1000U;
/// commit runs the same heat waits again, then purges and starts the print.
inline constexpr uint32_t kCommitTimeoutMs = 90U * 60U * 1000U;

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

/// server.helix.plr.mesh_include without an action: whether printer.cfg
/// carries the include line a resume's mesh carry-over needs.
void request_mesh_include_status(IMoonrakerAPI& api, std::function<void(bool present)> on_ok,
                                 ErrorCallback on_error);

/// server.helix.plr.mesh_include with an action. @p on_ok gets the reply's
/// `present` and `changed`; the plugin refuses with a coded error when
/// printer.cfg cannot be read or written, and the change reaches Klipper
/// only after a firmware restart, which the caller offers.
void mesh_include_action(IMoonrakerAPI& api, bool add,
                         std::function<void(bool present, bool changed)> on_ok,
                         ErrorCallback on_error);

/// Write `helix-screen` / `plr_profile` unless the stored record already equals
/// @p profile, so several screens on one printer do not rewrite it in turn.
/// @p on_stored runs once the stored record equals @p profile: after the write,
/// or as soon as the read shows it already did.
void write_profile(IMoonrakerAPI& api, const nlohmann::json& profile,
                   std::function<void()> on_stored, ErrorCallback on_error);

/// Write `helix-screen` / `plr_user`, the plugin's saving switch.
void write_user(IMoonrakerAPI& api, bool enabled, std::function<void()> on_ok,
                ErrorCallback on_error);

/// Re-read the plugin's features map into PrinterState, so a switch or profile
/// the plugin just read shows up as the `plr` feature's new state. @p announce
/// says whether an unchanged map still counts as a probe.
void refresh_features(IMoonrakerAPI& api, FeatureAnnounce announce = FeatureAnnounce::Always);

} // namespace plr_rpc
} // namespace helix
