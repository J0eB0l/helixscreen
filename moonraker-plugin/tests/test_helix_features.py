# SPDX-License-Identifier: GPL-3.0-or-later
"""
The plugin's feature framework: the status `features` map with its opt-in and
own-switch rules, and the helix-initiated job marker.

Run with: pytest tests/test_helix_features.py -v
"""

import time
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_print
from helix_print import (
    DB_RECORD_MAX_AGE,
    HELIX_JOB_KINDS,
    HELIX_JOBS_NAMESPACE,
    HELIX_MARKER_TTL_S,
    HELIX_SCREEN_NAMESPACE,
    PLUGIN_FEATURES_KEY,
    SIBLING_LOAD_ERRORS,
    HelixJobMarkers,
    load_component,
)
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockConfigHelper,
    MockFileManager,
    MockKlippy,
    MockKlippyApis,
    MockHistory,
    MockWebRequest,
    Moonraker093Database,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)


async def _status(component, mock_server):
    return await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))


async def _opt_in(component, record):
    await component.database.insert_item(
        HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, record
    )


class FakeClock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


# ============================================================================
# Features map
# ============================================================================

class TestFeaturesMap:

    @pytest.mark.asyncio
    async def test_status_carries_the_api_level_and_map(self, helix_print_component,
                                                        mock_server):
        await helix_print_component.component_init()
        helix_print_component.register_feature("file_analysis", 3, acts=False)

        result = await _status(helix_print_component, mock_server)

        assert result["api"] == 2
        assert result["features"]["file_analysis"] == {
            "version": 3, "available": True, "enabled": True, "reason": None,
        }

    @pytest.mark.asyncio
    async def test_defaults_observers_on_actors_off(self, helix_print_component,
                                                    mock_server):
        await helix_print_component.component_init()
        helix_print_component.register_feature("journal", 1, acts=False)
        helix_print_component.register_feature("print_hooks", 1, acts=True)

        features = (await _status(helix_print_component, mock_server))["features"]

        assert features["journal"]["enabled"] is True
        assert features["print_hooks"]["enabled"] is False
        assert await helix_print_component.feature_enabled("journal") is True
        assert await helix_print_component.feature_enabled("print_hooks") is False

    @pytest.mark.asyncio
    async def test_opt_in_record_overrides_the_default(self, helix_print_component,
                                                       mock_server):
        await helix_print_component.component_init()
        helix_print_component.register_feature("journal", 1, acts=False)
        helix_print_component.register_feature("print_hooks", 1, acts=True)
        await _opt_in(helix_print_component, {"journal": False, "print_hooks": True})

        features = (await _status(helix_print_component, mock_server))["features"]

        assert features["journal"]["enabled"] is False
        assert features["print_hooks"]["enabled"] is True
        assert await helix_print_component.feature_enabled("print_hooks") is True

    @pytest.mark.asyncio
    async def test_a_non_bool_opt_in_reads_as_the_default(self, helix_print_component):
        await helix_print_component.component_init()
        helix_print_component.register_feature("print_hooks", 1, acts=True)
        await _opt_in(helix_print_component, {"print_hooks": "yes"})

        assert await helix_print_component.feature_enabled("print_hooks") is False

    @pytest.mark.asyncio
    async def test_an_owned_switch_ignores_plugin_features(self, helix_print_component,
                                                           mock_server):
        await helix_print_component.component_init()
        switch_value = {"on": False}

        async def switch():
            return switch_value["on"]

        helix_print_component.register_feature("plr", 1, acts=True, switch=switch)
        await _opt_in(helix_print_component, {"plr": True})

        features = (await _status(helix_print_component, mock_server))["features"]
        assert features["plr"]["enabled"] is False
        assert await helix_print_component.feature_enabled("plr") is False

        switch_value["on"] = True
        await _opt_in(helix_print_component, {"plr": False})

        features = (await _status(helix_print_component, mock_server))["features"]
        assert features["plr"]["enabled"] is True
        assert await helix_print_component.feature_enabled("plr") is True

    @pytest.mark.asyncio
    async def test_a_switch_that_raises_reads_off(self, helix_print_component):
        await helix_print_component.component_init()
        async def switch():
            raise RuntimeError("store unreadable")

        helix_print_component.register_feature("plr", 1, acts=True, switch=switch)

        assert await helix_print_component.feature_enabled("plr") is False

    @pytest.mark.asyncio
    async def test_an_unknown_feature_is_off_and_not_reported(
        self, helix_print_component, mock_server
    ):
        await helix_print_component.component_init()
        await _opt_in(helix_print_component, {"no_such_feature": True})

        features = (await _status(helix_print_component, mock_server))["features"]

        assert "no_such_feature" not in features
        assert await helix_print_component.feature_enabled("no_such_feature") is False

    @pytest.mark.asyncio
    async def test_an_unavailable_feature_is_never_enabled(self, helix_print_component,
                                                           mock_server):
        await helix_print_component.component_init()
        helix_print_component.register_feature(
            "event_journal", 1, acts=False, available=False, reason="moonraker<0.9"
        )
        await _opt_in(helix_print_component, {"event_journal": True})

        entry = (await _status(helix_print_component, mock_server))["features"][
            "event_journal"
        ]

        assert entry == {
            "version": 1, "available": False, "enabled": False, "reason": "moonraker<0.9",
        }
        assert await helix_print_component.feature_enabled("event_journal") is False

    @pytest.mark.asyncio
    async def test_an_unreadable_settings_store_reads_as_defaults(
        self, helix_print_component, mock_server
    ):
        await helix_print_component.component_init()
        async def broken_get_item(*args, **kwargs):
            raise RuntimeError("database locked")

        helix_print_component.register_feature("journal", 1, acts=False)
        helix_print_component.register_feature("print_hooks", 1, acts=True)
        helix_print_component.database.get_item = broken_get_item

        features = (await _status(helix_print_component, mock_server))["features"]

        assert features["journal"]["enabled"] is True
        assert features["print_hooks"]["enabled"] is False


