// SPDX-License-Identifier: GPL-3.0-or-later

// HELIX_MOCK_AMS=indx: mock hardware the production AmsBackendIndx runs on.

#include "indx_contract.h"
#include "macro_param_cache.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Captures notify_status_update frames and waits for one whose
/// save_variables satisfy a predicate. Dispatch runs on the mock's thread.
class IndxFrames {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(n);
            cv_.notify_all();
        };
    }

    size_t mark() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    bool wait_for_active(size_t after, int tool, int timeout_ms = 6000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (size_t i = after; i < frames_.size(); ++i) {
                const auto& params = frames_[i]["params"];
                if (!params.is_array() || params.empty()) {
                    continue;
                }
                auto r = helix::indx::read_status(params[0]);
                if (r && r->active_tool == tool) {
                    return true;
                }
            }
            return false;
        });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<json> frames_;
};

struct IndxEnv {
    ~IndxEnv() {
        helix::MacroParamCache::instance().clear();
    }
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", "indx"};
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
    helix::ScopedEnv fault;
    helix::ScopedEnv tools{"HELIX_MOCK_INDX_TOOLS", "4"};
    explicit IndxEnv(const char* fault_value = nullptr)
        : fault("HELIX_MOCK_INDX_FAULT", fault_value) {}
};

} // namespace

TEST_CASE("INDX mock: real discovery finds INDX with the stock contract", "[ams][indx][mock]") {
    IndxEnv env;
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
    const helix::PrinterDiscovery hw = mock.hardware();
    CHECK(hw.mmu_type() == helix::AmsType::INDX);
    CHECK(helix::indx::config_tool_count() == 4);
    CHECK(helix::indx::check_contract(hw).recognized);
}

TEST_CASE("INDX mock: the Kalico shape detects from configfile alone", "[ams][indx][mock]") {
    IndxEnv env("kalico");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
    const helix::PrinterDiscovery hw = mock.hardware();
    CHECK(hw.mmu_type() == helix::AmsType::INDX);
}

TEST_CASE("INDX mock: a missing CHANGE_TOOL is not recognized", "[ams][indx][mock]") {
    IndxEnv env("unrecognized");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
    auto contract = helix::indx::check_contract(mock.hardware());
    CHECK_FALSE(contract.recognized);
}

TEST_CASE("INDX mock: a change parks, picks up and records the tool before acking",
          "[ams][indx][mock]") {
    IndxEnv env;
    // Declared before the mock, so it outlives the simulation thread that
    // calls into it.
    IndxFrames frames;
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
    mock.register_notify_update(frames.callback());

    const size_t mark = frames.mark();
    REQUIRE(mock.gcode_script("T2") == 0);
    // The macro has finished when the gcode acks: the tool is already recorded.
    auto now = helix::indx::read_status(mock.indx_status_json());
    REQUIRE(now.has_value());
    CHECK(now->active_tool == 2);
    // And the park and the pickup both went out as frames, with no
    // simulation thread running: the change publishes them itself.
    CHECK(frames.wait_for_active(mark, -1, 0));
    CHECK(frames.wait_for_active(mark, 2, 0));
}

TEST_CASE("INDX mock: no heat with an empty head", "[ams][indx][mock][heat]") {
    IndxEnv env;
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
    REQUIRE(mock.gcode_script("MANUAL_TOOL_REMOVE") == 0);
    mock.gcode_script("M104 S210");
    CHECK(mock.extruder_target() == Catch::Approx(0.0));
    mock.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210");
    CHECK(mock.extruder_target() == Catch::Approx(0.0));

    REQUIRE(mock.gcode_script("MANUAL_TOOL_SEAT TOOL=1") == 0);
    mock.gcode_script("M104 S210");
    CHECK(mock.extruder_target() == Catch::Approx(210.0));
}
