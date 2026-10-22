// SPDX-License-Identifier: GPL-3.0-or-later

// Standing print options: the choice between the plugin and the screen at a
// job start, telling the screen's own starts from foreign ones, and the
// rendered record coming from the same backend function as the screen's start.

#include "../lvgl_test_fixture.h"
#include "ams_backend.h"
#include "ams_backend_mock.h"
#include "config.h"
#include "helix_plugin_features.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "snapmaker_print_preferences.h"
#include "standing_print_options.h"

#if HELIX_HAS_SNAPMAKER
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#endif

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

PluginFeatureEntry entry(int version, bool available, bool enabled, std::string reason = "") {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    e.reason = std::move(reason);
    return e;
}

PluginFeatureMap hooks_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    features[kPrintHooksFeature] = entry(version, available, enabled);
    return features;
}

/// A backend whose standing gcode is whatever the test says.
class FixedStandingBackend : public AmsBackendMock {
  public:
    explicit FixedStandingBackend(std::string gcode) : gcode_(std::move(gcode)) {}
    std::string standing_print_gcode() const override {
        return gcode_;
    }

  private:
    std::string gcode_;
};

} // namespace

TEST_CASE("standing options go to exactly one sender per plugin state", "[plugin_print_hooks]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        bool opted_in;
        StandingOptionsSender expected;
    };
    const Row rows[] = {
        {"no plugin, opted in", false, hooks_map(1, true, true), true, StandingOptionsSender::App},
        {"no plugin, not opted in", false, {}, false, StandingOptionsSender::None},
        {"old plugin without a features map", true, {}, true, StandingOptionsSender::App},
        {"contract below the minimum", true, hooks_map(0, true, true), true,
         StandingOptionsSender::App},
        {"unavailable", true, hooks_map(1, false, false), true, StandingOptionsSender::App},
        {"switched off in the plugin", true, hooks_map(1, true, false), true,
         StandingOptionsSender::None},
        {"ready", true, hooks_map(1, true, true), true, StandingOptionsSender::Plugin},
        {"ready, stale opt-in read", true, hooks_map(1, true, true), false,
         StandingOptionsSender::Plugin},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        const PluginFeatureState state = plugin_feature_state(
            row.plugin_available, row.features, kPrintHooksFeature, kPrintHooksMinVersion);
        CHECK(standing_options_sender(state, row.opted_in, /*screen_started=*/false,
                                      /*has_gcode=*/true) == row.expected);
        INFO("the screen's own start already sent its options");
        CHECK(standing_options_sender(state, row.opted_in, /*screen_started=*/true, true) ==
              StandingOptionsSender::None);
        INFO("nothing rendered");
        CHECK(standing_options_sender(state, row.opted_in, false, /*has_gcode=*/false) ==
              StandingOptionsSender::None);
    }
}

TEST_CASE("only a ready print_hooks feature hears of the screen's starts", "[plugin_print_hooks]") {
    CHECK(announce_screen_start(PluginFeatureState::Ready));
    CHECK_FALSE(announce_screen_start(PluginFeatureState::Disabled));
    CHECK_FALSE(announce_screen_start(PluginFeatureState::Absent));
}

TEST_CASE("the ledger tells the screen's starts from foreign ones", "[plugin_print_hooks]") {
    ScreenStartLedger ledger;
    const auto t0 = ScreenStartLedger::Clock::time_point{} + 1000s;

    SECTION("a start the screen noted is its own, once") {
        ledger.note("benchy.gcode", t0);
        CHECK_FALSE(ledger.claim("cube.gcode", t0 + 1s));
        CHECK(ledger.claim("benchy.gcode", t0 + 1s));
        CHECK_FALSE(ledger.claim("benchy.gcode", t0 + 2s));
    }

    SECTION("nothing noted is a foreign start") {
        CHECK_FALSE(ledger.claim("benchy.gcode", t0));
    }

    SECTION("a noted start lapses after its window") {
        ledger.note("benchy.gcode", t0);
        CHECK_FALSE(ledger.claim("benchy.gcode", t0 + ScreenStartLedger::kWindow + 1s));
    }

    SECTION("a start whose job name is unknown claims the next job") {
        ledger.note_any(t0);
        CHECK(ledger.claim(".helix_print/abcd_benchy.gcode", t0 + 1s));
        CHECK_FALSE(ledger.claim("cube.gcode", t0 + 2s));
    }

    SECTION("a slow start waits the window it asked for") {
        ledger.note_any(t0, 30min);
        CHECK(ledger.claim("benchy.gcode", t0 + 20min));
    }
}

