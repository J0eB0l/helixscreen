# SPDX-License-Identifier: GPL-3.0-or-later
"""
Power-loss recovery's save side (helix_plr.py): storage-safety verdict, the
PLR profile and switch, XY-homing safety, and the `plr` feature entry.

Run with: pytest tests/test_helix_plr.py -v
"""

import asyncio
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_plr
from helix_plr import (
    HomingCheck,
    PlrProfile,
    StorageVerdict,
    check_xy_homing,
    classify_block_device,
    effective_tier,
    parse_profile,
    saving_enabled,
    storage_verdict,
    user_enabled,
)


def run(coro):
    return asyncio.run(coro)


# --------------------------------------------------------------------------
# A fake /sys
# --------------------------------------------------------------------------


class FakeSys:
    """Builds /sys/dev/block/<maj>:<min> links into a fake /sys/devices tree."""

    def __init__(self, root: Path):
        self.root = root
        (root / "dev" / "block").mkdir(parents=True)
        self.devices = root / "devices"
        self.devices.mkdir()
        self._minor = 0

    def disk(self, name, files=None):
        disk = self.devices / "platform" / "block" / name
        disk.mkdir(parents=True)
        for rel, text in (files or {}).items():
            path = disk / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text + "\n")
        return disk

    def link(self, target, major=179):
        self._minor += 1
        dev = os.makedev(major, self._minor)
        (self.root / "dev" / "block" / ("%d:%d" % (major, self._minor))).symlink_to(target)
        return dev

    def partition(self, disk, name):
        part = disk / name
        part.mkdir()
        (part / "partition").write_text("1\n")
        return part


@pytest.fixture
def fake_sys(tmp_path):
    return FakeSys(tmp_path / "sys")


@pytest.mark.parametrize(
    "name, files, expected",
    [
        ("mmcblk0", {"device/type": "MMC"}, "emmc"),
        ("mmcblk1", {"device/type": "SD"}, "sd"),
        ("mmcblk2", {}, "unknown"),
        ("nvme0n1", {}, "ssd"),
        ("sda", {"removable": "0", "queue/rotational": "0"}, "ssd"),
        ("sdb", {"removable": "1", "queue/rotational": "0"}, "unknown"),
        ("sdc", {"removable": "0", "queue/rotational": "1"}, "unknown"),
        ("ubiblock0_1", {}, "nand"),
        ("mtdblock3", {}, "nand"),
        ("dm-0", {}, "unknown"),
    ],
)
def test_classify_block_device(fake_sys, name, files, expected):
    disk = fake_sys.disk(name, files)
    assert classify_block_device(fake_sys.root, fake_sys.link(disk)) == (expected, name)


def test_partition_resolves_to_its_disk(fake_sys):
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    part = fake_sys.partition(disk, "mmcblk0p10")
    assert classify_block_device(fake_sys.root, fake_sys.link(part)) == ("emmc", "mmcblk0")


def test_no_block_device_is_unknown(fake_sys):
    assert classify_block_device(fake_sys.root, os.makedev(0, 42)) == ("unknown", None)


def test_unreadable_type_is_unknown(fake_sys):
    disk = fake_sys.disk("mmcblk0", {})
    (disk / "device").mkdir()
    (disk / "device" / "type").mkdir()  # reading a directory fails
    assert classify_block_device(fake_sys.root, fake_sys.link(disk)) == ("unknown", "mmcblk0")


@pytest.mark.parametrize(
    "runtime, hint, expected",
    [
        ("emmc", "sd", StorageVerdict("emmc", True, "runtime", "dev")),
        ("sd", "emmc", StorageVerdict("sd", False, "runtime", "dev")),
        ("ssd", "unknown", StorageVerdict("ssd", True, "runtime", "dev")),
        ("unknown", "emmc", StorageVerdict("emmc", True, "printer_db", "dev")),
        ("unknown", "sd", StorageVerdict("sd", False, "printer_db", "dev")),
        ("nand", "nand", StorageVerdict("nand", True, "printer_db", "dev")),
        ("nand", "unknown", StorageVerdict("nand", False, "runtime", "dev")),
        ("unknown", "unknown", StorageVerdict("unknown", False, "none", "dev")),
        ("unknown", "bogus", StorageVerdict("unknown", False, "none", "dev")),
    ],
)
def test_storage_verdict(runtime, hint, expected):
    assert storage_verdict(runtime, "dev", hint) == expected


@pytest.mark.parametrize(
    "wear, tier, user, expected",
    [
        (True, 1, None, True),
        (True, 2, None, True),
        (True, 0, None, False),
        (False, 2, None, False),
        (False, 0, True, True),
        (True, 2, False, False),
    ],
)
def test_saving_enabled(wear, tier, user, expected):
    verdict = StorageVerdict("emmc" if wear else "sd", wear, "runtime", None)
    assert saving_enabled(verdict, tier, user) is expected


# --------------------------------------------------------------------------
# Profile and switch
# --------------------------------------------------------------------------


def test_missing_profile_is_tier_zero_with_defaults():
    assert parse_profile(None) == PlrProfile()
    assert PlrProfile().tier == 0
    assert PlrProfile().xy_home_gcode == "G28 X Y"


def test_profile_reads_every_key():
    profile = parse_profile({
        "tier": 2, "backend": "qidi_print_state", "storage_hint": "emmc",
        "z_holds_unpowered": True, "xy_home_gcode": "HOME_XY", "z_lift_mm": 3,
        "purge_gcode": "PURGE", "resume_skip_fraction": 0.25,
        "macro_vars": {"_STATE": ["a", "b", 3]},
    })
    assert profile == PlrProfile(2, "qidi_print_state", "emmc", True, "HOME_XY", 3.0,
                                 "PURGE", 0.25, {"_STATE": ("a", "b")})


