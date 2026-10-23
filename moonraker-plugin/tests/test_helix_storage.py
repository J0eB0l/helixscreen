# SPDX-License-Identifier: GPL-3.0-or-later
"""
The storage report and guard (helix_storage.py): the breakdown per consumer on
a fixture tree, the free-space floor print_modified and print_edited keep, and
cleanup, which refuses unknown consumers and never touches the user's files.

Run with: pytest tests/test_helix_storage.py -v
"""

import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_storage
from helix_storage import (
    AUTO_CLEANUP_FEATURE,
    CONSUMERS,
    CONSUMERS_BY_NAME,
    FREE_SPACE_FLOOR_BYTES,
    STORAGE_FEATURE,
    StorageError,
    classify_gcodes_path,
    cleanup_candidates,
    free_space_refusal,
    parse_older_than,
    resolve_consumer,
    scan_usage,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, PrintInfo
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockWebRequest,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

STORAGE = "/server/helix/storage"
CLEANUP = "/server/helix/storage_cleanup"
TEMP_DIRS = (".helix_temp", "helix_staging", ".helix_print")
MB = 1024 * 1024
OLD = 40 * 24 * 3600


class Request(MockWebRequest):
    def get(self, key, default=None):
        return self._params.get(key, default)


def _write(path: Path, size: int, age: float = 0.0) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x" * size)
    if age:
        t = time.time() - age
        os.utime(str(path), (t, t))
    return path


@pytest.fixture
def tree(tmp_path):
    """A printer_data with every consumer populated, sizes distinct per consumer."""
    data = tmp_path / "printer_data"
    gcodes = data / "gcodes"
    _write(gcodes / "benchy.gcode", 1000)
    _write(gcodes / "sub" / "part.gcode", 2000)
    _write(gcodes / ".helix_temp" / "modified_benchy.gcode", 300)
    _write(gcodes / "helix_staging" / "copy.gcode", 30)
    _write(gcodes / ".helix_index" / "benchy.gcode.layers.jsonl", 40)
    _write(gcodes / ".helix_index" / "sub" / "part.gcode.layers.jsonl", 4)
    _write(gcodes / ".helix_photos" / "00002A" / "layer2.jpg", 500)
    _write(gcodes / ".thumbs" / "benchy-32x32.png", 60)
    _write(gcodes / "sub" / ".thumbs" / "part-32x32.png", 6)
    _write(gcodes / "plate.gcode", 700)
    _write(gcodes / "plate.gcode.3mf", 70)
    _write(data / "timelapse" / "benchy.mp4", 9000)
    _write(data / "logs" / "klippy.log", 800)
    _write(data / "logs" / "klippy.log.2026-09-01", 80)
    _write(data / "helix-plr" / "snapshot.bin", 11)
    _write(data / "helix-plr" / "mesh.cfg", 22)
    return data


def _roots(data: Path, timelapse: bool = True):
    return {
        "gcodes": str(data / "gcodes"),
        "timelapse": str(data / "timelapse") if timelapse else None,
        "logs": str(data / "logs"),
        "data": str(data),
    }


# ============================================================================
# Breakdown
# ============================================================================

class TestClassify:

    @pytest.mark.parametrize("rel,consumer", [
        (".helix_temp/x.gcode", "helix_temp"),
        ("helix_staging/x.gcode", "helix_temp"),
        (".helix_print/full/sub/x.gcode", "helix_temp"),
        (".helix_index/sub/x.gcode.layers.jsonl", "analysis_sidecars"),
        (".helix_photos/00002A/end.jpg", "photos"),
        (".thumbs/x-32x32.png", "thumbnails"),
        ("deep/er/.thumbs/x-300x300.png", "thumbnails"),
        ("plate.gcode", "3mf_outputs"),
        ("x.gcode", "user_gcode"),
        ("sub/.helix_temp/x.gcode", "user_gcode"),
        (".helix_temp", "user_gcode"),
        (".hidden_by_user/x.gcode", "user_gcode"),
    ])
    def test_each_path_belongs_to_one_consumer(self, rel, consumer):
        assert classify_gcodes_path(rel, TEMP_DIRS, {"plate.gcode"}) == consumer


class TestScanUsage:

    def test_reports_every_consumer_in_one_order(self, tree):
        usage = scan_usage(_roots(tree), TEMP_DIRS, set())
        assert list(usage) == [c.name for c in CONSUMERS]

    def test_breakdown_per_consumer(self, tree):
        usage = scan_usage(_roots(tree), TEMP_DIRS, {"plate.gcode"})
        got = {name: (u["bytes"], u["files"]) for name, u in usage.items()}
        assert got == {
            "helix_temp": (330, 2),
            "3mf_outputs": (700, 1),
            "analysis_sidecars": (44, 2),
            "photos": (500, 1),
            "timelapse": (9000, 1),
            "logs": (880, 2),
            "thumbnails": (66, 2),
            "user_gcode": (3070, 3),
            "plr": (33, 2),
        }

    def test_cleanable_follows_the_consumer_table(self, tree):
        usage = scan_usage(_roots(tree), TEMP_DIRS, set())
        assert {n for n, u in usage.items() if u["cleanable"]} == {
            "helix_temp", "analysis_sidecars", "photos", "timelapse", "logs"}

    def test_a_missing_root_is_unavailable_and_not_cleanable(self, tree):
        usage = scan_usage(_roots(tree, timelapse=False), TEMP_DIRS, set())
        assert usage["timelapse"] == {
            "bytes": 0, "files": 0, "available": False, "cleanable": False}
        assert usage["user_gcode"]["available"] is True

    def test_a_root_inside_gcodes_counts_once(self, tree):
        roots = _roots(tree)
        nested = tree / "gcodes" / "timelapse"
        _write(nested / "inner.mp4", 5)
        roots["timelapse"] = str(nested)
        usage = scan_usage(roots, TEMP_DIRS, set())
        assert usage["timelapse"]["bytes"] == 5
        assert usage["user_gcode"]["bytes"] == 3770

    def test_a_linked_directory_is_not_followed(self, tree):
        gcodes = tree / "gcodes"
        os.symlink(str(tree / "timelapse"), str(gcodes / "usb"))
        usage = scan_usage(_roots(tree), TEMP_DIRS, set())
        assert usage["timelapse"]["bytes"] == 9000
        assert usage["user_gcode"] == {"bytes": 3770, "files": 4, "available": True,
                                       "cleanable": False}


# ============================================================================
# Free-space floor
# ============================================================================

class TestFreeSpaceRefusal:

    def test_exactly_the_floor_left_is_allowed(self):
        assert free_space_refusal(FREE_SPACE_FLOOR_BYTES + 10, 10) is None

    def test_one_byte_under_the_floor_refuses(self):
        refusal = free_space_refusal(FREE_SPACE_FLOOR_BYTES + 9, 10)
        assert refusal is not None
        assert "100 MB" in refusal
        assert "Settings > System > Storage" in refusal

    def test_the_incoming_copy_counts(self):
        assert free_space_refusal(FREE_SPACE_FLOOR_BYTES * 2, 0) is None
        assert free_space_refusal(FREE_SPACE_FLOOR_BYTES * 2, FREE_SPACE_FLOOR_BYTES + 1)

    def test_an_unreadable_volume_never_refuses(self):
        assert free_space_refusal(None, 10 ** 12) is None


def _free(monkeypatch, free_bytes):
    monkeypatch.setattr(helix_storage, "disk_usage",
                        lambda path: {"total": 10 ** 12, "used": 0, "free": free_bytes})


class TestGuard:

    @pytest.mark.asyncio
    async def test_print_modified_refuses_below_the_floor_and_drops_the_copy(
        self, helix_print_component, mock_server, temp_gcodes_dir, monkeypatch
    ):
        await helix_print_component.component_init()
        gcodes = Path(temp_gcodes_dir)
        _write(gcodes / "benchy.gcode", 10)
        copy = _write(gcodes / ".helix_temp" / "mod_benchy.gcode", 10)
        _free(monkeypatch, FREE_SPACE_FLOOR_BYTES - 1)

        with pytest.raises(Exception, match="507: Not enough free space"):
            await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
                "original_filename": "benchy.gcode",
                "temp_file_path": ".helix_temp/mod_benchy.gcode",
            }))

        assert not copy.exists()
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_print_modified_starts_at_the_floor(
        self, helix_print_component, mock_server, temp_gcodes_dir, monkeypatch
    ):
        await helix_print_component.component_init()
        gcodes = Path(temp_gcodes_dir)
        _write(gcodes / "benchy.gcode", 10)
        _write(gcodes / ".helix_temp" / "mod_benchy.gcode", 10)
        _free(monkeypatch, FREE_SPACE_FLOOR_BYTES)

        result = await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
        }))

        assert result["status"] == "printing"

    @pytest.mark.asyncio
    async def test_print_edited_refuses_before_writing_the_copy(
        self, helix_print_component, mock_server, temp_gcodes_dir, monkeypatch
    ):
        await helix_print_component.component_init()
        gcodes = Path(temp_gcodes_dir)
        _write(gcodes / "benchy.gcode", 1000)
        _free(monkeypatch, FREE_SPACE_FLOOR_BYTES + 999)

        with pytest.raises(Exception, match="507"):
            await mock_server.endpoints["/server/helix/print_edited"](MockWebRequest({
                "original_filename": "benchy.gcode",
                "temp_file_path": ".helix_temp/edited_benchy.gcode",
                "edits": [{"line_number": 1, "expect_raw_line": "", "action": "delete",
                           "args": {}}],
            }))

        assert list((gcodes / ".helix_temp").iterdir()) == []

    @pytest.mark.asyncio
    async def test_no_storage_module_means_no_guard(
        self, helix_print_component, mock_server, temp_gcodes_dir, monkeypatch
    ):
        await helix_print_component.component_init()
        helix_print_component.storage = None
        gcodes = Path(temp_gcodes_dir)
        _write(gcodes / "benchy.gcode", 10)
        _write(gcodes / ".helix_temp" / "mod_benchy.gcode", 10)
        _free(monkeypatch, 0)

        result = await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
        }))
        assert result["status"] == "printing"


