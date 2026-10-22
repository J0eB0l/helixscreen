# SPDX-License-Identifier: GPL-3.0-or-later
"""
Server-side G-code edits (helix_edits.py): the edit grammar against the golden
fixtures the app's C++ suite also reads, the parser's refusals, and
print_edited end to end through the linked-print path.

Run with: pytest tests/test_helix_edits.py -v
"""

import io
import json
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_edits
from helix_edits import (
    EditError,
    EditRefused,
    apply_file,
    apply_plan,
    parse_edits,
    remap_line,
    tool_index_for_line,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockWebRequest,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

# Shared with tests/unit/test_gcode_edit_list.cpp
GOLDEN_DIR = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "gcode_edits"
GOLDEN_CASES = sorted(p.name for p in GOLDEN_DIR.iterdir() if p.is_dir())

PRINT_EDITED = "/server/helix/print_edited"


def _apply_bytes(data: bytes, edits) -> bytes:
    out = io.BytesIO()
    apply_plan(io.BytesIO(data), out, parse_edits(edits))
    return out.getvalue()


def _edit(line, action, expect="", **args):
    return {"line_number": line, "expect_raw_line": expect, "action": action, "args": args}


# ============================================================================
# Golden fixtures
# ============================================================================

class TestGoldenFixtures:

    def test_the_fixture_set_is_there(self):
        # An empty glob would pass every parametrized case by running none.
        assert len(GOLDEN_CASES) >= 10
        assert "tool_remap" in GOLDEN_CASES and "expect_mismatch_refuses" in GOLDEN_CASES

    @pytest.mark.parametrize("name", GOLDEN_CASES)
    def test_case(self, name):
        case = GOLDEN_DIR / name
        spec = json.loads((case / "edits.json").read_text())
        data = (case / "input.gcode").read_bytes()
        if spec.get("refused"):
            with pytest.raises(EditRefused):
                _apply_bytes(data, spec["edits"])
            return
        assert _apply_bytes(data, spec["edits"]) == (case / "expected.gcode").read_bytes()

    @pytest.mark.parametrize("name", GOLDEN_CASES)
    def test_case_file_to_file(self, name, tmp_path):
        case = GOLDEN_DIR / name
        spec = json.loads((case / "edits.json").read_text())
        dst = tmp_path / "out.gcode"
        plan = parse_edits(spec["edits"])
        if spec.get("refused"):
            with pytest.raises(EditRefused):
                apply_file(str(case / "input.gcode"), str(dst), plan)
            assert not dst.exists()
            return
        apply_file(str(case / "input.gcode"), str(dst), plan)
        assert dst.read_bytes() == (case / "expected.gcode").read_bytes()


# ============================================================================
# Applying, beyond the fixtures
# ============================================================================

class TestApply:

    def test_a_later_edit_for_the_same_line_wins(self):
        out = _apply_bytes(b"A\nB\n", [
            _edit(2, "comment_out"), _edit(2, "replace", gcode="C"),
        ])
        assert out == b"A\nC\n"

    def test_the_copy_past_the_last_edit_is_verbatim(self):
        tail = b"".join(b"G1 X%d\n" % i for i in range(5000))
        out = _apply_bytes(b"PRINT_START\n" + tail, [_edit(1, "delete", "PRINT_START")])
        assert out == tail

    def test_deleting_every_line_leaves_an_empty_file(self):
        assert _apply_bytes(b"A\nB\n", [_edit(1, "delete", end_line=2)]) == b""

    def test_an_empty_file_stays_empty(self):
        assert _apply_bytes(b"", [_edit(1, "comment_out")]) == b""

    def test_an_empty_list_copies_the_file(self):
        assert _apply_bytes(b"A\r\nB", []) == b"A\r\nB"

    def test_a_refusal_comes_from_the_first_stale_line(self):
        with pytest.raises(EditRefused) as e:
            _apply_bytes(b"A\nB\nC\n", [_edit(3, "delete", "X"), _edit(2, "delete", "Y")])
        assert e.value.line_number == 2

    def test_non_ascii_text_round_trips_as_utf8(self):
        out = _apply_bytes("A\nM117 Düse\n".encode(), [
            _edit(2, "comment_out", "M117 Düse", comment="Aus"),
        ])
        assert out == "A\n; M117 Düse  ; [HelixScreen: Aus]\n".encode()


class TestToolRemapRules:

    @pytest.mark.parametrize("line,expected", [
        (b"T12", (12, 1, 3)),
        (b"  T3 ; comment", (3, 3, 4)),
        (b"T0 PURGE=1 X_Y=2", (0, 1, 2)),
        (b"T0 X1", None),
        (b"T0X1", None),
        (b"TURN", None),
        (b"T", None),
        (b"T100001", None),
        (b"T0 =1", None),
    ])
    def test_tool_index_for_line(self, line, expected):
        assert tool_index_for_line(line) == expected

    @pytest.mark.parametrize("line,expected", [
        (b"SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=200", b"SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=0 TEMP=200"),
        (b"SM_PRINT_FLOW_CALIBRATE EXTRUDER=x", b"SM_PRINT_FLOW_CALIBRATE EXTRUDER=x"),
        (b"M109 S200 T1", b"M109 S200 T0"),
        (b"M104 S200 T2 T0", b"M104 S200 T2 T0"),
        (b"M104 S200 ;T0", b"M104 S200 ;T0"),
        (b"START tool=1 initial_tool=0", b"START tool=0 initial_tool=1"),
        (b"START TOOL=T0", b"START TOOL=T0"),
        (b"START\tTOOL=0", b"START\tTOOL=1"),
        (b"; START TOOL=0", b"; START TOOL=0"),
        (b"START ; TOOL=0", b"START ; TOOL=0"),
        (b"TOOL=0", b"TOOL=0"),
    ])
    def test_remap_line(self, line, expected):
        assert remap_line(line, {0: 1, 1: 0}) == expected


# ============================================================================
# Parsing
# ============================================================================

class TestParse:

    @pytest.mark.parametrize("edits", [
        "not a list",
        ["not an object"],
        [_edit(1, "explode")],
        [{"line_number": 1, "action": "replace", "args": []}],
        [_edit(1, "replace")],
        [_edit(1, "inject_after", gcode=7)],
        [_edit(1, "delete", gcode="X")],
        [_edit(0, "delete")],
        [_edit(True, "delete")],
        [_edit("1", "delete")],
        [_edit(3, "delete", end_line=2)],
        [{"line_number": 1, "expect_raw_line": None, "action": "delete", "args": {}}],
        [_edit(0, "tool_remap", map={})],
        [_edit(0, "tool_remap", map={"T0": 1})],
        [_edit(0, "tool_remap", map={"0": -1})],
        [_edit(0, "tool_remap", map={"0": "1"})],
        [_edit(1, "tool_remap", map={"0": 1})],
        [_edit(False, "tool_remap", map={"0": 1})],
        [_edit(0, "tool_remap", "T0", map={"0": 1})],
        [_edit(0, "tool_remap", map={"0": 1}), _edit(1, "delete")],
    ])
    def test_malformed_lists_are_refused(self, edits):
        with pytest.raises(EditError):
            parse_edits(edits)

    def test_a_range_fills_one_entry_per_line(self):
        plan = parse_edits([_edit(2, "comment_out", "X", end_line=4)])
        assert sorted(plan.by_line) == [2, 3, 4]
        assert all(edit.expect == b"X" for edit in plan.by_line.values())

    def test_a_single_line_edit_takes_its_own_line(self):
        plan = parse_edits([_edit(2, "comment_out"), _edit(5, "replace", gcode="Y")])
        assert sorted(plan.by_line) == [2, 5]


# ============================================================================
# print_edited
# ============================================================================

def _request(**params):
    base = {
        "original_filename": "benchy.gcode",
        "temp_file_path": ".helix_temp/modified_1_benchy.gcode",
        "edits": [_edit(2, "comment_out", "BED_MESH_CALIBRATE", comment="Disabled Bed mesh")],
        "modifications": ["bed_mesh_disabled"],
    }
    base.update(params)
    return MockWebRequest(base)


@pytest.fixture
def gcodes(temp_gcodes_dir):
    root = Path(temp_gcodes_dir)
    (root / "benchy.gcode").write_bytes(b"G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n")
    return root


class TestPrintEdited:

    @pytest.mark.asyncio
    async def test_status_reports_the_feature_on_by_default(self, helix_print_component,
                                                            mock_server):
        await helix_print_component.component_init()
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))
        assert status["features"]["server_edits"] == {
            "version": 1, "available": True, "enabled": True, "reason": None,
        }

    @pytest.mark.asyncio
    async def test_edits_and_starts_the_copy_under_the_original_name(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request())

        copy = gcodes / ".helix_temp" / "modified_1_benchy.gcode"
        assert copy.read_bytes() == (
            b"G28\n; BED_MESH_CALIBRATE  ; [HelixScreen: Disabled Bed mesh]\nG1 X0 Y0\n"
        )
        assert not (gcodes / ".helix_temp" / "modified_1_benchy.gcode.part").exists()
        assert result["original_filename"] == "benchy.gcode"
        assert result["print_filename"] == ".helix_print/full/benchy.gcode"
        assert result["lines_changed"] == 1
        assert (gcodes / result["print_filename"]).resolve() == copy.resolve()
        helix_print_component.klippy_apis.run_gcode.assert_awaited_once_with(
            'SDCARD_PRINT_FILE FILENAME=".helix_print/full/benchy.gcode"'
        )
        info = helix_print_component.active_prints[result["print_filename"]]
        assert info.modifications == ["bed_mesh_disabled"]

    @pytest.mark.asyncio
    async def test_the_job_is_marked_print_edited(self, helix_print_component,
                                                  mock_server, gcodes):
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request())
        await helix_print_component._on_history_changed({
            "action": "added",
            "job": {"filename": result["print_filename"], "job_id": "00003A",
                    "start_time": 5.0},
        })
        assert helix_print_component.helix_initiated(
            "00003A", result["print_filename"], 5.0
        ) == "print_edited"

    @pytest.mark.asyncio
    async def test_a_failed_start_withdraws_the_marker_and_the_copy(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        helix_print_component.klippy_apis.run_gcode.side_effect = RuntimeError("klippy down")
        with pytest.raises(Exception, match="Failed to start print"):
            await mock_server.endpoints[PRINT_EDITED](_request())
        assert helix_print_component.job_markers.pending_count() == 0
        assert not (gcodes / ".helix_temp" / "modified_1_benchy.gcode").exists()

    @pytest.mark.asyncio
    async def test_a_stale_line_refuses_without_writing_or_starting(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        request = _request(edits=[
            _edit(1, "comment_out", "G28"), _edit(2, "comment_out", "QUAD_GANTRY_LEVEL"),
        ])
        with pytest.raises(Exception, match="409.*line 2"):
            await mock_server.endpoints[PRINT_EDITED](request)
        assert list((gcodes / ".helix_temp").iterdir()) == []
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()
        assert helix_print_component.job_markers.pending_count() == 0

    @pytest.mark.asyncio
    async def test_a_malformed_list_is_a_400(self, helix_print_component, mock_server, gcodes):
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="400.*Invalid edit list"):
            await mock_server.endpoints[PRINT_EDITED](_request(edits=[_edit(1, "explode")]))
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_a_plugin_features_opt_out_is_ignored(self, helix_print_component,
                                                         mock_server, gcodes):
        await helix_print_component.component_init()
        await helix_print_component.database.insert_item(
            HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, {"server_edits": False}
        )
        await mock_server.endpoints[PRINT_EDITED](_request())
        assert (gcodes / ".helix_temp" / "modified_1_benchy.gcode").exists()

    @pytest.mark.asyncio
    async def test_paths_outside_gcodes_are_refused(self, helix_print_component,
                                                    mock_server, gcodes):
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="400"):
            await mock_server.endpoints[PRINT_EDITED](_request(temp_file_path="../x.gcode"))
        with pytest.raises(Exception, match="not found"):
            await mock_server.endpoints[PRINT_EDITED](_request(original_filename="nope.gcode"))
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_without_a_path_the_copy_goes_to_the_temp_dir(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request(temp_file_path=""))
        assert result["temp_filename"].startswith(".helix_temp/modified_")
        assert result["temp_filename"].endswith("_benchy.gcode")
        assert (gcodes / result["temp_filename"]).is_file()

    @pytest.mark.asyncio
    async def test_a_missing_module_reports_the_feature_unavailable(
        self, helix_print_component, mock_server, monkeypatch
    ):
        import helix_print
        monkeypatch.setattr(helix_print, "helix_edits", None)
        monkeypatch.setitem(helix_print.SIBLING_LOAD_ERRORS, "helix_edits", "module_missing")
        await helix_print_component.component_init()
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))
        assert status["features"]["server_edits"]["available"] is False
        assert status["features"]["server_edits"]["reason"] == "module_missing"
        assert PRINT_EDITED not in mock_server.endpoints
