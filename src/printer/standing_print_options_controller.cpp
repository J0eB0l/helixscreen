// SPDX-License-Identifier: GPL-3.0-or-later

#include "standing_print_options_controller.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "moonraker_db_item.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standing_print_options.h"

#include <spdlog/spdlog.h>

#include <vector>

namespace helix {

using ui::observe;

namespace {
constexpr const char* kHandlerName = "StandingPrintOptions";
} // namespace

StandingPrintOptionsController::StandingPrintOptionsController() {
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](StandingPrintOptionsController* self, int seq) {
            if (seq > 0) {
                self->on_plugin_probed();
            }
        },
        ps.get_subjects_lifetime());

    // The inputs the backends render from: which backends exist, what they
    // last reported, and the user's unload-at-end choice.
    auto& ams = AmsState::instance();
    backend_count_observer_ = observe<int>(
        ams.get_backend_count_subject(), this,
        [](StandingPrintOptionsController* self, int) { self->refresh_record(); },
        ams.get_subjects_lifetime());
    data_revision_observer_ = observe<int>(
        ams.get_ams_data_revision_subject(), this,
        [](StandingPrintOptionsController* self, int) { self->refresh_record(); },
        ams.get_subjects_lifetime());
    auto& settings = SettingsManager::instance();
    unload_tools_observer_ = observe<int>(
        settings.subject_end_of_print_unload_tools(), this,
        [](StandingPrintOptionsController* self, int) { self->refresh_record(); },
        settings.get_subjects_lifetime());
}

std::string StandingPrintOptionsController::render_now() {
    auto& ams = AmsState::instance();
    std::vector<const AmsBackend*> backends;
    for (int i = 0; i < ams.backend_count(); ++i) {
        backends.push_back(ams.get_backend(i));
    }
    return render_standing_gcode(backends);
}

void StandingPrintOptionsController::on_plugin_probed() {
    auto& status = get_printer_state().plugin_status_state();
    screen_start_ledger().set_announce(
        announce_screen_start(status.plugin_feature(kPrintHooksFeature, kPrintHooksMinVersion)));
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    subscribe_history(*api);
    load_opt_in(*api);
    written_.reset();
    refresh_record();
}

void StandingPrintOptionsController::refresh_record() {
    // Only a plugin reads the record; without one there is nothing to keep.
    if (!get_printer_state().plugin_status_state().service_has_helix_plugin()) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || !api->is_connected()) {
        return;
    }
    std::string gcode = render_now();
    if (written_ && *written_ == gcode) {
        return;
    }
    written_ = gcode;
    spdlog::debug("[StandingPrintOptions] Record now: '{}'", gcode);
    post_item_if_changed(
        *api, kPluginFeaturesNamespace, kStandingOptionsKey, standing_options_record(gcode),
        [] { spdlog::info("[StandingPrintOptions] Record written"); },
        [this, token = lifetime_.token()](const MoonrakerError& err) {
            spdlog::warn("[StandingPrintOptions] Writing the record failed: {}", err.message);
            // The next refresh writes it again.
            token.defer("StandingPrintOptions::write_failed", [this] { written_.reset(); });
        });
}

void StandingPrintOptionsController::load_opt_in(IMoonrakerAPI& api) {
    api.database_get_item(
        kPluginFeaturesNamespace, kPluginFeaturesKey,
        [this, token = lifetime_.token()](const nlohmann::json& record) {
            const bool opted_in = plugin_feature_opted_in(record, kPrintHooksFeature, true);
            token.defer("StandingPrintOptions::opt_in", [this, opted_in] { opted_in_ = opted_in; });
        },
        [this, token = lifetime_.token()](const MoonrakerError&) {
            // No record: the default, off.
            token.defer("StandingPrintOptions::opt_in_absent", [this] { opted_in_ = false; });
        });
}

void StandingPrintOptionsController::subscribe_history(IMoonrakerAPI& api) {
    IMoonrakerClient& client = api.get_client();
    if (subscribed_client_ == &client) {
        return;
    }
    subscribed_client_ = &client;
    client.register_method_callback(
        "notify_history_changed", kHandlerName,
        [this, token = lifetime_.token()](const nlohmann::json& data) {
            // bg thread: parse only.
            const nlohmann::json* payload = json_util::notification_payload(data);
            if (payload == nullptr || json_util::safe_string(*payload, "action") != "added") {
                return;
            }
            const nlohmann::json* job = json_util::find_member(*payload, "job");
            if (job == nullptr || !job->is_object()) {
                return;
            }
            std::string filename = json_util::safe_string(*job, "filename");
            token.defer("StandingPrintOptions::job_added",
                        [this, filename = std::move(filename)] { on_job_added(filename); });
        });
}

void StandingPrintOptionsController::on_job_added(const std::string& filename) {
    const bool screen_started = screen_start_ledger().claim(filename);
    const PluginFeatureState hooks = get_printer_state().plugin_status_state().plugin_feature(
        kPrintHooksFeature, kPrintHooksMinVersion);
    const std::string gcode = render_now();
    const StandingOptionsSender sender =
        standing_options_sender(hooks, opted_in_, screen_started, !gcode.empty());
    if (sender != StandingOptionsSender::App) {
        spdlog::debug("[StandingPrintOptions] Job '{}' started: {}", filename,
                      sender == StandingOptionsSender::Plugin ? "the plugin sends the options"
                                                              : "no options to send");
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    spdlog::info("[StandingPrintOptions] Job '{}' started elsewhere, sending:\n{}", filename,
                 gcode);
    api->execute_gcode(
        gcode, [] {},
        [](const MoonrakerError& err) {
            spdlog::warn("[StandingPrintOptions] Sending the options failed: {}", err.message);
        },
        0, /*silent=*/true, nullptr, /*caller_surfaces_errors=*/false);
}

} // namespace helix