TEST_CASE("the stored opt-in follows the plugin's rule", "[plugin_print_hooks]") {
    CHECK(plugin_feature_opted_in(json{{"print_hooks", true}}, "print_hooks", true));
    CHECK_FALSE(plugin_feature_opted_in(json{{"print_hooks", false}}, "print_hooks", false));
    INFO("absent or not a bool: the default, off for a feature that acts");
    CHECK_FALSE(plugin_feature_opted_in(json::object(), "print_hooks", true));
    CHECK_FALSE(plugin_feature_opted_in(json{{"print_hooks", 1}}, "print_hooks", true));
    CHECK_FALSE(plugin_feature_opted_in(json(), "print_hooks", true));
    CHECK(plugin_feature_opted_in(json::object(), "journal", false));
}

TEST_CASE_METHOD(LVGLTestFixture, "the record joins every backend's standing gcode",
                 "[plugin_print_hooks]") {
    const FixedStandingBackend first{"M117 one"};
    const FixedStandingBackend silent{""};
    const FixedStandingBackend second{"M117 two"};

    CHECK(render_standing_gcode({&first, nullptr, &silent, &second}) == "M117 one\nM117 two");
    CHECK(render_standing_gcode({&silent}).empty());
    CHECK(render_standing_gcode({}).empty());
    CHECK(standing_options_record("M117 one") == json{{"version", 1}, {"gcode", "M117 one"}});
}

TEST_CASE("a forced preference write carries FORCE=1 last", "[plugin_print_hooks]") {
    snapmaker::PrintPreferences changes;
    changes.end_unload_filament = {true, false};
    changes.force = true;
    CHECK(snapmaker::write_print_preferences_gcode(changes) ==
          "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0] FORCE=1");

    INFO("nothing to write stays nothing");
    snapmaker::PrintPreferences empty;
    empty.force = true;
    CHECK(snapmaker::write_print_preferences_gcode(empty).empty());
}

namespace {

/// The mock, plus every JSON-RPC method sent through it, in order.
struct MethodRecordingClient : public MoonrakerClientMock {
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;
    std::vector<std::pair<std::string, json>> sent;

    RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 std::function<void(const json&)> success_cb,
                 std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
                 bool silent = false,
                 std::optional<rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        sent.emplace_back(method, params);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }
};

struct JobStartFx : LVGLTestFixture {
    MethodRecordingClient client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;

