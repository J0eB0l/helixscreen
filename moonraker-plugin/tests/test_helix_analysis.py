# SPDX-License-Identifier: GPL-3.0-or-later
"""
File analysis (helix_analysis.py): the record each slicer's output yields,
the per-field source order, the layer index it writes for power-loss
recovery and the screen, invalidation, and the cost guard.

Fixtures in tests/fixtures/analysis are small files in each slicer's own
header and footer shapes: OrcaSlicer (two tools, EXCLUDE_OBJECT_DEFINE, an
embedded thumbnail, M600), PrusaSlicer (`; printing object` labels), Cura
(`;LAYER:n`, `;MESH:`), a .gcode.3mf plate converted output (`; CHANGE_LAYER`),
M486 labels, and a file with no DEFINE, labels or footer.

Run with: pytest tests/test_helix_analysis.py -v
"""

import asyncio
import io
import json
import shutil
import sys
import threading
import types
from pathlib import Path
from typing import Any, Dict, List

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import helix_analysis as analysis
import helix_gcode_reader as reader
from helix_analysis import (
    HelixAnalysis,
    analyze_file,
    convex_hull,
    merge_tools,
    parse_object_define,
    record_current,
)

FIXTURES = Path(__file__).parent / "fixtures" / "analysis"


@pytest.fixture
def gc(tmp_path):
    root = tmp_path / "gcodes"
    root.mkdir()
    for f in FIXTURES.glob("*.gcode"):
        shutil.copy(str(f), str(root / f.name))
    return root


def run(gc, name, metadata=None):
    return analyze_file(gc, name, metadata or {})


# ============================================================================
# Records per slicer
# ============================================================================


class TestOrca:
    def test_tools_come_from_the_footer(self, gc):
        r = run(gc, "orca_two_tool.gcode").record
        assert [(t["tool"], t["mm"], t["grams"], t["color"], t["material"]) for t in r["tools"]] \
            == [(0, 120.5, 0.36, "#FF8800", "PLA"), (1, 40.25, 0.12, "#0000FF", "PETG")]
        assert all(set(t["sources"].values()) == {"footer"} for t in r["tools"])

    def test_header_bounds_and_temps(self, gc):
        r = run(gc, "orca_two_tool.gcode").record
        assert r["slicer"] == "OrcaSlicer 2.3.1"
        assert r["header"] == {
            "printer_model": "Voron 2.4 250", "nozzle_diameters": [0.4, 0.4],
            "bed": {"min": [0.0, 0.0], "max": [250.0, 210.0], "max_height": 220.0},
            "filament_types": ["PLA", "PETG"],
        }
        # The purge line is printed, so it is inside the print bounds.
        assert r["bounds"]["print"] == {"min": [5.0, 5.0, 0.2], "max": [100.0, 25.0, 0.6]}
        assert r["bounds"]["max_hotend_temp"] == 240.0
        assert r["bounds"]["max_bed_temp"] == 60.0

    def test_objects_from_define_lines(self, gc):
        objects = run(gc, "orca_two_tool.gcode").record["objects"]
        assert objects["source"] == "define"
        assert [o["name"] for o in objects["list"]] == ["cube_id_0_copy_0", "cyl_id_1_copy_0"]
        assert objects["list"][0]["center"] == [20.0, 20.0]
        assert objects["list"][1]["polygon"] == [[55, 15], [65, 15], [65, 25], [55, 25]]

    def test_layers_and_pauses(self, gc):
        r = run(gc, "orca_two_tool.gcode").record
        assert r["layers"] == {"count": 3, "index": ".helix_index/orca_two_tool.gcode.layers.jsonl"}
        assert r["pauses"] == 1

    def test_embedded_thumbnail_is_used_when_metadata_has_none(self, gc):
        out = run(gc, "orca_two_tool.gcode")
        assert analysis.png_size(out.thumbnail_png) == (32, 24)  # the larger one

    def test_no_thumbnail_when_metadata_lists_one(self, gc):
        out = run(gc, "orca_two_tool.gcode", {"thumbnails": [{"width": 32}]})
        assert out.thumbnail_png is None


