// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "plr_backend.h"
#include "plr_wizard.h"
#include "print_lifecycle_state.h"

#include <optional>
#include <string>

class PlrOfferControllerTestAccess;

namespace helix {
class IMoonrakerAPI;
}

namespace helix::ui {

/// App-lifetime controller that offers the power-loss-recovery prompt at
/// connect time. Owned by SubjectInitializer (constructed in init_observers,
/// destroyed with it — the ObserverGuard members auto-reset via RAII).
///
/// Four backends feed ONE normalized "a resumable snapshot exists" signal, which
/// is what keeps the latch / re-arm / wizard logic backend-agnostic. Full
/// mechanism: docs/devel/POWER_LOSS_RECOVERY.md.
///
///   Snapmaker (PASSIVE): virtual_sdcard.pl_env_valid goes true on its own.
///   Qidi      (PASSIVE): save_variables.variables.was_interrupted reads true
///                        at boot after power loss; the stock macros keep it
///                        true during every normal print, so the printer_idle
///                        input of the offer decision scopes it to a fresh boot.
///   Creality  (ACTIVE):  print_stats.power_loss appearing in status marks the
///                        capability; availability requires firing the
///                        side-effectful `check_continue_print_state` probe,
///                        ONCE per connection, ONLY while the printer is in
///                        STANDBY, and getting both states back true.
///   Plugin    (ASKED):   with the HelixPrint plugin's `plr` feature Ready,
///                        server.helix.plr.status names its backend and whether
///                        it offers a resume. Side-effect free, asked once per
///                        connection once Klipper is ready.
///
/// The offer observers all watch PrinterState / app-global subjects, each
/// registered with that owner's SubjectLifetime. The authoritative account of
/// WHEN the offer fires and re-fires lives at the plr_should_offer decision site in
/// evaluate_offer (ui_plr_offer_controller.cpp); the observers below are just
/// the edges that drive it:
///   - pl_env_valid (PRIMARY Snapmaker trigger): a genuine 0->1 edge offers.
///   - plr_interrupted_flag (PRIMARY Qidi trigger): an edge re-evaluates the
///     offer; the capability half comes from discovery, not status.
///   - plr_power_loss_signal (PRIMARY Creality trigger): a 0->1 edge fires the
///     one-shot probe, whose response then offers.
///   - connection state: on a CONNECTED->not-CONNECTED edge, re-arms BOTH
///     one-shot latches (offer and probe), drops the cached Creality detect
///     result, AND forces the capability and availability subjects back to 0
///     (see on_connection_state_changed) so the next reconnect produces real
///     0->1 edges rather than same-value writes the subjects would swallow.
///   - wizard active: on a 1->0 edge (wizard closed) re-evaluates the offer,
///     so an offer that was suppressed only because the wizard owned the
///     screen now fires. This is what makes wizard suppression temporary
///     instead of permanent.
///   - print lifecycle: a job that drops straight to Idle (Klipper restarted
///     under it) re-evaluates the offer and retries the Creality probe, both
///     of which the dead job's lifecycle held back.
///
/// Observer callbacks all run on the main thread — observe<int> defers via
/// the update queue and plr_should_offer/plr_should_rearm are pure. The one
/// genuine thread crossing is the probe response (a JSON-RPC callback on the
/// WebSocket thread), which is wrapped in lifetime_.bg_cb().
class PlrOfferController {
    friend class ::PlrOfferControllerTestAccess;

  public:
    PlrOfferController();
    ~PlrOfferController() = default;

    PlrOfferController(const PlrOfferController&) = delete;
    PlrOfferController& operator=(const PlrOfferController&) = delete;

  private:
    // Single evaluation point: reads both capability subjects plus
    // printer/wizard state live, normalizes them into one availability signal,
    // and offers when plr_should_offer() agrees AND a plan with a permitted
    // resume can be built. Every observer routes here so the decision lives in
    // exactly one place.
    void evaluate_offer();
    void on_pl_env_valid_changed(int pl_env_valid);
    void on_plr_interrupted_flag_changed(int was_interrupted);
    void on_creality_capable_changed(int capable);
    void on_connection_state_changed(int new_conn_state);
    void on_wizard_active_changed(int wizard_active);
    /// A held -> Idle edge with no terminal state between re-evaluates the
    /// offer (and the Creality probe) the dead job's lifecycle held back.
    void on_print_lifecycle_changed(PrintState next);

    /// Fire the Creality probe at most once per connection, and only from
    /// STANDBY. See the header warning on
    /// IMoonrakerAPI::check_continue_print_state — this call has side effects.
    void probe_creality_once();
    void on_creality_detect_result(const helix::PlrDetectResult& result);

    /// Ask the plugin for its recovery status once per connection, when the
    /// link is up, Klipper is ready and the `plr` feature is Ready.
    void request_plugin_status();
    void on_plugin_status(const helix::PlrPluginStatus& status);
    /// Listen for helix:plr_status on the current API, once per API. The plugin
    /// sends it each time its Klippy-ready handling finishes, so an offer that
    /// becomes available after the first ask still reaches the screen.
    void listen_for_plugin_status();
    void on_plugin_status_notify(const nlohmann::json& msg);
    /// The `plr` feature changed state (Absent / Disabled / Ready): the
    /// plugin's earlier answer is stale, and its profile may need writing.
    void on_plr_feature_changed(int state);
    /// Resolve and write `plr_profile` once per connection and printer type,
    /// while the feature is available at all: the plugin decides from it
    /// whether to save, so it has to land even while saving is off.
    void publish_profile();

    ObserverGuard pl_valid_observer_;
    ObserverGuard interrupted_flag_observer_;
    ObserverGuard creality_capable_observer_;
    ObserverGuard conn_observer_;
    ObserverGuard wizard_observer_;
    ObserverGuard lifecycle_observer_;
    ObserverGuard plr_feature_observer_;
    ObserverGuard printer_type_observer_;
    ObserverGuard plugin_probe_observer_;
    ObserverGuard klippy_observer_;

    /// Guards the probe response, which arrives on the WebSocket thread.
    helix::AsyncLifetimeGuard lifetime_;

    bool prompted_this_connect_ = false;
    bool creality_probed_this_connect_ = false;
    /// Cached probe outcome. `completed` is the resume authorization — never
    /// set it anywhere but on_creality_detect_result.
    helix::PlrDetectResult creality_detect_{};
    std::string creality_recovery_file_;
    int last_conn_state_ = 0;
    PrintState last_lifecycle_ = PrintState::Idle;
    /// Prompts shown over the controller's life.
    int offers_shown_ = 0;

    /// The plugin's answer this connection; nullopt until it answers.
    std::optional<helix::PlrPluginStatus> plugin_status_;
    bool plugin_status_inflight_ = false;
    int last_plr_feature_state_ = 0;
    /// The plugin probe of this connection has answered, so the printer type
    /// discovery resolved for this connection is in place.
    bool probe_answered_this_connect_ = false;
    /// The printer type whose profile this connection wrote; empty when none.
    std::optional<std::string> published_profile_type_;
    /// Counts connections and pushed statuses, so a status reply from a
    /// dropped link, or older than a pushed status, is ignored.
    int connection_generation_ = 0;
    /// The API the status notification is registered on.
    helix::IMoonrakerAPI* status_notify_api_ = nullptr;
    static constexpr const char* kStatusNotifyHandler = "plr_offer";
};

} // namespace helix::ui
