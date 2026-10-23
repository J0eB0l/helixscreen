// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "helix_plugin_features.h"
#include "storage_report.h"
#include "storage_report_service.h"

#include <ctime>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::storage;
using nlohmann::json;

namespace {

std::size_t consumer_at(const char* key) {
    const auto i = consumer_index(key);
    REQUIRE(i.has_value());
    return *i;
}

json status_with(const json& entry) {
    return json{{"api", 2}, {"features", {{kStorageFeature, entry}}}};
}

PluginFeatureState storage_state(bool installed, const json& status) {
    return plugin_feature_state(installed, parse_plugin_features(status), kStorageFeature,
                                kStorageMinVersion);
}

const json kReady = {{"version", 1}, {"available", true}, {"enabled", true}, {"reason", nullptr}};

constexpr uint64_t kMb = 1024ULL * 1024ULL;

} // namespace

// ============================================================================
// Where the report comes from
// ============================================================================

TEST_CASE("Storage reads the plugin's report only when its feature is ready",
          "[plugin_storage][plugin_feature]") {
    CHECK(report_source(storage_state(true, status_with(kReady))) == ReportSource::Plugin);

    json disabled = kReady;
    disabled["enabled"] = false;
    json unavailable = kReady;
    unavailable["available"] = false;
    unavailable["reason"] = "load_failed";
    json old = kReady;
    old["version"] = 0;

    CHECK(report_source(storage_state(false, status_with(kReady))) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, json{{"version", "1.2.0"}})) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, status_with(disabled))) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, status_with(unavailable))) == ReportSource::FileApi);
    CHECK(report_source(storage_state(true, status_with(old))) == ReportSource::FileApi);
}

TEST_CASE("Automatic cleanup is plugin-only: greyed with Install without the plugin",
          "[plugin_storage][plugin_feature]") {
    const PluginFeatureMap none;
    CHECK(plugin_switch_mode(false, none, kAutoCleanupFeature, kAutoCleanupMinVersion, false,
                             true) == PluginSwitchMode::NeedsPlugin);
    const PluginFeatureMap ready =
        parse_plugin_features(json{{"features", {{kAutoCleanupFeature, kReady}}}});
    CHECK(plugin_switch_mode(true, ready, kAutoCleanupFeature, kAutoCleanupMinVersion, false,
                             false) == PluginSwitchMode::Switch);
}

// ============================================================================
// The plugin's report
// ============================================================================

TEST_CASE("A plugin storage report parses per consumer", "[plugin_storage]") {
    const json result = {
        {"volume", {{"total", 1000 * kMb}, {"used", 600 * kMb}, {"free", 400 * kMb}}},
        {"floor", 100 * kMb},
        {"consumers", json::array({
                          {{"name", "helix_temp"},
                           {"bytes", 300},
                           {"files", 2},
                           {"available", true},
                           {"cleanable", true}},
                          {{"name", "user_gcode"},
                           {"bytes", 5000},
                           {"files", 7},
                           {"available", true},
                           {"cleanable", true}},
                          {{"name", "timelapse"},
                           {"bytes", 0},
                           {"files", 0},
                           {"available", false},
                           {"cleanable", false}},
                          {{"name", "from_a_newer_plugin"}, {"bytes", 9}, {"available", true}},
                      })},
    };
    const auto report = parse_storage_result(result);
    REQUIRE(report.has_value());
    CHECK(report->total == 1000 * kMb);
    CHECK(report->free == 400 * kMb);

    const ConsumerUsage& temp = report->consumers[consumer_at("helix_temp")];
    CHECK(temp.bytes == 300);
    CHECK(temp.files == 2);
    CHECK(temp.available);
    CHECK(temp.cleanable);

    // The app never offers to clean the user's own files, whatever a reply says.
    CHECK_FALSE(report->consumers[consumer_at("user_gcode")].cleanable);
    CHECK(report->consumers[consumer_at("user_gcode")].bytes == 5000);
    CHECK_FALSE(report->consumers[consumer_at("timelapse")].available);
    CHECK_FALSE(report->consumers[consumer_at("plr")].available);
}