class TestPrusa:
    def test_objects_from_printing_object_labels(self, gc):
        r = run(gc, "prusa_labels.gcode").record
        assert r["objects"]["source"] == "label"
        a, b = r["objects"]["list"]
        assert a["name"] == "part_a id:0 copy 0"
        assert a["polygon"] == [[30.0, 40.0], [42.0, 40.0], [42.0, 52.0], [30.0, 52.0]]
        assert a["center"] == [36.0, 46.0]
        assert b["center"] == [86.0, 46.0]

    def test_header_reads_bed_shape_and_palette_fallback(self, gc):
        r = run(gc, "prusa_labels.gcode").record
        assert r["header"]["bed"] == {"min": [0.0, 0.0], "max": [250.0, 210.0],
                                      "max_height": 210.0}
        assert r["header"]["printer_model"] == "MK4"
        assert r["header"]["nozzle_diameters"] == [0.6]
        # extruder_colour is empty, so filament_colour answers
        assert r["tools"][0]["color"] == "#00FF00"

    def test_unknown_start_position_stays_out_of_the_bounds(self, gc):
        # The first move names only Z: X/Y are wherever the printer was.
        r = run(gc, "prusa_labels.gcode").record
        assert r["bounds"]["travel"]["min"][:2] == [30.0, 40.0]


class TestCura:
    def test_header_and_meters(self, gc):
        r = run(gc, "cura_mesh.gcode").record
        assert r["slicer"] == "Cura 5.3.1"
        assert r["header"]["printer_model"] == "Creality Ender-3"
        assert r["header"]["nozzle_diameters"] == [0.4]
        assert r["header"]["bed"] is None
        assert r["tools"][0]["mm"] == 50.0
        assert r["tools"][0]["sources"]["mm"] == "footer"

    def test_layers_and_mesh_labels(self, gc):
        r = run(gc, "cura_mesh.gcode").record
        assert r["layers"]["count"] == 3
        assert r["objects"]["source"] == "label"
        assert [o["name"] for o in r["objects"]["list"]] == ["cube.stl"]
        assert r["objects"]["list"][0]["polygon"] == [[10.0, 10.0], [30.0, 10.0], [30.0, 30.0],
                                                      [10.0, 30.0]]

    def test_thumbnail_is_generated_when_the_file_has_none(self, gc):
        png = run(gc, "cura_mesh.gcode").thumbnail_png
        assert png.startswith(b"\x89PNG")
        assert analysis.png_size(png) == (analysis.THUMB_SIZE, analysis.THUMB_SIZE)


class TestConverted3mf:
    def test_change_layer_markers_and_tools(self, gc):
        r = run(gc, "bambu_3mf_plate.gcode").record
        assert r["slicer"] == "BambuStudio 01.09.07.52"
        assert r["layers"]["count"] == 2
        assert [(t["tool"], t["color"]) for t in r["tools"]] == [(0, "#FFFFFF"), (1, "#000000")]
        assert r["header"]["printer_model"] == "Bambu Lab X1 Carbon"
        assert r["objects"] == {"source": "none", "list": []}


class TestM486:
    def test_named_labels_with_computed_polygons(self, gc):
        r = run(gc, "m486_objects.gcode").record
        assert r["objects"]["source"] == "label"
        assert [(o["name"], o["center"]) for o in r["objects"]["list"]] \
            == [("Box A", [12.5, 12.5]), ("Box B", [44.0, 14.0])]
        assert r["objects"]["list"][1]["polygon"] == [[40.0, 10.0], [48.0, 10.0], [48.0, 18.0],
                                                      [40.0, 18.0]]


class TestNoDefine:
    def test_everything_from_the_scan(self, gc):
        r = run(gc, "no_define.gcode").record
        assert r["objects"] == {"source": "none", "list": []}
        assert r["slicer"] is None
        (tool,) = r["tools"]
        assert tool["tool"] == 0
        assert tool["mm"] == pytest.approx(2.64)
        assert tool["sources"] == {"mm": "scan", "grams": None, "color": None, "material": None}
        assert r["bounds"]["print"] == {"min": [50.0, 50.0, 0.25], "max": [60.0, 60.0, 0.5]}


# ============================================================================
# Per-field source order
# ============================================================================