# ============================================================================
# Cleanup rules
# ============================================================================

class TestCleanupRules:

    @pytest.mark.parametrize("name", ["user_gcode", "3mf_outputs", "thumbnails", "plr"])
    def test_consumers_holding_user_files_or_state_are_refused(self, name):
        with pytest.raises(StorageError, match="cannot be cleaned"):
            resolve_consumer(name)

    @pytest.mark.parametrize("name", ["", "gcodes", "../logs", None, 3])
    def test_unknown_consumers_are_refused(self, name):
        with pytest.raises(StorageError, match="Unknown storage consumer"):
            resolve_consumer(name)

    @pytest.mark.parametrize("value", ["x", None, -1, float("nan"), True])
    def test_older_than_must_be_seconds(self, value):
        with pytest.raises(StorageError):
            parse_older_than(value)

    def test_older_than_takes_numbers_and_numeric_strings(self):
        assert parse_older_than("3600") == 3600.0
        assert parse_older_than(0) == 0.0

    def test_age_protection_and_live_logs(self):
        logs = CONSUMERS_BY_NAME["logs"]
        files = [("/l/klippy.log", 5, 0.0), ("/l/klippy.log.1", 6, 0.0),
                 ("/l/moonraker.log.2026", 7, 950.0), ("/l/keep.log.2", 8, 0.0)]
        assert cleanup_candidates(logs, files, 1000.0, 100.0, {"/l/keep.log.2"}) == [
            ("/l/klippy.log.1", 6)]

    def test_temp_files_younger_than_the_minimum_age_stay(self):
        temp = CONSUMERS_BY_NAME["helix_temp"]
        files = [("/t/new", 1, 1000.0 - temp.min_age_s + 1), ("/t/old", 2, 0.0)]
        assert cleanup_candidates(temp, files, 1000.0, 0.0, set()) == [("/t/old", 2)]

    def test_sidecar_cleanup_leaves_a_worker_temp_to_the_prune(self):
        sidecars = CONSUMERS_BY_NAME["analysis_sidecars"]
        files = [("/i/a.gcode.layers.jsonl", 1, 0.0), ("/i/b.gcode.layers.jsonl.tmp", 2, 0.0)]
        assert cleanup_candidates(sidecars, files, 1000.0, 0.0, set()) == [
            ("/i/a.gcode.layers.jsonl", 1)]

    def test_an_uncleanable_consumer_yields_nothing(self):
        files = [("/g/x.gcode", 1, 0.0)]
        assert cleanup_candidates(CONSUMERS_BY_NAME["user_gcode"], files, 1e9, 0, set()) == []


