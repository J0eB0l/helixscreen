# SPDX-License-Identifier: GPL-3.0-or-later
"""
File-fits-printer check (helix_fitcheck.py): the checker and the printer limits
pinned by the table the screen's checker reads too, and the opt-in hold on a
slicer's upload-and-print.

Run with: pytest tests/test_helix_fitcheck.py -v
"""

import json
import shutil
import sys
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_fitcheck
from helix_fitcheck import (
    FIT_CHECK_FEATURE,
    REFUSED_EVENT,
    check_fit,
    fit_file_from_record,
    machine_limits,
    may_autostart,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, load_component
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockConfigHelper,
    MockDatabase,
    MockFileManager,
    MockHistory,
    MockKlippy,
    MockKlippyApis,
    mock_server,
    temp_gcodes_dir,
)

# Shared with tests/unit/test_fit_check.cpp
FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "fit_check" / "cases.json"
CASES = json.loads(FIXTURE.read_text())

SETTINGS = {
    "printer": {"kinematics": "corexy"},
    "stepper_x": {"position_min": 0.0, "position_max": 220.0},
    "stepper_y": {"position_min": 0.0, "position_max": 220.0},
    "stepper_z": {"position_min": 0.0, "position_max": 250.0},
    "extruder": {"max_temp": 300.0, "nozzle_diameter": 0.4},
    "heater_bed": {"max_temp": 110.0},
}


def _record(x_max=200.0, hotend=240.0, tools=(0,)):
    return {
        "schema": 1,
        "tools": [{"tool": t} for t in tools],
        "bounds": {
            "print": {"min": [10.0, 10.0, 0.2], "max": [x_max, 200.0, 20.0]},
            "travel": {"min": [0.0, 0.0, 0.2], "max": [x_max, 200.0, 30.0]},
            "max_hotend_temp": hotend, "max_bed_temp": 60.0,
        },
        "header": {"printer_model": "Voron 0.2", "nozzle_diameters": [0.4],
                   "bed": None, "filament_types": ["PLA"]},
    }


# ============================================================================
# The shared table
# ============================================================================


@pytest.mark.parametrize("case", CASES["machines"], ids=[c["name"] for c in CASES["machines"]])
def test_machine_limits_match_the_table(case):
    assert machine_limits(case["settings"]) == case["machine"]


@pytest.mark.parametrize("case", CASES["checks"], ids=[c["name"] for c in CASES["checks"]])
def test_findings_match_the_table(case):
    assert check_fit(case["machine"], case["file"]) == case["findings"]


def test_the_table_covers_every_kind_and_both_kinematics_families():
    kinds = {f["kind"] for c in CASES["checks"] for f in c["findings"]}
    assert kinds == {"out_of_bounds", "temp_over_max", "too_many_tools", "nozzle_mismatch",
                     "printer_model_mismatch"}
    kinematics = {c["machine"]["kinematics"] for c in CASES["machines"]}
    assert {"delta", "corexy", "cartesian"} <= kinematics


def test_limits_of_something_that_is_not_settings_are_unknown():
    limits = machine_limits(None)
    assert limits["axis_max"] == [None, None, None]
    assert limits["tool_count"] is None


# ============================================================================
# The record, and the decision
# ============================================================================


def test_file_facts_come_from_the_records_bounds_header_and_tools():
    facts = fit_file_from_record(_record(tools=(1, 0)))
    assert facts["travel"]["max"][0] == 200.0
    assert facts["print"]["min"][2] == 0.2
    assert facts["max_hotend_temp"] == 240.0
    assert facts["max_bed_temp"] == 60.0
    assert facts["tools"] == [0, 1]
    assert facts["nozzle_diameters"] == [0.4]
    assert facts["printer_model"] == "Voron 0.2"