class TestMergeTools:
    FOOTER = {"mm": [100.0], "grams": [], "color": [], "material": ["PLA"]}

    def test_footer_then_metadata_then_scan_per_field(self):
        md = analysis._metadata_tools({
            "filament_weights": [3.0], "filament_weight_total": 3.0,
            "filament_colors": ["#112233"], "filament_type": "PETG",
        })
        (t,) = merge_tools([0], self.FOOTER, md, {0: 99.0})
        assert (t["mm"], t["grams"], t["color"], t["material"]) == (100.0, 3.0, "#112233", "PLA")
        assert t["sources"] == {"mm": "footer", "grams": "metadata", "color": "metadata",
                                "material": "footer"}

    def test_metadata_weights_that_do_not_add_up_are_not_grams(self):
        md = analysis._metadata_tools({"filament_weights": [1234.0],
                                       "filament_weight_total": 3.0})
        (t,) = merge_tools([0], {}, md, {})
        assert t["grams"] is None

    def test_metadata_total_length_answers_only_a_one_tool_file(self):
        md = analysis._metadata_tools({"filament_total": 500.0})
        one = merge_tools([0], {}, md, {})
        two = merge_tools([0, 1], {}, md, {0: 10.0, 1: 20.0})
        assert one[0]["mm"] == 500.0 and one[0]["sources"]["mm"] == "metadata"
        assert [t["mm"] for t in two] == [10.0, 20.0]
        assert {t["sources"]["mm"] for t in two} == {"scan"}

    def test_zero_amount_falls_through(self):
        (t,) = merge_tools([1], {"mm": [5.0, 0.0]}, {}, {1: 7.5})
        assert t["mm"] == 7.5 and t["sources"]["mm"] == "scan"

    def test_used_tools_come_from_the_footer_amounts_first(self):
        footer = {"grams": [0.0, 1.0, 0.0], "mm": []}
        assert analysis.tools_used(footer, {0: 5.0}, {0, 1}) == {1}
        assert analysis.tools_used({}, {0: 5.0, 2: 0.0}, {0, 2}) == {0}
        assert analysis.tools_used({}, {}, {3}) == {3}


# ============================================================================
# Pure helpers
# ============================================================================


def test_convex_hull_drops_interior_points():
    pts = [(0, 0), (2, 0), (2, 2), (0, 2), (1, 1), (1, 0)]
    assert convex_hull(pts) == [[0, 0], [2, 0], [2, 2], [0, 2]]


def test_running_hull_folds_batches():
    hull = analysis._Hull()
    hull.BATCH = 3
    for x, y in [(0, 0), (1, 1), (4, 0), (4, 4), (2, 2), (0, 4)]:
        hull.add(x, y)
    assert hull.result() == [[0, 0], [4, 0], [4, 4], [0, 4]]


def test_parse_object_define():
    d = parse_object_define("EXCLUDE_OBJECT_DEFINE NAME=a CENTER=1.5,2 POLYGON=[[0,0],[3,0],[3,4]]")
    assert d == {"name": "a", "center": [1.5, 2.0], "polygon": [[0.0, 0.0], [3.0, 0.0], [3.0, 4.0]]}
    assert parse_object_define("EXCLUDE_OBJECT_DEFINE CENTER=1,2") is None


@pytest.mark.parametrize("path,ok", [
    ("a.gcode", True), ("sub/b.GCO", True), ("c.g", True), ("d.gcode.3mf", False),
    (".helix_temp/x.gcode", False), ("sub/.hidden.gcode", False), ("notes.txt", False),
])
def test_is_analyzable(path, ok):
    assert analysis.is_analyzable(path) is ok


def test_record_current_compares_size_and_mtime():
    rec = {"schema": analysis.RECORD_SCHEMA, "file_size": 10, "file_mtime": 5.0}
    assert record_current(rec, 10, 5.0)
    assert not record_current(rec, 10, 6.0)
    assert not record_current(rec, 11, 5.0)
    assert not record_current(dict(rec, schema=0), 10, 5.0)
    assert not record_current(None, 10, 5.0)


def test_thumbnail_relpath_is_moonrakers_layout():
    assert analysis.thumbnail_relpath("sub/part.gcode", 300, 300) == "sub/.thumbs/part-300x300.png"


# ============================================================================
# The layer index sidecar
# ============================================================================