# ============================================================================
# Endpoints
# ============================================================================

@pytest.fixture
def storage(helix_print_component, mock_server, tree, monkeypatch):
    helix = helix_print_component
    mock_server.components["file_manager"].get_directory = lambda name: {
        "gcodes": str(tree / "gcodes"),
        "timelapse": str(tree / "timelapse"),
        "logs": str(tree / "logs"),
    }.get(name, "")
    monkeypatch.setattr(helix, "_data_dir", lambda: tree)
    return helix


async def _init(helix, tree):
    await helix.component_init()
    return helix


class TestEndpoints:

    @pytest.mark.asyncio
    async def test_status_reports_storage_on_and_auto_cleanup_off(self, storage, tree,
                                                                  mock_server):
        await _init(storage, tree)
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))
        assert status["features"][STORAGE_FEATURE] == {
            "version": 1, "available": True, "enabled": True, "reason": None}
        assert status["features"][AUTO_CLEANUP_FEATURE]["enabled"] is False

    @pytest.mark.asyncio
    async def test_report_shape(self, storage, tree, mock_server):
        await _init(storage, tree)
        report = await mock_server.endpoints[STORAGE](Request({}))
        assert report["floor"] == FREE_SPACE_FLOOR_BYTES
        assert set(report["volume"]) == {"total", "used", "free"}
        assert report["auto_cleanup"] == {"enabled": False, "last_run": None}
        by_name = {c["name"]: c for c in report["consumers"]}
        assert [c["name"] for c in report["consumers"]] == [c.name for c in CONSUMERS]
        assert by_name["timelapse"] == {"name": "timelapse", "root": "timelapse",
                                        "bytes": 9000, "files": 1, "available": True,
                                        "cleanable": True}
        plr_dir = tree / "helix-plr"
        assert by_name["plr"]["bytes"] == sum(
            p.stat().st_size for p in plr_dir.rglob("*") if p.is_file())

    @pytest.mark.asyncio
    async def test_report_counts_the_converters_own_outputs(self, storage, tree, mock_server):
        await _init(storage, tree)

        class Conv:
            output, owned = "plate.gcode", True

        storage.converter = type("C", (), {"records": {"plate.gcode.3mf": [Conv()]}})()
        report = await mock_server.endpoints[STORAGE](Request({}))
        by_name = {c["name"]: c for c in report["consumers"]}
        assert by_name["3mf_outputs"]["bytes"] == 700

    @pytest.mark.asyncio
    async def test_cleanup_refuses_user_gcode_and_touches_nothing(self, storage, tree,
                                                                 mock_server):
        await _init(storage, tree)
        before = sorted(str(p) for p in tree.rglob("*"))
        for name in ("user_gcode", "nonsense"):
            with pytest.raises(Exception, match="400"):
                await mock_server.endpoints[CLEANUP](Request({"consumer": name,
                                                              "older_than": 0}))
        assert sorted(str(p) for p in tree.rglob("*")) == before

    @pytest.mark.asyncio
    async def test_cleanup_removes_only_its_consumer(self, storage, tree, mock_server):
        await _init(storage, tree)
        _write(tree / "gcodes" / ".helix_temp" / "modified_benchy.gcode", 300, age=OLD)
        _write(tree / "gcodes" / "helix_staging" / "copy.gcode", 30, age=OLD)
        result = await mock_server.endpoints[CLEANUP](Request({"consumer": "helix_temp",
                                                               "older_than": 0}))
        assert result == {"consumer": "helix_temp", "removed": 2, "freed": 330, "errors": 0}
        assert (tree / "gcodes" / ".helix_temp").is_dir()
        assert (tree / "gcodes" / "benchy.gcode").exists()
        assert (tree / "gcodes" / "sub" / "part.gcode").exists()

    @pytest.mark.asyncio
    async def test_cleanup_keeps_newer_files_and_the_active_print(self, storage, tree,
                                                                 mock_server):
        await _init(storage, tree)
        gcodes = tree / "gcodes"
        active = _write(gcodes / ".helix_temp" / "active.gcode", 5, age=OLD)
        recent = _write(gcodes / ".helix_temp" / "recent.gcode", 5, age=3600)
        old = _write(gcodes / ".helix_temp" / "old.gcode", 5, age=OLD)
        storage.active_prints[".helix_print/full/x.gcode"] = PrintInfo(
            "x.gcode", ".helix_temp/active.gcode", ".helix_print/full/x.gcode", [], 0.0)

        await mock_server.endpoints[CLEANUP](Request({"consumer": "helix_temp",
                                                      "older_than": 86400}))

        assert active.exists() and recent.exists()
        assert not old.exists()

    @pytest.mark.asyncio
    async def test_cleanup_keeps_what_klipper_is_printing(self, storage, tree, mock_server):
        await _init(storage, tree)
        held = _write(tree / "gcodes" / ".helix_photos" / "job" / "p.jpg", 5, age=OLD)
        storage.klippy_apis.query_objects.return_value = {
            "print_stats": {"filename": ".helix_photos/job/p.jpg", "state": "paused"}}

        await mock_server.endpoints[CLEANUP](Request({"consumer": "photos",
                                                      "older_than": 0}))

        assert held.exists()

    @pytest.mark.asyncio
    async def test_cleanup_removes_emptied_subdirectories_only(self, storage, tree,
                                                              mock_server):
        await _init(storage, tree)
        result = await mock_server.endpoints[CLEANUP](Request({"consumer": "photos",
                                                               "older_than": 0}))
        assert result["removed"] == 1
        assert (tree / "gcodes" / ".helix_photos").is_dir()
        assert not (tree / "gcodes" / ".helix_photos" / "00002A").exists()

    @pytest.mark.asyncio
    async def test_log_cleanup_keeps_live_logs(self, storage, tree, mock_server):
        await _init(storage, tree)
        await mock_server.endpoints[CLEANUP](Request({"consumer": "logs", "older_than": 0}))
        assert (tree / "logs" / "klippy.log").exists()
        assert not (tree / "logs" / "klippy.log.2026-09-01").exists()

    @pytest.mark.asyncio
    async def test_sidecar_cleanup_keeps_the_index_recovery_uses(self, storage, tree,
                                                                mock_server):
        await _init(storage, tree)
        kept = tree / "gcodes" / ".helix_index" / "benchy.gcode.layers.jsonl"
        storage.analysis = type("A", (), {"_protected_indexes": lambda self: [str(kept)]})()

        await mock_server.endpoints[CLEANUP](Request({"consumer": "analysis_sidecars",
                                                      "older_than": 0}))

        assert kept.exists()
        assert not (tree / "gcodes" / ".helix_index" / "sub" / "part.gcode.layers.jsonl").exists()