class TestModuleFeatures:
    """A sibling module that fails disables only the feature built on it."""

    def test_a_sibling_that_raises_on_import_records_why(self, tmp_path, monkeypatch):
        (tmp_path / "helix_broken.py").write_text("raise ImportError('needs 3.12')\n")
        (tmp_path / "helix_fine.py").write_text("VALUE = 7\n")
        monkeypatch.setattr(helix_print, "__file__", str(tmp_path / "helix_print.py"))

        broken = helix_print._load_sibling("helix_broken")
        fine = helix_print._load_sibling("helix_fine")
        missing = helix_print._load_sibling("helix_absent")

        assert broken is None
        assert "needs 3.12" in SIBLING_LOAD_ERRORS["helix_broken"]
        assert fine is not None and fine.VALUE == 7
        assert "helix_fine" not in SIBLING_LOAD_ERRORS
        assert missing is None
        assert SIBLING_LOAD_ERRORS["helix_absent"] == "module_missing"

    @pytest.mark.asyncio
    async def test_a_failed_converter_module_reports_unavailable(
        self, helix_print_component, mock_server, monkeypatch
    ):
        monkeypatch.setattr(helix_print, "helix_3mf", None)
        monkeypatch.setitem(SIBLING_LOAD_ERRORS, "helix_3mf", "load_failed: SyntaxError")
        helix_print_component.register_feature("journal", 1, acts=False)
        await helix_print_component.component_init()

        features = (await _status(helix_print_component, mock_server))["features"]

        assert features["convert_3mf"]["available"] is False
        assert features["convert_3mf"]["enabled"] is False
        assert features["convert_3mf"]["reason"] == "load_failed: SyntaxError"
        assert features["journal"]["enabled"] is True

    @pytest.mark.asyncio
    async def test_conversion_switched_off_in_config_is_available_but_off(
        self, mock_server, temp_gcodes_dir
    ):
        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = Moonraker093Database()
        mock_server.components["klippy_apis"] = MockKlippyApis()
        mock_server.components["history"] = MockHistory()
        component = load_component(MockConfigHelper(mock_server, {
            "cleanup_delay": 3600, "enabled": True, "convert_3mf": False,
        }))
        await component.component_init()

        entry = (await _status(component, mock_server))["features"]["convert_3mf"]

        assert entry["available"] is True
        assert entry["enabled"] is False
        assert entry["reason"] is None


# ============================================================================
# Helix-initiated job marker
# ============================================================================