class TestLayerIndex:
    def entries(self, gc, name):
        run(gc, name)
        lines = [json.loads(l) for l in
                 open(str(reader.layer_index_path(gc, name))).read().splitlines()]
        return lines[0], [l for l in lines if "layer" in l], lines

    def test_identity_is_the_one_power_loss_recovery_checks(self, gc):
        import helix_plr
        run(gc, "orca_two_tool.gcode")
        identity = helix_plr.file_identity(gc / "orca_two_tool.gcode")
        summary = reader.read_layer_index(
            reader.layer_index_path(gc, "orca_two_tool.gcode"), identity)
        assert summary is not None and summary.complete
        assert summary.last.layer == 3

    def test_each_start_resumes_a_scan_to_the_same_end_state(self, gc):
        name = "orca_two_tool.gcode"
        run(gc, name)
        data = (gc / name).read_bytes()
        whole = reader.scan_to_offset(io.BytesIO(data), len(data))
        _header, starts, _ = self.entries(gc, name)
        assert len(starts) == 3
        for s in starts:
            resumed = reader.scan_to_offset(io.BytesIO(data), len(data), s["offset"],
                                            reader.ModalState.from_dict(s["state"]))
            assert resumed.state_at_head == whole.state_at_head

    def test_starts_carry_tools_print_z_and_feature(self, gc):
        _h, starts, _ = self.entries(gc, "orca_two_tool.gcode")
        assert [s["tools"] for s in starts] == [[0], [0, 1], [0, 1]]
        assert [s["print_z"] for s in starts] == [0.2, 0.4, 0.6]
        # The section in effect at the marker is the one the layer opens in.
        assert starts[0]["feature"] == "Custom"
        assert starts[1]["feature"] == "Inner wall"
        assert starts[1]["state"]["active_tool"] == 0

    def test_progress_lines_hold_pauses_and_m73(self, gc):
        _h, _s, lines = self.entries(gc, "orca_two_tool.gcode")
        (record,) = [l for l in lines if "progress_lines" in l]
        texts = [t for _o, t in record["progress_lines"]]
        assert "M600" in texts
        assert texts[0] == "M73 P0 R10"
        assert "SET_PRINT_STATS_INFO CURRENT_LAYER=2" in texts
        data = (gc / "orca_two_tool.gcode").read_bytes()
        for offset, text in record["progress_lines"]:
            assert data[offset:offset + len(text)].decode() == text

    def test_a_follower_index_in_place_is_replaced_whole(self, gc):
        name = "no_define.gcode"
        path = reader.layer_index_path(gc, name)
        identity = reader.file_identity(gc / name)
        partial = reader.LayerIndexWriter(path, identity)
        partial.append(1, 30, reader.ModalState())
        run(gc, name)
        partial.append(2, 99, reader.ModalState())  # lands in the replaced file
        partial.close()
        summary = reader.read_layer_index(path, identity)
        assert summary.complete and summary.last.layer == 2 and summary.last.offset != 99


# ============================================================================
# The component
# ============================================================================


class ServerError(Exception):
    def __init__(self, message, status_code=400):
        super().__init__(message)
        self.status_code = status_code


class FakeServer:
    def __init__(self):
        self.endpoints: Dict[str, Any] = {}
        self.handlers: Dict[str, List[Any]] = {}
        self.notifications: List[str] = []
        self.events: List[Any] = []

    def register_endpoint(self, path, methods, handler):
        self.endpoints[path] = handler

    def register_event_handler(self, event, handler):
        self.handlers.setdefault(event, []).append(handler)

    def register_notification(self, name):
        self.notifications.append(name)

    def send_event(self, event, data):
        self.events.append((event, data))

    def error(self, message, code=400):
        return ServerError(message, code)


class FakeDatabase:
    def __init__(self):
        self.ns: Dict[str, Dict[str, Any]] = {}

    async def insert_item(self, namespace, key, value):
        assert isinstance(key, list) and len(key) == 1
        self.ns.setdefault(namespace, {})[key[0]] = value

    async def get_item(self, namespace, key=None, default=None):
        items = self.ns.get(namespace)
        if key is None:
            return dict(items) if items is not None else default
        return items.get(key[0], default) if items else default

    async def delete_item(self, namespace, key):
        self.ns.get(namespace, {}).pop(key[0], None)


class FakeMetadataStorage:
    def __init__(self):
        self.entries: Dict[str, Dict[str, Any]] = {}

    def get(self, key, default=None):
        return self.entries.get(key, default)

    def insert(self, key, value):
        self.entries[key] = value


class FakeFileManager:
    def __init__(self):
        self.gcode_metadata = FakeMetadataStorage()

    def get_file_metadata(self, path):
        return self.gcode_metadata.get(path, {})