TEST_CASE("A reply that is not a storage report is refused", "[plugin_storage]") {
    CHECK_FALSE(parse_storage_result(json::object()).has_value());
    CHECK_FALSE(parse_storage_result(json{{"consumers", "nope"}}).has_value());
    CHECK_FALSE(parse_storage_result(json::array()).has_value());
    const auto no_volume = parse_storage_result(json{{"consumers", json::array()}});
    REQUIRE(no_volume.has_value());
    CHECK_FALSE(no_volume->total.has_value());
}

// ============================================================================
// File-API fallback
// ============================================================================

TEST_CASE("The fallback sorts gcodes files into the plugin's consumers", "[plugin_storage]") {
    CHECK(classify_gcodes_path(".helix_temp/x.gcode", "") == consumer_at("helix_temp"));
    CHECK(classify_gcodes_path("helix_staging/x.gcode", "") == consumer_at("helix_temp"));
    CHECK(classify_gcodes_path(".helix_print/full/sub/x.gcode", "") == consumer_at("helix_temp"));
    CHECK(classify_gcodes_path("helix_links/full/x.gcode", "helix_links") ==
          consumer_at("helix_temp"));
    CHECK(classify_gcodes_path(".helix_print/full/x.gcode", "helix_links") ==
          consumer_at("user_gcode"));
    CHECK(classify_gcodes_path(".helix_index/sub/x.gcode.layers.jsonl", "") ==
          consumer_at("analysis_sidecars"));
    CHECK(classify_gcodes_path(".helix_photos/00002A/end.jpg", "") == consumer_at("photos"));
    CHECK(classify_gcodes_path(".thumbs/x-32x32.png", "") == consumer_at("thumbnails"));
    CHECK(classify_gcodes_path("deep/er/.thumbs/x-300x300.png", "") == consumer_at("thumbnails"));
    CHECK(classify_gcodes_path("deep/.thumbsnot/x.png", "") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path("sub/.helix_temp/x.gcode", "") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path(".helix_temp", "") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path("x.thumbs", "") == consumer_at("user_gcode"));
    CHECK(classify_gcodes_path("benchy.gcode", "") == consumer_at("user_gcode"));
}

TEST_CASE("A get_directory listing parses in both shapes", "[plugin_storage]") {
    const json object = {
        {"dirs", json::array({{{"dirname", "sub"}}, {{"dirname", ".thumbs"}}})},
        {"files",
         json::array({{{"filename", "a.gcode"}, {"size", 10}, {"modified", 5.0}}, {{"size", 3}}})},
        {"disk_usage", {{"total", 100}, {"used", 40}, {"free", 60}}},
    };
    const auto listing = parse_directory_result(object);
    REQUIRE(listing.has_value());
    REQUIRE(listing->files.size() == 1);
    CHECK(listing->files[0].name == "a.gcode");
    CHECK(listing->files[0].size == 10);
    CHECK(listing->files[0].modified == 5.0);
    CHECK(listing->dirs == std::vector<std::string>{"sub", ".thumbs"});
    CHECK(listing->free == 60u);
    CHECK(listing->total == 100u);

    const json flat = json::array({{{"path", "v.mp4"}, {"size", 7}, {"modified", 1.0}}});
    const auto flat_listing = parse_directory_result(flat);
    REQUIRE(flat_listing.has_value());
    REQUIRE(flat_listing->files.size() == 1);
    CHECK(flat_listing->files[0].name == "v.mp4");
    CHECK(flat_listing->dirs.empty());
    CHECK_FALSE(flat_listing->free.has_value());

    CHECK_FALSE(parse_directory_result(json{{"dirs", json::array()}}).has_value());
}