def test_a_real_analysis_record_feeds_the_checker(tmp_path):
    """The record helix_analysis writes is the shape fit_file_from_record reads."""
    from helix_analysis import analyze_file
    fixtures = Path(__file__).parent / "fixtures" / "analysis"
    gc = tmp_path / "gcodes"
    gc.mkdir()
    shutil.copy(str(fixtures / "no_define.gcode"), str(gc / "no_define.gcode"))
    record = analyze_file(gc, "no_define.gcode", {}).record
    facts = fit_file_from_record(record)
    travel_max_x = record["bounds"]["travel"]["max"][0]
    assert facts["travel"]["max"][0] == travel_max_x
    limits = machine_limits(dict(SETTINGS, stepper_x={"position_min": 0.0,
                                                      "position_max": travel_max_x - 1.0}))
    found = check_fit(limits, facts)
    assert [(f["kind"], f["detail"]["axis"]) for f in found] == [("out_of_bounds", "x")]
    limits["axis_max"][0] = travel_max_x
    assert check_fit(limits, facts) == []


def test_file_facts_of_a_record_without_bounds_check_nothing():
    facts = fit_file_from_record({"schema": 1})
    assert check_fit(machine_limits(SETTINGS), facts) == []


BLOCK = [{"kind": "out_of_bounds", "severity": "block", "detail": {}}]
WARN = [{"kind": "nozzle_mismatch", "severity": "warn", "detail": {}}]


@pytest.mark.parametrize("enabled,findings,starts", [
    (False, BLOCK, True),   # switched off: the slicer's start stands
    (True, None, True),     # could not be checked: started unchecked
    (True, [], True),
    (True, WARN, True),     # a warning never holds a start
    (True, BLOCK, False),
    (True, WARN + BLOCK, False),
])
def test_may_autostart(enabled, findings, starts):
    assert may_autostart(enabled, findings) is starts


# ============================================================================
# The upload hook
# ============================================================================


class UploadFileManager(MockFileManager):
    """finalize_upload in Moonraker's shape: stores the file, and starts it
    when the form asks for a print."""

    def __init__(self, gcodes_path):
        super().__init__(gcodes_path)
        self.calls = []

    async def finalize_upload(self, form_args):
        self.calls.append(dict(form_args))
        path = form_args["filename"]
        return {"item": {"path": path, "root": "gcodes"}, "action": "create_file",
                "print_started": form_args.get("print") == "true", "print_queued": False}


@pytest.fixture
def component(mock_server, temp_gcodes_dir):
    mock_server.components["file_manager"] = UploadFileManager(temp_gcodes_dir)
    mock_server.components["database"] = MockDatabase()
    mock_server.components["klippy_connection"] = MockKlippy()
    mock_server.components["klippy_apis"] = MockKlippyApis()
    mock_server.components["history"] = MockHistory()
    mock_server.components["klippy_apis"].query_objects.return_value = {
        "configfile": {"settings": SETTINGS}}
    mock_server.send_event = MagicMock()
    return load_component(MockConfigHelper(mock_server, {
        "temp_dir": ".helix_temp", "symlink_dir": ".helix_print",
        "cleanup_delay": 3600, "enabled": True,
    }))


async def _setup(component, opt_in=True, record=None):
    await component.component_init()
    if opt_in is not None:
        await component.database.insert_item(HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY,
                                             {FIT_CHECK_FEATURE: opt_in})
    component.analysis.record_for = AsyncMock(return_value=record)


async def _upload(component, filename="part.gcode", start=True):
    form = {"root": "gcodes", "filename": filename, "tmp_file_path": "/tmp/x",
            "print": "true" if start else "false", "current_user": {"username": "orca"}}
    return await component.file_manager.finalize_upload(form)


