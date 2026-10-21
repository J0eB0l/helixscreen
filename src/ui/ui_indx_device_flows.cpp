// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_error_reporting.h"
#include "ui_guided_device_flow.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "save_config_restart.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>

namespace helix::ui {

namespace {

// A tool cycle or a heater fit runs for minutes with nothing else on screen
// to show it, so one bound status line under the section's buttons carries
// it: running, then the result. ui_xml/components/indx_flow_status.xml renders
// these subjects (phase: 0 nothing to report, 1 running, 2 result). Only one
// INDX flow runs at a time, so the three share it.
lv_subject_t s_indx_flow_phase{};
lv_subject_t s_indx_flow_status{};
char s_indx_flow_status_buf[128] = {};
bool s_indx_flow_subjects_ready = false;

constexpr int kPhaseIdle = 0;
constexpr int kPhaseRunning = 1;
constexpr int kPhaseResult = 2;

/// Subjects initialize once per process (a second init would memzero a subject whose observers
/// are live) and always register in the global scope, which is where the status line's XML binds
/// them. A global registry that lost them is repopulated without re-initializing.
void ensure_flow_subjects() {
    if (!s_indx_flow_subjects_ready) {
        s_indx_flow_subjects_ready = true;
        lv_subject_init_int(&s_indx_flow_phase, kPhaseIdle);
        lv_subject_init_string(&s_indx_flow_status, s_indx_flow_status_buf, nullptr,
                               sizeof(s_indx_flow_status_buf), "");
    }
    if (lv_xml_get_subject(nullptr, "indx_flow_phase") != &s_indx_flow_phase) {
        lv_xml_register_subject(nullptr, "indx_flow_phase", &s_indx_flow_phase);
        lv_xml_register_subject(nullptr, "indx_flow_status", &s_indx_flow_status);
    }
}

void set_flow_state(const std::string& text, int phase) {
    ensure_flow_subjects();
    lv_subject_copy_string(&s_indx_flow_status, text.c_str());
    lv_subject_set_int(&s_indx_flow_phase, phase);
    // The backend's gates change with the run; the section shows that once
    // rebuilt. Never builds the overlay: a completion can land during teardown.
    if (auto* overlay = lazy_global_if_exists<AmsDeviceSectionDetailOverlay>()) {
        overlay->refresh();
    }
}

bool flow_running() {
    ensure_flow_subjects();
    return lv_subject_get_int(&s_indx_flow_phase) == kPhaseRunning;
}

/// The save watch outlives any one flow. Held in an optional and destroyed
/// from a StaticSubjectRegistry deinit, while LVGL is still up: its destructor
/// resets an observer on a subject lv_deinit() frees.
std::optional<SaveConfigWatch>& save_watch_storage() {
    static std::optional<SaveConfigWatch> storage;
    return storage;
}

SaveConfigWatch& save_watch() {
    auto& storage = save_watch_storage();
    if (!storage) {
        storage.emplace();
        StaticSubjectRegistry::instance().register_deinit("indx::save_watch",
                                                          [] { save_watch_storage().reset(); });
    }
    return *storage;
}

/// Run @p action_id on the backend, tracked: the status line says
/// @p running_text until @p on_done reports the end.
void run_tracked(const char* action_id, const std::string& running_text,
                 AmsBackend::DeviceActionDone on_done) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    if (flow_running()) {
        NOTIFY_WARNING("{}", lv_tr("Wait for the running INDX operation to finish"));
        return;
    }
    set_flow_state(running_text, kPhaseRunning);
    const AmsError err = backend->run_device_action(action_id, {}, std::move(on_done));
    if (!err.success()) {
        set_flow_state("", kPhaseIdle);
        notify_ams_error(err);
    }
}

void save_calibration() {
    IMoonrakerAPI* api = get_moonraker_api();
    set_flow_state(lv_tr("Saving calibration..."), kPhaseResult);
    save_watch().begin(
        api, lv_tr("Saving calibration"),
        [] { set_flow_state(lv_tr("Calibration saved"), kPhaseResult); },
        [](const std::string& message) {
            set_flow_state(fmt::format(fmt::runtime(lv_tr("Save failed: {}")), message),
                           kPhaseResult);
        });
}

void calibration_done(const AmsError& result) {
    if (result.success()) {
        set_flow_state(lv_tr("Calibration finished, not saved yet"), kPhaseResult);
        ConfirmOptions options;
        options.cancel_text = lv_tr("Later");
        modal_confirm(
            lv_tr("Save calibration"),
            lv_tr("Save the new heater values to printer.cfg? Klipper restarts."),
            ModalSeverity::Warning, lv_tr("Save & Restart"), [] { save_calibration(); }, options);
        return;
    }
    if (result.result == AmsResult::TIMEOUT) {
        set_flow_state(lv_tr("No answer from the printer; it may still be calibrating"),
                       kPhaseResult);
        return;
    }
    set_flow_state(fmt::format(fmt::runtime(lv_tr("Calibration failed: {}")), result.user_msg),
                   kPhaseResult);
}

void confirm_calibration(const char* action_id, const char* title, const char* body) {
    const std::string id = action_id;
    modal_confirm(title, body, ModalSeverity::Warning, lv_tr("Start"), [id] {
        run_tracked(id.c_str(), lv_tr("Calibrating... (several minutes)"), calibration_done);
    });
}

void confirm_cycle_all() {
    modal_confirm(
        lv_tr("Cycle every tool"),
        lv_tr("Every tool is picked up and parked in turn. The head ends empty. This "
              "takes a few minutes."),
        ModalSeverity::Warning, lv_tr("Start"), [] {
            run_tracked("cycle_all", lv_tr("Cycling tools..."), [](const AmsError& result) {
                set_flow_state(result.success()
                                   ? std::string(lv_tr("Tool cycle finished"))
                                   : fmt::format(fmt::runtime(lv_tr("Tool cycle failed: {}")),
                                                 result.user_msg),
                               kPhaseResult);
            });
        });
}

/// The status line, once per section: the section's flows share it.
void attach_status(lv_obj_t* actions_container) {
    ensure_flow_subjects();
    if (lv_obj_find_by_name(actions_container, "indx_flow_status_line") != nullptr) {
        return;
    }
    lv_xml_create(actions_container, "indx_flow_status", nullptr);
}

} // namespace

void register_indx_device_flows() {
    register_guided_device_flow("indx_cycle_all",
                                {attach_status, [](const std::string&) { confirm_cycle_all(); }});
    register_guided_device_flow("indx_heater_calibration",
                                {attach_status, [](const std::string&) {
                                     confirm_calibration(
                                         "calibrate_heater", lv_tr("Calibrate heater"),
                                         lv_tr("The mounted nozzle heats to 200°C and cools "
                                               "again while the heater's safety model is "
                                               "fitted. Keep the part fan off and flammable "
                                               "material away from the head. This takes "
                                               "several minutes."));
                                 }});
    register_guided_device_flow("indx_fan_calibration",
                                {attach_status, [](const std::string&) {
                                     confirm_calibration(
                                         "calibrate_fan", lv_tr("Calibrate part fan"),
                                         lv_tr("The nozzle heats to 200°C while the part fan "
                                               "steps through its speeds. For an accurate fit, "
                                               "fit the duct as you print with it. This takes "
                                               "several minutes."));
                                 }});
}

} // namespace helix::ui