TEST_CASE("The fallback cleanup keeps what the plugin's cleanup keeps", "[plugin_storage]") {
    const double now = 100000.0;
    const ListedFile old_copy{".helix_temp/a.gcode", 5, 0.0};
    const ListedFile new_copy{".helix_temp/b.gcode", 5, now - kScratchMinAgeS + 1};
    CHECK(fallback_removes(consumer_at("helix_temp"), old_copy, now, 0.0, ""));
    CHECK_FALSE(fallback_removes(consumer_at("helix_temp"), new_copy, now, 0.0, ""));
    // A print running through a link holds some copy: none is removed.
    CHECK_FALSE(fallback_removes(consumer_at("helix_temp"), old_copy, now, 0.0,
                                 ".helix_print/full/benchy.gcode"));
    const ListedFile photo{".helix_photos/1/a.jpg", 5, 0.0};
    CHECK(fallback_removes(consumer_at("photos"), photo, now, 0.0, "benchy.gcode"));
    CHECK_FALSE(fallback_removes(consumer_at("photos"), photo, now, 0.0, ".helix_photos/1/a.jpg"));

    CHECK_FALSE(fallback_removes(consumer_at("logs"), {"klippy.log", 5, 0.0}, now, 0.0, ""));
    CHECK(fallback_removes(consumer_at("logs"), {"klippy.log.2026-09-01", 5, 0.0}, now, 0.0, ""));
    CHECK_FALSE(
        fallback_removes(consumer_at("logs"), {"klippy.log.1", 5, now - 10}, now, 60.0, ""));

    CHECK(fallback_removes(consumer_at("analysis_sidecars"), {"a.gcode.layers.jsonl", 1, 0.0}, now,
                           0.0, ""));
    CHECK_FALSE(fallback_removes(consumer_at("analysis_sidecars"),
                                 {"a.gcode.layers.jsonl.tmp", 1, 0.0}, now, 0.0, ""));

    for (const char* kept : {"user_gcode", "thumbnails", "3mf_outputs", "plr"}) {
        CAPTURE(kept);
        CHECK_FALSE(fallback_removes(consumer_at(kept), {"benchy.gcode", 1, 0.0}, now, 0.0, ""));
    }
    CHECK_FALSE(fallback_removes(kConsumerCount, old_copy, now, 0.0, ""));
}

TEST_CASE("An upload must leave the free-space floor", "[plugin_storage]") {
    CHECK(upload_space(kFreeSpaceFloorBytes + 10, 10) == UploadSpace::Ok);
    CHECK(upload_space(kFreeSpaceFloorBytes + 9, 10) == UploadSpace::TooFull);
    CHECK(upload_space(5, 10) == UploadSpace::TooFull);
    CHECK(upload_space(std::nullopt, 10) == UploadSpace::Unknown);
}

// ============================================================================
// The service
// ============================================================================

namespace {

/// The first call to @p method still waiting for its reply, removed.
test::FakeMoonrakerClient::RpcCall take_call(test::FakeMoonrakerClient& client,
                                             const std::string& method) {
    for (auto it = client.rpc_calls.begin(); it != client.rpc_calls.end(); ++it) {
        if (it->method == method) {
            auto call = *it;
            client.rpc_calls.erase(it);
            return call;
        }
    }
    FAIL("no " << method << " call");
    return {};
}

json listing(json files, json dirs = json::array()) {
    return json{{"result", {{"files", std::move(files)}, {"dirs", std::move(dirs)}}}};
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "The fallback lists the known roots and sums them",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    std::optional<StorageReport> got;
    std::optional<ReportSource> from;
    service.fetch(ReportSource::FileApi, "", [&](ReportSource s, std::optional<StorageReport> r) {
        from = s;
        got = r;
    });
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };

    // gcodes root: one file, two folders; the volume rides along.
    auto root = take_call(client, "server.files.get_directory");
    CHECK(root.params["path"] == "gcodes");
    json root_reply = listing(json::array({{{"filename", "benchy.gcode"}, {"size", 1000}}}),
                              json::array({{{"dirname", ".helix_temp"}}, {{"dirname", "sub"}}}));
    root_reply["result"]["disk_usage"] = {{"total", 9000}, {"free", 4000}};
    root.success_cb(root_reply);
    pump();

    auto timelapse = take_call(client, "server.files.get_directory");
    CHECK(timelapse.params["path"] == "timelapse");
    timelapse.error_cb(MoonrakerError::unknown("Not Found", "server.files.get_directory"));
    pump();

    auto logs = take_call(client, "server.files.get_directory");
    CHECK(logs.params["path"] == "logs");
    logs.success_cb(listing(json::array({{{"filename", "klippy.log"}, {"size", 50}},
                                         {{"filename", "klippy.log.1"}, {"size", 5}}})));
    pump();

    auto temp = take_call(client, "server.files.get_directory");
    CHECK(temp.params["path"] == "gcodes/.helix_temp");
    temp.success_cb(listing(json::array({{{"filename", "mod.gcode"}, {"size", 300}}})));
    pump();