class TestUploadHold:

    @pytest.mark.asyncio
    async def test_the_feature_is_off_until_opted_into(self, component):
        await _setup(component, opt_in=None)
        assert await component.feature_enabled(FIT_CHECK_FEATURE) is False
        status = await component._features_status()
        assert status[FIT_CHECK_FEATURE]["available"] is True
        assert status[FIT_CHECK_FEATURE]["enabled"] is False

    @pytest.mark.asyncio
    async def test_switched_off_the_upload_starts_as_moonraker_would(self, component):
        await _setup(component, opt_in=False, record=_record(x_max=400.0))
        result = await _upload(component)
        fm = component.file_manager
        assert fm.calls[-1]["print"] == "true"
        assert result["print_started"] is True
        component.analysis.record_for.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_a_blocking_finding_keeps_the_file_and_starts_nothing(self, component,
                                                                        mock_server):
        await _setup(component, record=_record(x_max=221.0))
        result = await _upload(component)
        stored = component.file_manager.calls[-1]
        assert "print" not in stored
        assert stored["filename"] == "part.gcode"
        assert result["print_started"] is False and result["print_queued"] is False
        component.klippy_apis.start_print.assert_not_awaited()
        event, payload = mock_server.send_event.call_args.args
        assert event == REFUSED_EVENT
        assert payload["filename"] == "part.gcode"
        assert [f["kind"] for f in payload["findings"]] == ["out_of_bounds"]

    @pytest.mark.asyncio
    async def test_a_file_that_fits_is_started(self, component):
        await _setup(component, record=_record(x_max=219.0))
        result = await _upload(component)
        assert "print" not in component.file_manager.calls[-1]
        component.klippy_apis.start_print.assert_awaited_once_with(
            "part.gcode", user={"username": "orca"})
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_a_hotend_over_max_temp_is_held(self, component):
        await _setup(component, record=_record(hotend=301.0))
        result = await _upload(component)
        assert result["print_started"] is False
        component.klippy_apis.start_print.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_extra_tools_do_not_hold_a_start(self, component):
        """The plugin cannot see a filament system's T commands."""
        await _setup(component, record=_record(tools=(0, 1, 2, 3)))
        result = await _upload(component)
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_a_file_without_an_analysis_starts_unchecked(self, component):
        await _setup(component, record=None)
        result = await _upload(component)
        assert result["print_started"] is True
        component.analysis.record_for.assert_awaited_once_with(
            "part.gcode", helix_fitcheck.RECORD_WAIT_S)

    @pytest.mark.asyncio
    async def test_unreadable_printer_limits_start_unchecked(self, component):
        await _setup(component, record=_record(x_max=400.0))
        component.klippy_apis.query_objects.side_effect = RuntimeError("klippy down")
        result = await _upload(component)
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_an_upload_without_a_print_passes_through(self, component):
        await _setup(component, record=_record(x_max=400.0))
        await _upload(component, start=False)
        await _upload(component, filename="part.3mf")
        assert component.file_manager.calls[0]["print"] == "false"
        assert component.file_manager.calls[1]["print"] == "true"
        component.analysis.record_for.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_close_puts_moonrakers_upload_back(self, component, mock_server):
        original = mock_server.components["file_manager"].finalize_upload
        await _setup(component)
        assert component.file_manager.finalize_upload != original
        await component.close()
        assert component.file_manager.finalize_upload == original

    @pytest.mark.asyncio
    async def test_the_converter_asks_before_starting(self, component):
        converter = MagicMock(active=True, start_gate=None)
        converter.close = AsyncMock()
        component.converter = converter
        component._start_3mf_converter = AsyncMock()
        await _setup(component, record=_record(x_max=400.0))
        assert converter.start_gate == component.fit_check.may_start
        assert await converter.start_gate("plate_1.gcode") is False
        component.analysis.record_for.return_value = _record()
        assert await converter.start_gate("plate_1.gcode") is True
        await component.close()
        assert converter.start_gate is None

    @pytest.mark.asyncio
    async def test_unavailable_without_file_analysis(self, component, monkeypatch):
        import helix_print
        monkeypatch.setattr(helix_print, "helix_analysis", None)
        await component.component_init()
        status = await component._features_status()
        assert status[FIT_CHECK_FEATURE]["available"] is False
        assert status[FIT_CHECK_FEATURE]["reason"] == "needs_file_analysis"