def test_profile_bad_types_take_defaults():
    profile = parse_profile({
        "tier": "2", "backend": "marlin", "storage_hint": 4, "z_holds_unpowered": 1,
        "xy_home_gcode": "  ", "z_lift_mm": -1, "purge_gcode": "", "resume_skip_fraction": 2,
        "macro_vars": [],
    })
    assert profile == PlrProfile()
    assert parse_profile({"tier": 9}).tier == 3
    assert parse_profile({"tier": True}).tier == 0


@pytest.mark.parametrize(
    "record, expected",
    [(None, None), ({}, None), ({"enabled": "yes"}, None), ({"enabled": True}, True),
     ({"enabled": False}, False)],
)
def test_user_enabled(record, expected):
    assert user_enabled(record) is expected


# --------------------------------------------------------------------------
# XY homing
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "settings, kin, expected",
    [
        ({}, "corexy", HomingCheck(True, None)),
        ({"homing_override": {"axes": "z", "gcode": "G28 Z"}}, "corexy", HomingCheck(True, None)),
        ({"homing_override": {"axes": "xyz", "gcode": "G28 X\nG28 Y"}}, "corexy",
         HomingCheck(True, None)),
        ({"homing_override": {"axes": "xyz", "gcode": "G28 X Y\nG1 X100 Y100\nG28 Z"}},
         "corexy", HomingCheck(False, "override_homes_z")),
        ({"homing_override": {"gcode": "G28 ; all"}}, "cartesian",
         HomingCheck(False, "override_homes_z")),
        ({"homing_override": {"axes": "xy", "gcode": ["G28 X0", "g28 z0"]}}, "corexy",
         HomingCheck(False, "override_homes_z")),
        ({}, "delta", HomingCheck(False, "delta_kinematics")),
    ],
)
def test_check_xy_homing(settings, kin, expected):
    assert check_xy_homing(settings, kin) == expected


def test_unsafe_homing_drops_tier_unless_the_profile_names_a_command():
    unsafe = HomingCheck(False, "override_homes_z")
    assert effective_tier(PlrProfile(tier=2), unsafe) == (0, "override_homes_z")
    assert effective_tier(PlrProfile(tier=2, xy_home_gcode="HOME_XY_ONLY"), unsafe) == (2, None)
    assert effective_tier(PlrProfile(tier=1), HomingCheck(True, None)) == (1, None)


# --------------------------------------------------------------------------
# The `plr` feature through helix_print
# --------------------------------------------------------------------------

from test_helix_print import (  # noqa: E402
    MockConfigHelper,
    MockDatabase,
    MockFileManager,
    MockHistory,
    MockKlippyApis,
    MockServer,
)
from helix_print import load_component  # noqa: E402


@pytest.fixture
def helix(tmp_path, fake_sys):
    server = MockServer()
    gcodes = tmp_path / "printer_data" / "gcodes"
    gcodes.mkdir(parents=True)
    server.components["file_manager"] = MockFileManager(str(gcodes))
    server.components["database"] = MockDatabase()
    server.components["klippy_apis"] = MockKlippyApis()
    server.components["history"] = MockHistory()
    server.get_app_args = lambda: {"data_path": str(tmp_path / "printer_data")}
    component = load_component(MockConfigHelper(server, {
        "temp_dir": ".helix_temp", "symlink_dir": ".helix_print", "cleanup_delay": 3600,
        "enabled": True,
    }))
    run(component.component_init())
    return component


def test_plr_feature_entry_defaults_off_without_a_profile(helix):
    status = run(helix._handle_status(None))
    assert status["features"]["plr"] == {
        "version": 1, "available": True, "enabled": False, "reason": None}


def test_plr_feature_follows_storage_tier_and_switch(helix, fake_sys):
    db = helix.database
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    helix.plr.sys_root = fake_sys.root
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 1}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is True
    run(db.insert_item("helix-screen", "plr_user", {"enabled": False}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False
    # plugin_features is not PLR's switch
    run(db.insert_item("helix-screen", "plugin_features", {"plr": True}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False


def test_plr_feature_on_sd_needs_the_user(helix, fake_sys):
    db = helix.database
    disk = fake_sys.disk("mmcblk0", {"device/type": "SD"})
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 2, "storage_hint": "emmc"}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False
    run(db.insert_item("helix-screen", "plr_user", {"enabled": True}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is True


def test_plr_feature_reports_why_it_cannot_run(helix):
    helix.plr.has_virtual_sdcard = False
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert (entry["available"], entry["enabled"], entry["reason"]) == (
        False, False, "no_virtual_sdcard")


def test_storage_detection_reads_the_data_dir(tmp_path, fake_sys, monkeypatch):
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    part = fake_sys.partition(disk, "mmcblk0p10")
    dev = fake_sys.link(part)
    real_stat = os.stat

    def fake_stat(path, *a, **k):
        st = real_stat(path, *a, **k)
        if str(path) == str(tmp_path):
            return os.stat_result((st.st_mode, st.st_ino, dev) + tuple(st)[3:])
        return st

    monkeypatch.setattr(helix_plr.os, "stat", fake_stat)

    class Stub:
        server = None
        database = None

    plr = helix_plr.HelixPlr(Stub(), tmp_path, sys_root=fake_sys.root)
    plr.start()
    assert plr.runtime_storage == ("emmc", "mmcblk0")