    auto sub = take_call(client, "server.files.get_directory");
    CHECK(sub.params["path"] == "gcodes/sub");
    sub.success_cb(listing(json::array({{{"filename", "part.gcode"}, {"size", 20}}}),
                           json::array({{{"dirname", ".thumbs"}}})));
    pump();

    auto thumbs = take_call(client, "server.files.get_directory");
    CHECK(thumbs.params["path"] == "gcodes/sub/.thumbs");
    thumbs.success_cb(listing(json::array({{{"filename", "part-32x32.png"}, {"size", 2}}})));
    pump();

    REQUIRE(got.has_value());
    CHECK(from == ReportSource::FileApi);
    CHECK(got->free == 4000u);
    CHECK(got->total == 9000u);
    CHECK(got->consumers[consumer_at("user_gcode")].bytes == 1020);
    CHECK(got->consumers[consumer_at("user_gcode")].files == 2);
    CHECK(got->consumers[consumer_at("helix_temp")].bytes == 300);
    CHECK(got->consumers[consumer_at("helix_temp")].cleanable);
    CHECK(got->consumers[consumer_at("thumbnails")].bytes == 2);
    CHECK(got->consumers[consumer_at("logs")].bytes == 55);
    CHECK_FALSE(got->consumers[consumer_at("timelapse")].available);
    CHECK_FALSE(got->consumers[consumer_at("3mf_outputs")].available);
    CHECK_FALSE(got->consumers[consumer_at("plr")].available);
    CHECK_FALSE(got->partial);
}

TEST_CASE_METHOD(LVGLTestFixture, "The fallback cleanup deletes only old rotated logs",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };
    bool reported = false;
    service.fetch(ReportSource::FileApi, "",
                  [&](ReportSource, std::optional<StorageReport>) { reported = true; });
    take_call(client, "server.files.get_directory").success_cb(listing(json::array()));
    pump();
    take_call(client, "server.files.get_directory").success_cb(listing(json::array()));
    pump();
    take_call(client, "server.files.get_directory")
        .success_cb(listing(json::array({{{"filename", "klippy.log"}, {"size", 50}},
                                         {{"filename", "klippy.log.1"}, {"size", 5}}})));
    pump();
    REQUIRE(reported);

    std::optional<uint64_t> freed;
    service.cleanup(ReportSource::FileApi, consumer_at("logs"), "",
                    [&](std::optional<uint64_t> f, const std::string&) { freed = f; });
    auto del = take_call(client, "server.files.delete_file");
    CHECK(del.params["path"] == "logs/klippy.log.1");
    del.success_cb(json{{"result", json::object()}});
    pump();
    CHECK(client.rpc_calls.empty());
    REQUIRE(freed.has_value());
    CHECK(*freed == 5u);
}

TEST_CASE_METHOD(LVGLTestFixture, "The plugin path asks the plugin and cleans through it",
                 "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    StorageReportService service(&client);
    const auto pump = [] { helix::ui::UpdateQueue::instance().drain(); };

    service.cleanup(ReportSource::Plugin, consumer_at("user_gcode"), "",
                    [](std::optional<uint64_t> f, const std::string&) { CHECK_FALSE(f); });
    CHECK(client.rpc_calls.empty());

    std::optional<uint64_t> freed;
    service.cleanup(ReportSource::Plugin, consumer_at("timelapse"), "",
                    [&](std::optional<uint64_t> f, const std::string&) { freed = f; });
    auto call = take_call(client, "server.helix.storage_cleanup");
    CHECK(call.params == json{{"consumer", "timelapse"}, {"older_than", 0}});
    call.success_cb(json{{"result", {{"freed", 77}}}});
    pump();
    CHECK(freed == 77u);
}

TEST_CASE_METHOD(LVGLTestFixture, "An upload check reads the gcodes volume", "[plugin_storage]") {
    test::FakeMoonrakerClient client;
    AsyncLifetimeGuard guard;
    std::optional<UploadSpace> verdict;
    check_upload_space(&client, 50, guard.token(), [&](UploadSpace s) { verdict = s; });
    auto call = take_call(client, "server.files.get_directory");
    CHECK(call.params["path"] == "gcodes");
    json reply = listing(json::array());
    reply["result"]["disk_usage"] = {{"total", 10 * kFreeSpaceFloorBytes},
                                     {"free", kFreeSpaceFloorBytes + 49}};
    call.success_cb(reply);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(verdict == UploadSpace::TooFull);
}