# ============================================================================
# Automatic cleanup
# ============================================================================

class TestAutoCleanup:

    async def _old_tree(self, storage, tree):
        await _init(storage, tree)
        for p in tree.rglob("*"):
            if p.is_file():
                t = time.time() - OLD
                os.utime(str(p), (t, t))

    @pytest.mark.asyncio
    async def test_off_by_default(self, storage, tree):
        await self._old_tree(storage, tree)
        storage.klippy_apis.query_objects.return_value = {"print_stats": {"state": "standby"}}
        assert await storage.storage.auto_cleanup() is None
        assert (tree / "gcodes" / ".helix_temp" / "modified_benchy.gcode").exists()

    @pytest.mark.asyncio
    async def test_opted_in_removes_scratch_and_rotated_logs_only(self, storage, tree):
        await self._old_tree(storage, tree)
        await storage.database.insert_item(
            HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, {AUTO_CLEANUP_FEATURE: True})
        storage.klippy_apis.query_objects.return_value = {"print_stats": {"state": "complete"}}

        result = await storage.storage.auto_cleanup()

        assert result["removed"] == 3
        assert not (tree / "gcodes" / ".helix_temp" / "modified_benchy.gcode").exists()
        assert not (tree / "logs" / "klippy.log.2026-09-01").exists()
        for kept in ("timelapse/benchy.mp4", "logs/klippy.log", "gcodes/benchy.gcode",
                     "gcodes/.helix_photos/00002A/layer2.jpg", "helix-plr/snapshot.bin",
                     "gcodes/.helix_index/benchy.gcode.layers.jsonl"):
            assert (tree / kept).exists(), kept
        assert storage.storage.last_auto_cleanup["removed"] == 3

    @pytest.mark.asyncio
    async def test_never_while_printing(self, storage, tree):
        await self._old_tree(storage, tree)
        await storage.database.insert_item(
            HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, {AUTO_CLEANUP_FEATURE: True})
        storage.klippy_apis.query_objects.return_value = {"print_stats": {"state": "printing"}}

        assert await storage.storage.auto_cleanup() is None
        assert (tree / "logs" / "klippy.log.2026-09-01").exists()