    JobStartFx() {
        ps.init_subjects(false);
        ps.set_klippy_state_sync(KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        while (screen_start_ledger().claim("")) {
        }
    }
    ~JobStartFx() override {
        screen_start_ledger().set_announce(false);
        while (screen_start_ledger().claim("")) {
        }
        client.stop_temperature_simulation();
        client.disconnect();
    }

    std::vector<std::string> methods() const {
        std::vector<std::string> out;
        for (const auto& s : client.sent)
            out.push_back(s.first);
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(JobStartFx, "a start from this screen is noted before it is sent",
                 "[plugin_print_hooks]") {
    MoonrakerAPI api{client, ps};

    SECTION("the job history adds for it is the screen's own") {
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        CHECK(screen_start_ledger().claim("benchy.gcode"));
        CHECK_FALSE(screen_start_ledger().claim("cube.gcode"));
    }

    SECTION("a ready print_hooks feature hears of it first") {
        screen_start_ledger().set_announce(true);
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        const auto sent = methods();
        const auto announce = std::find(sent.begin(), sent.end(), kScreenStartRpc);
        const auto start = std::find(sent.begin(), sent.end(), "printer.print.start");
        REQUIRE(announce != sent.end());
        REQUIRE(start != sent.end());
        CHECK(announce < start);
        CHECK(client.sent[static_cast<size_t>(announce - sent.begin())].second ==
              json{{"filename", "benchy.gcode"}});
    }

    SECTION("otherwise the plugin is not told") {
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        const auto sent = methods();
        CHECK(std::find(sent.begin(), sent.end(), kScreenStartRpc) == sent.end());
    }
}

TEST_CASE("a plugin switch row's mode over every plugin state", "[plugin_feature]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        PluginSwitchMode expected;
    };
    const Row rows[] = {
        {"no plugin", false, hooks_map(1, true, true), PluginSwitchMode::NeedsPlugin},
        {"no plugin, nothing reported", false, {}, PluginSwitchMode::NeedsPlugin},
        {"old plugin without a features map", true, {}, PluginSwitchMode::NeedsUpdate},
        {"contract below the minimum", true, hooks_map(0, true, true),
         PluginSwitchMode::NeedsUpdate},
        {"unavailable on this printer", true, hooks_map(1, false, false), PluginSwitchMode::Hidden},
        {"unavailable, below the minimum too", true, hooks_map(0, false, false),
         PluginSwitchMode::NeedsUpdate},
        {"switched off", true, hooks_map(1, true, false), PluginSwitchMode::Switch},
        {"ready", true, hooks_map(1, true, true), PluginSwitchMode::Switch},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        CHECK(plugin_switch_mode(row.plugin_available, row.features, kPrintHooksFeature,
                                 kPrintHooksMinVersion) == row.expected);
    }
    INFO("another feature's entry does not stand in");
    PluginFeatureMap other;
    other["plr"] = entry(1, true, true);
    CHECK(plugin_switch_mode(true, other, kPrintHooksFeature, kPrintHooksMinVersion) ==
          PluginSwitchMode::NeedsUpdate);
}

TEST_CASE("the plugin overview lists every reported feature with its status", "[plugin_feature]") {
    PluginFeatureMap features;
    features["plr"] = entry(1, true, false);
    features["convert_3mf"] = entry(1, true, true);
    features["print_hooks"] = entry(1, true, true);
    features["zz_future"] = entry(2, true, true);
    features["aa_broken"] = entry(1, false, false, "load_failed");

    const auto lines = plugin_feature_lines(features);

    REQUIRE(lines.size() == 5);
    INFO("known features in table order, with their settings home");
    CHECK(lines[0].name == "print_hooks");
    CHECK(lines[0].label == "Start options for prints from other apps");
    CHECK(lines[0].status == PluginFeatureStatus::Active);
    CHECK(lines[0].home == PluginFeatureHome::Printing);
    CHECK(lines[1].name == "plr");
    CHECK(lines[1].status == PluginFeatureStatus::Off);
    CHECK(lines[1].home == PluginFeatureHome::Printing);
    CHECK(lines[2].name == "convert_3mf");
    CHECK(lines[2].home == PluginFeatureHome::None);

    INFO("unknown features follow by name, with no setting to lead to");
    CHECK(lines[3].name == "aa_broken");
    CHECK(lines[3].label == "aa_broken");
    CHECK(lines[3].status == PluginFeatureStatus::Unavailable);
    CHECK(lines[3].reason == "load_failed");
    CHECK(lines[3].home == PluginFeatureHome::None);
    CHECK(lines[3].icon == kUnknownPluginFeatureIcon);
    CHECK(lines[4].name == "zz_future");

    CHECK(plugin_feature_lines({}).empty());
}

#if HELIX_HAS_SNAPMAKER

namespace {
struct StandingU1Fx : LVGLTestFixture {
    StandingU1Fx() {
        helix::Config::get_instance();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_end_of_print_unload_tools(0b0101);
    }
    ~StandingU1Fx() override {
        SettingsManager::instance().set_end_of_print_unload_tools(0);
    }
};
} // namespace

TEST_CASE_METHOD(StandingU1Fx, "the record carries the unload list the screen's own start sends",
                 "[plugin_print_hooks][end_unload]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SECTION("nothing before the firmware reports its heads") {
        CHECK(backend.standing_print_gcode().empty());
        CHECK(render_standing_gcode({&backend}).empty());
    }

    SECTION("the same list, forced for a print already running") {
        SnapmakerTestAccess::handle_status(
            backend,
            json{{"print_task_config", {{"end_unload_filament", {false, false, false, false}}}}});
        const std::string own_start = backend.build_preprint_gcode({}, {});
        REQUIRE(own_start == "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,1,0]");
        CHECK(backend.standing_print_gcode() == own_start + " FORCE=1");
        CHECK(render_standing_gcode({&backend}) == own_start + " FORCE=1");

        INFO("a later setting edit changes both alike");
        SettingsManager::instance().set_end_of_print_unload_tools(0b0010);
        CHECK(backend.standing_print_gcode() == backend.build_preprint_gcode({}, {}) + " FORCE=1");
    }

    SECTION("no routing rides along: it is refused once a print runs") {
        SnapmakerTestAccess::handle_status(
            backend,
            json{{"print_task_config", {{"end_unload_filament", {false, false, false, false}}}}});
        const std::string standing = backend.standing_print_gcode();
        CHECK(standing.find("SET_PRINT_EXTRUDER_MAP") == std::string::npos);
        CHECK(standing.find("SET_PRINT_USED_EXTRUDERS") == std::string::npos);
    }
}

#endif // HELIX_HAS_SNAPMAKER