class FakeKlippyApis:
    def __init__(self, state="standby"):
        self.state = state

    async def query_objects(self, objects):
        return {"print_stats": {"state": self.state}}


class FakeHelix:
    def __init__(self, gc):
        self.server = FakeServer()
        self.gc_path = gc
        self.enabled = True
        self.database = FakeDatabase()
        self.file_manager = FakeFileManager()
        self.klippy_apis = FakeKlippyApis()
        self.feature_on = True

    async def feature_enabled(self, name):
        assert name == analysis.ANALYSIS_FEATURE
        return self.feature_on


class FakeRequest:
    def __init__(self, params):
        self.params = params

    def get_str(self, key, default=None):
        return self.params.get(key, default)


async def make(gc, cpu_count=4, state="standby", files=None):
    if files is not None:
        for f in list(gc.iterdir()):
            if f.is_file() and f.name not in files:
                f.unlink()
    helix = FakeHelix(gc)
    helix.klippy_apis.state = state
    comp = HelixAnalysis(helix, cpu_count=cpu_count)
    comp.register()
    await comp.start()
    return helix, comp


async def ask(comp, filename):
    return await comp.handle_analysis(FakeRequest({"filename": filename}))


async def settle(comp):
    await comp.wait_idle()
    await asyncio.sleep(0)