class TestJobMarkers:

    @pytest.mark.parametrize("kind", HELIX_JOB_KINDS)
    def test_a_marker_binds_to_the_job_for_its_file(self, kind):
        markers = HelixJobMarkers(FakeClock())
        markers.mark("benchy.gcode", kind)

        assert markers.bind("benchy.gcode", "00001A") == kind
        assert markers.kind_of("00001A") == kind
        assert markers.pending_count() == 0

    def test_the_three_kinds_are_the_spec_set(self):
        assert set(HELIX_JOB_KINDS) == {"plr_resume", "print_modified", "print_edited"}

    def test_an_unknown_kind_is_refused(self):
        markers = HelixJobMarkers(FakeClock())
        with pytest.raises(ValueError):
            markers.mark("benchy.gcode", "print_started")

    def test_a_foreign_job_gets_no_kind(self):
        markers = HelixJobMarkers(FakeClock())
        markers.mark("benchy.gcode", "plr_resume")

        assert markers.bind("somebody_else.gcode", "00001B") is None
        assert markers.kind_of("00001B") is None
        # The marker still waits for its own file
        assert markers.bind("benchy.gcode", "00001C") == "plr_resume"

    def test_an_unmatched_marker_expires(self):
        clock = FakeClock()
        markers = HelixJobMarkers(clock)
        markers.mark("benchy.gcode", "print_modified")

        clock.now += HELIX_MARKER_TTL_S + 0.1

        assert markers.bind("benchy.gcode", "00001A") is None
        assert markers.kind_of("00001A") is None

    def test_a_marker_is_live_up_to_its_ttl(self):
        clock = FakeClock()
        markers = HelixJobMarkers(clock)
        markers.mark("benchy.gcode", "print_modified")

        clock.now += HELIX_MARKER_TTL_S

        assert markers.bind("benchy.gcode", "00001A") == "print_modified"

    def test_a_marker_binds_once(self):
        markers = HelixJobMarkers(FakeClock())
        markers.mark("benchy.gcode", "print_modified")
        markers.bind("benchy.gcode", "00001A")

        assert markers.bind("benchy.gcode", "00001B") is None

    def test_the_oldest_marker_for_a_file_binds_first(self):
        clock = FakeClock()
        markers = HelixJobMarkers(clock)
        markers.mark("benchy.gcode", "print_modified")
        clock.now += 1
        markers.mark("benchy.gcode", "plr_resume")

        assert markers.bind("benchy.gcode", "00001A") == "print_modified"
        assert markers.bind("benchy.gcode", "00001B") == "plr_resume"

    def test_an_int_job_id_names_the_same_job(self):
        markers = HelixJobMarkers(FakeClock())
        markers.mark("benchy.gcode", "plr_resume")
        markers.bind("benchy.gcode", 0x1A)

        assert markers.kind_of("00001A") == "plr_resume"


def _stage_modified(gcodes: Path) -> MockWebRequest:
    (gcodes / "benchy.gcode").write_text("G28\n")
    (gcodes / ".helix_temp").mkdir(exist_ok=True)
    (gcodes / ".helix_temp" / "modified_1_benchy.gcode").write_text("G28\nG1\n")
    return MockWebRequest({
        "original_filename": "benchy.gcode",
        "temp_file_path": ".helix_temp/modified_1_benchy.gcode",
        "modifications": [],
    })


def _added(filename, job_id):
    return {"action": "added", "job": {"filename": filename, "job_id": job_id}}


class TestJobMarkerWiring:

    @pytest.mark.asyncio
    async def test_print_modified_marks_its_job(self, helix_print_component,
                                                mock_server, temp_gcodes_dir):
        await helix_print_component.component_init()
        result = await mock_server.endpoints["/server/helix/print_modified"](
            _stage_modified(Path(temp_gcodes_dir))
        )

        await helix_print_component._on_history_changed(
            _added(result["print_filename"], "00002A")
        )

        assert helix_print_component.helix_initiated("00002A") == "print_modified"
        stored = helix_print_component.database.namespaces[HELIX_JOBS_NAMESPACE]
        assert stored["00002A"]["kind"] == "print_modified"

    @pytest.mark.asyncio
    async def test_a_job_another_client_started_is_not_helix_initiated(
        self, helix_print_component
    ):
        await helix_print_component.component_init()
        helix_print_component.mark_helix_initiated("benchy.gcode", "plr_resume")

        await helix_print_component._on_history_changed(_added("other.gcode", "00002B"))

        assert helix_print_component.helix_initiated("00002B") is None
        assert HELIX_JOBS_NAMESPACE not in helix_print_component.database.namespaces

    @pytest.mark.asyncio
    async def test_a_binding_survives_a_restart_and_old_ones_are_dropped(
        self, mock_server, temp_gcodes_dir
    ):
        db = Moonraker093Database()
        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = db
        mock_server.components["klippy_apis"] = MockKlippyApis()
        mock_server.components["history"] = MockHistory()
        config = MockConfigHelper(mock_server, {"cleanup_delay": 3600, "enabled": True})

        first = load_component(config)
        await first.component_init()
        first.mark_helix_initiated("benchy.gcode", "plr_resume")
        await first._on_history_changed(_added("benchy.gcode", "00003A"))
        await db.insert_item(
            HELIX_JOBS_NAMESPACE, ["000001"],
            {"kind": "print_modified", "bound_at": time.time() - DB_RECORD_MAX_AGE - 10},
        )

        second = load_component(config)
        await second.component_init()

        assert second.helix_initiated("00003A") == "plr_resume"
        assert second.helix_initiated("000001") is None
        assert "000001" not in db.ns[HELIX_JOBS_NAMESPACE]