class TestComponent:
    @pytest.mark.asyncio
    async def test_backfill_writes_a_record_per_file_and_notifies(self, gc):
        helix, comp = await make(gc)
        await settle(comp)
        stored = helix.database.ns[analysis.DB_NAMESPACE]
        assert sorted(stored) == sorted(p.name for p in FIXTURES.glob("*.gcode"))
        assert ("helix:analysis_ready", {"filename": "cura_mesh.gcode"}) in helix.server.events
        assert helix.server.notifications == [analysis.READY_EVENT]
        reply = await ask(comp, "cura_mesh.gcode")
        assert reply["status"] == "ready"
        assert reply["analysis"]["layers"]["count"] == 3

    @pytest.mark.asyncio
    async def test_generated_thumbnail_is_published_and_listed_in_metadata(self, gc):
        (gc / "sub").mkdir()
        shutil.move(str(gc / "cura_mesh.gcode"), str(gc / "sub" / "cura_mesh.gcode"))
        helix = FakeHelix(gc)
        helix.file_manager.gcode_metadata.entries["sub/cura_mesh.gcode"] = {"size": 1}
        comp = HelixAnalysis(helix, cpu_count=4)
        comp.register()
        await comp.start()
        await settle(comp)
        thumb = comp.records["sub/cura_mesh.gcode"]["thumbnail"]
        assert thumb["relative_path"] == "sub/.thumbs/cura_mesh-300x300.png"
        assert (gc / thumb["relative_path"]).read_bytes().startswith(b"\x89PNG")
        listed = helix.file_manager.gcode_metadata.entries["sub/cura_mesh.gcode"]["thumbnails"]
        assert listed == [{"width": 300, "height": 300, "size": thumb["size"],
                           "relative_path": ".thumbs/cura_mesh-300x300.png"}]

    @pytest.mark.asyncio
    async def test_a_changed_file_is_pending_until_reanalyzed(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert (await ask(comp, "no_define.gcode"))["status"] == "ready"
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"G1 X70 Y70 E9\n")
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await settle(comp)
        reply = await ask(comp, "no_define.gcode")
        assert reply["status"] == "ready"
        assert reply["analysis"]["bounds"]["print"]["max"][:2] == [70.0, 70.0]

    @pytest.mark.asyncio
    async def test_modify_event_reanalyzes(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        before = len(helix.server.events)
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"; more\n")
        await comp.on_filelist_changed({"action": "modify_file",
                                        "item": {"root": "gcodes", "path": "no_define.gcode"}})
        await settle(comp)
        assert len(helix.server.events) == before + 1
        assert comp.records["no_define.gcode"]["file_size"] == path.stat().st_size

    @pytest.mark.asyncio
    async def test_paused_while_printing_on_one_core(self, gc):
        helix, comp = await make(gc, cpu_count=1, state="printing", files={"no_define.gcode"})
        await asyncio.sleep(0.05)
        assert comp.records == {}
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await asyncio.sleep(0.05)
        assert comp.records == {}
        await comp.on_job_state_changed(None, {}, {"state": "complete"})
        await settle(comp)
        assert "no_define.gcode" in comp.records

    @pytest.mark.asyncio
    async def test_runs_while_printing_on_a_multi_core_host(self, gc):
        helix, comp = await make(gc, cpu_count=4, state="printing", files={"no_define.gcode"})
        await settle(comp)
        assert "no_define.gcode" in comp.records

    def test_a_print_starting_on_one_core_cancels_the_scan(self):
        owner = types.SimpleNamespace(printing=True, cpu_count=1)
        cancel = threading.Event()
        analysis._Pacer(owner, cancel).pace(1)
        assert cancel.is_set()
        cancel.clear()
        owner.cpu_count = 2
        analysis._Pacer(owner, cancel).pace(1)
        assert not cancel.is_set()

    @pytest.mark.asyncio
    async def test_switched_off_answers_disabled_and_drops_the_queue(self, gc):
        helix = FakeHelix(gc)
        helix.feature_on = False
        comp = HelixAnalysis(helix, cpu_count=4)
        comp.register()
        await comp.start()
        await settle(comp)
        assert comp.records == {}
        assert (await ask(comp, "no_define.gcode"))["status"] == "disabled"
        helix.feature_on = True
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await settle(comp)
        assert set(comp.records) == {p.name for p in FIXTURES.glob("*.gcode")}

    @pytest.mark.asyncio
    async def test_bad_requests(self, gc):
        helix, comp = await make(gc, files=set())
        with pytest.raises(ServerError) as e:
            await ask(comp, "../etc/passwd.gcode")
        assert e.value.status_code == 400
        with pytest.raises(ServerError) as e:
            await ask(comp, "missing.gcode")
        assert e.value.status_code == 404

    @pytest.mark.asyncio
    async def test_delete_drops_record_index_and_generated_thumbnail(self, gc):
        helix, comp = await make(gc, files={"cura_mesh.gcode"})
        await settle(comp)
        record = comp.records["cura_mesh.gcode"]
        index = reader.layer_index_path(gc, "cura_mesh.gcode")
        thumb = gc / record["thumbnail"]["relative_path"]
        assert Path(index).exists() and thumb.exists()
        (gc / "cura_mesh.gcode").unlink()
        await comp.on_filelist_changed({"action": "delete_file",
                                        "item": {"root": "gcodes", "path": "cura_mesh.gcode"}})
        assert comp.records == {}
        assert analysis.DB_NAMESPACE not in helix.database.ns \
            or "cura_mesh.gcode" not in helix.database.ns[analysis.DB_NAMESPACE]
        assert not Path(index).exists() and not thumb.exists()

    @pytest.mark.asyncio
    async def test_move_keeps_the_record_and_index(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        (gc / "sub").mkdir()
        shutil.move(str(gc / "no_define.gcode"), str(gc / "sub" / "moved.gcode"))
        events = len(helix.server.events)
        await comp.on_filelist_changed({
            "action": "move_file", "item": {"root": "gcodes", "path": "sub/moved.gcode"},
            "source_item": {"root": "gcodes", "path": "no_define.gcode"}})
        await settle(comp)
        assert set(comp.records) == {"sub/moved.gcode"}
        assert len(helix.server.events) == events  # no second analysis
        assert comp.records["sub/moved.gcode"]["layers"]["index"] \
            == ".helix_index/sub/moved.gcode.layers.jsonl"
        assert Path(reader.layer_index_path(gc, "sub/moved.gcode")).exists()

    @pytest.mark.asyncio
    async def test_records_survive_a_restart_without_reanalysis(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        again = HelixAnalysis(helix, cpu_count=4)
        helix.server.events.clear()
        await again.start()
        await settle(again)
        assert helix.server.events == []
        assert (await ask(again, "no_define.gcode"))["status"] == "ready"

    @pytest.mark.asyncio
    async def test_records_of_vanished_files_are_dropped_at_startup(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode", "cura_mesh.gcode"})
        await settle(comp)
        (gc / "cura_mesh.gcode").unlink()
        again = HelixAnalysis(helix, cpu_count=4)
        await again.start()
        await settle(again)
        assert set(again.records) == {"no_define.gcode"}
        assert set(helix.database.ns[analysis.DB_NAMESPACE]) == {"no_define.gcode"}
