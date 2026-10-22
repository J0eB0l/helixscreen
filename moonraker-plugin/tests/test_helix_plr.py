# SPDX-License-Identifier: GPL-3.0-or-later
"""
Power-loss recovery's save side (helix_plr.py): storage-safety verdict, the
PLR profile and switch, XY-homing safety, and the `plr` feature entry.

Run with: pytest tests/test_helix_plr.py -v
"""

import asyncio
import json
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
        server = MockServer()
        database = None

    plr = helix_plr.HelixPlr(Stub(), tmp_path, sys_root=fake_sys.root)
    plr.start()
    assert plr.runtime_storage == ("emmc", "mmcblk0")


# --------------------------------------------------------------------------
# Slot store
# --------------------------------------------------------------------------

from helix_plr import (  # noqa: E402
    SLOT_PAYLOAD_MAX,
    SLOT_SIZE,
    SavePolicy,
    SlotStore,
    build_record,
    decode_slot,
    encode_slot,
    fit_record,
    mesh_cfg,
    mesh_record,
    snapshot_objects,
)


@pytest.fixture
def store(tmp_path):
    s = SlotStore(tmp_path / "helix-plr" / "snapshot.bin")
    s.open()
    yield s
    s.close()


def payload(n):
    return json.dumps({"n": n}).encode()


def test_slot_round_trip_and_bounds():
    assert decode_slot(encode_slot(7, b'{"a":1}')) == (7, b'{"a":1}')
    assert len(encode_slot(1, b"x" * SLOT_PAYLOAD_MAX)) == SLOT_SIZE
    with pytest.raises(ValueError):
        encode_slot(1, b"x" * (SLOT_PAYLOAD_MAX + 1))
    assert decode_slot(b"\0" * SLOT_SIZE) is None


def test_store_is_preallocated_and_alternates(store):
    assert store.path.stat().st_size == 2 * SLOT_SIZE
    assert store.load() is None
    for n in range(1, 6):
        assert store.write(payload(n)) == n
        assert store.load() == {"n": n, "seq": n}
    assert store.path.stat().st_size == 2 * SLOT_SIZE


def test_reopen_continues_the_sequence(tmp_path):
    path = tmp_path / "snapshot.bin"
    a = SlotStore(path)
    a.open()
    a.write(payload(1))
    a.write(payload(2))
    a.close()
    b = SlotStore(path)
    b.open()
    assert b.seq == 2
    assert b.write(payload(3)) == 3
    assert b.load() == {"n": 3, "seq": 3}
    b.close()


def _damage(path, slot, how):
    data = bytearray(path.read_bytes())
    base = slot * SLOT_SIZE
    if how == "crc":
        data[base + 22] ^= 0xFF  # inside the payload
    elif how == "truncated":
        for i in range(base + 24, base + SLOT_SIZE):
            data[i] = 0
    elif how == "magic":
        data[base] = ord("X")
    elif how == "length":
        data[base + 12:base + 16] = (SLOT_SIZE * 2).to_bytes(4, "little")
    path.write_bytes(bytes(data))


@pytest.mark.parametrize("how", ["crc", "truncated", "magic", "length"])
def test_a_torn_newer_slot_falls_back_to_the_older(store, how):
    store.write(payload(1))  # slot 1
    store.write(payload(2))  # slot 0
    _damage(store.path, 0, how)
    assert store.load() == {"n": 1, "seq": 1}


@pytest.mark.parametrize("how", ["crc", "truncated"])
def test_a_torn_older_slot_does_not_matter(store, how):
    store.write(payload(1))
    store.write(payload(2))
    _damage(store.path, 1, how)
    assert store.load() == {"n": 2, "seq": 2}


def test_both_slots_torn_is_no_snapshot(store):
    store.write(payload(1))
    store.write(payload(2))
    _damage(store.path, 0, "crc")
    _damage(store.path, 1, "crc")
    assert store.load() is None


def test_clear_leaves_no_snapshot(store):
    store.write(payload(1))
    store.clear()
    assert store.load() is None


def test_a_wrong_sized_file_is_reinitialised(tmp_path):
    path = tmp_path / "snapshot.bin"
    path.write_bytes(b"junk")
    s = SlotStore(path)
    s.open()
    assert path.stat().st_size == 2 * SLOT_SIZE
    assert s.load() is None
    s.close()


def test_oversized_records_move_objects_and_macro_vars_out():
    small = {"layer": 3, "objects": [], "macro_vars": {}}
    data, moved = fit_record(small)
    assert moved == {} and json.loads(data) == small
    big = {"layer": 3, "objects": [{"name": "o%d" % i, "polygon": [[i, i]] * 20}
                                   for i in range(40)], "macro_vars": {"M": {"v": 1}}}
    data, moved = fit_record(big)
    assert len(data) <= SLOT_PAYLOAD_MAX
    assert json.loads(data) == {"layer": 3, "overflow": ["macro_vars", "objects"]}
    assert moved == {"objects": big["objects"], "macro_vars": {"M": {"v": 1}}}


# --------------------------------------------------------------------------
# Record, mesh and policy
# --------------------------------------------------------------------------

STATUS = {
    "print_stats": {"state": "printing", "print_duration": 120.5, "filament_used": 33.0,
                    "info": {"current_layer": 4, "total_layer": 90}},
    "virtual_sdcard": {"file_position": 123456},
    "toolhead": {"position": [10, 20, 0.8, 500], "extruder": "extruder", "max_velocity": 300,
                 "max_accel": 5000, "square_corner_velocity": 5, "minimum_cruise_ratio": 0.5},
    "motion_report": {"live_position": [9.5, 19.5, 0.8, 498.2]},
    "gcode_move": {"gcode_position": [10, 20, 0.6, 12], "homing_origin": [0, 0, 0.2, 0],
                   "absolute_coordinates": True, "absolute_extrude": False, "speed": 100,
                   "speed_factor": 1.0, "extrude_factor": 0.95},
    "extruder": {"target": 220, "temperature": 219.6, "pressure_advance": 0.04,
                 "smooth_time": 0.04},
    "extruder_stepper belt": {"pressure_advance": 0.1},
    "heater_bed": {"target": 60, "temperature": 60.1},
    "heater_generic chamber": {"target": 40, "temperature": 38},
    "fan": {"speed": 0.5},
    "fan_generic aux": {"speed": 0.2},
    "firmware_retraction": {"retract_length": 0.8, "retract_speed": 35,
                            "unretract_extra_length": 0, "unretract_speed": 30},
    "exclude_object": {"objects": [{"name": "A"}, {"name": "B"}], "excluded_objects": ["B"]},
    "gcode_macro _STATE": {"count": 3, "other": 1},
    "bed_mesh": {"profile_name": "default", "mesh_min": [10, 10], "mesh_max": [200, 200],
                 "probed_matrix": [[0.1, 0.2], [0.0, -0.1]],
                 "profiles": {"default": {"mesh_params": {"x_count": 2}}}},
}


def test_build_record_reads_every_section_4_1_field():
    r = build_record(STATUS, 1000.0, {"_STATE": ("count",)})
    assert r == {
        "schema": 1, "saved_at": 1000.0, "file_position": 123456,
        "toolhead_pos": [10.0, 20.0, 0.8, 500.0], "live_pos": [9.5, 19.5, 0.8, 498.2],
        "gcode_pos": [10.0, 20.0, 0.6, 12.0], "layer": 4, "total_layers": 90,
        "print_duration": 120.5, "filament_used": 33.0,
        "gcode_offset": [0.0, 0.0, 0.2, 0.0], "abs_coord": True, "abs_extrude": False,
        "speed": 100.0, "speed_factor": 1.0, "extrude_factor": 0.95,
        "heaters": {"extruder": {"target": 220.0, "temp": 219.6},
                    "heater_bed": {"target": 60.0, "temp": 60.1},
                    "heater_generic chamber": {"target": 40.0, "temp": 38.0}},
        "fans": {"fan": 0.5, "fan_generic aux": 0.2},
        "active_extruder": "extruder",
        "limits": {"velocity": 300.0, "accel": 5000.0, "scv": 5.0, "min_cruise_ratio": 0.5},
        "pa": {"extruder": {"advance": 0.04, "smooth_time": 0.04}},
        "retraction": {"length": 0.8, "speed": 35.0, "extra": 0.0, "unretract_speed": 30.0},
        "objects": [{"name": "A"}, {"name": "B"}], "excluded": ["B"],
        "macro_vars": {"_STATE": {"count": 3}}, "power_fail_seen": False,
    }


def test_build_record_tolerates_missing_objects():
    r = build_record({}, 5.0)
    assert r["file_position"] is None and r["retraction"] is None and r["heaters"] == {}


def test_snapshot_objects_subscribes_the_families():
    subs = snapshot_objects(["print_stats", "toolhead", "extruder", "extruder1",
                             "extruder_stepper belt", "heater_generic chamber", "fan_generic x",
                             "gcode_macro M", "virtual_sdcard", "webhooks"])
    assert sorted(subs) == ["extruder", "extruder1", "fan_generic x", "gcode_macro M",
                            "heater_generic chamber", "print_stats", "toolhead", "virtual_sdcard"]
    assert all(v is None for v in subs.values())


def test_mesh_record_and_cfg():
    saved = mesh_record(STATUS)
    assert (saved["profile"], saved["saved"], saved["params"]) == ("default", True, {"x_count": 2})
    adaptive = dict(STATUS, bed_mesh=dict(STATUS["bed_mesh"], profile_name="adaptive-1"))
    unsaved = mesh_record(adaptive)
    assert unsaved["saved"] is False
    text = mesh_cfg(unsaved)
    assert text.splitlines()[:5] == ["[bed_mesh helix_plr]", "version = 1", "points =",
                                     "\t  0.100000, 0.200000", "\t  0.000000, -0.100000"]
    assert "x_count = 2" in text and "algo = lagrange" in text and "max_y = 200.000000" in text
    big = dict(unsaved, points=[[0.0] * 7] * 7)
    assert "algo = bicubic" in mesh_cfg(big)
    assert mesh_record({"bed_mesh": {"probed_matrix": [[]]}}) is None


def rec(pos, **kw):
    r = build_record(STATUS, 0.0)
    r["file_position"] = pos
    r.update(kw)
    return r


def test_policy_interval_and_events():
    p = SavePolicy(1.0)
    assert p.decide(0.0, "printing", rec(10)) == "first"
    p.saved(0.0, rec(10))
    assert p.decide(0.5, "printing", rec(20)) is None       # under the interval
    assert p.decide(1.0, "printing", rec(10)) is None       # position unchanged
    assert p.decide(1.0, "printing", rec(30)) == "interval"
    p.saved(1.0, rec(30))
    assert p.decide(1.1, "printing", rec(31, layer=5)) == "event"
    assert p.decide(1.1, "printing", rec(31, excluded=["A", "B"])) == "event"
    assert p.decide(1.1, "printing", rec(31, speed_factor=1.2)) == "event"
    assert p.decide(1.1, "printing", rec(31, gcode_offset=[0, 0, 0.25, 0])) == "event"
    hot = rec(31)
    hot["heaters"] = dict(hot["heaters"], extruder={"target": 230.0, "temp": 219.0})
    assert p.decide(1.1, "printing", hot) == "event"
    warm = rec(31)
    warm["heaters"] = dict(warm["heaters"], extruder={"target": 220.0, "temp": 225.0})
    assert p.decide(1.1, "printing", warm) is None          # a temperature is not an event
    assert p.decide(1.1, "printing", rec(31, fans={"fan": 1.0})) == "event"


def test_policy_saves_once_on_pause_and_never_idle():
    p = SavePolicy(1.0)
    p.decide(0.0, "printing", rec(10))
    p.saved(0.0, rec(10))
    assert p.decide(0.2, "paused", rec(10)) == "pause"
    assert p.decide(5.0, "paused", rec(10)) is None
    assert p.decide(9.0, "standby", rec(99)) is None
    assert p.decide(9.0, "complete", rec(99)) is None


# --------------------------------------------------------------------------
# The snapshot loop inside the component
# --------------------------------------------------------------------------


class Clock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


@pytest.fixture
def plr(helix, tmp_path):
    config = tmp_path / "printer_data" / "config"
    config.mkdir()
    fm = helix.file_manager
    fm.get_directory = lambda name: str(config) if name == "config" else fm._gcodes_path
    db = helix.database
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 2}))
    run(db.insert_item("helix-screen", "plr_user", {"enabled": True}))
    (helix.gc_path / "sub").mkdir()
    (helix.gc_path / "sub" / "part.gcode").write_bytes(b"G1 X1\n" * 50000)
    p = helix.plr
    p.clock = Clock()
    return p


def added(plr, job_id="0000A1", filename="sub/part.gcode"):
    run(plr.on_history_changed({"action": "added", "job": {"job_id": job_id, "filename": filename}}))


def finished(plr, status, job_id="0000A1"):
    run(plr.on_history_changed({"action": "finished",
                                "job": {"job_id": job_id, "status": status}}))


def tick(plr, dt=1.0, **objects):
    plr.clock.t += dt
    run(plr.on_status_update(objects))


def printing_status(position, layer=1, state="printing"):
    s = json.loads(json.dumps(STATUS))
    s["print_stats"]["state"] = state
    s["print_stats"]["info"]["current_layer"] = layer
    s["virtual_sdcard"]["file_position"] = position
    return s


def test_job_start_records_identity(plr, helix):
    plr.store.write(b'{"stale": 1}')
    added(plr)
    job = run(helix.database.get_item("helix_plr", "job"))
    path = helix.gc_path / "sub" / "part.gcode"
    assert job["job_id"] == "0000A1" and job["filename"] == "sub/part.gcode"
    assert job["file_size"] == path.stat().st_size
    assert job["file_hash"] == helix_plr.file_hash(path)
    assert run(helix.database.get_item("helix_plr", "newest_job")) == {
        "job_id": "0000A1", "filename": "sub/part.gcode"}
    assert plr.store.load() is None


def test_file_hash_reads_both_ends(tmp_path):
    a = tmp_path / "a.gcode"
    b = tmp_path / "b.gcode"
    body = bytearray(b"G1 X1\n" * 50000)
    a.write_bytes(bytes(body))
    body[-3] = ord("9")
    b.write_bytes(bytes(body))
    assert helix_plr.file_hash(a) != helix_plr.file_hash(b)


def test_snapshots_are_saved_while_printing(plr):
    added(plr)
    tick(plr, **printing_status(100))
    assert plr.store.load()["file_position"] == 100
    tick(plr, 0.3, **printing_status(200))
    assert plr.store.load()["file_position"] == 100          # within the interval
    tick(plr, 1.0, **printing_status(300))
    assert plr.store.load()["file_position"] == 300
    tick(plr, 0.1, **printing_status(310, layer=2))
    assert plr.store.load()["layer"] == 2                    # layer change saves at once


def test_nothing_is_saved_when_saving_is_off_or_idle(plr, helix):
    added(plr)
    tick(plr, **printing_status(100, state="standby"))
    assert plr.store.load() is None
    run(helix.database.insert_item("helix-screen", "plr_user", {"enabled": False}))
    plr._switch = (True, float("-inf"))
    tick(plr, **printing_status(200))
    assert plr.store.load() is None


def test_nothing_is_saved_without_a_job(plr):
    tick(plr, **printing_status(100))
    assert plr.store.load() is None


@pytest.mark.parametrize("status", ["completed", "cancelled"])
def test_finished_jobs_clear_the_snapshot(plr, helix, status):
    added(plr)
    tick(plr, **printing_status(100))
    finished(plr, status)
    assert plr.store.load() is None
    assert run(helix.database.get_item("helix_plr", "job")) is None


@pytest.mark.parametrize("status", ["error", "klippy_shutdown", "klippy_disconnect"])
def test_failed_jobs_keep_the_snapshot(plr, helix, status):
    added(plr)
    tick(plr, **printing_status(100))
    finished(plr, status)
    assert plr.store.load()["file_position"] == 100
    assert run(helix.database.get_item("helix_plr", "job"))["finished_status"] == status


def test_another_jobs_finish_is_ignored(plr):
    added(plr)
    tick(plr, **printing_status(100))
    finished(plr, "completed", job_id="0000FF")
    assert plr.store.load()["file_position"] == 100


def test_the_job_record_survives_a_restart(plr, helix):
    added(plr)
    plr.job = None
    run(plr.load_job())
    assert plr.job["job_id"] == "0000A1"


def test_mesh_is_kept_after_layer_one(plr, helix, tmp_path):
    added(plr)
    adaptive = printing_status(100)
    adaptive["bed_mesh"]["profile_name"] = "adaptive"
    tick(plr, **adaptive)
    assert "mesh" not in plr.job
    adaptive["print_stats"]["info"]["current_layer"] = 2
    tick(plr, **adaptive)
    job = run(helix.database.get_item("helix_plr", "job"))
    assert job["mesh"]["saved"] is False and job["mesh"]["profile"] == "adaptive"
    cfg = tmp_path / "printer_data" / "config" / "helix-plr" / "mesh.cfg"
    assert cfg.read_text().startswith("[bed_mesh helix_plr]\n")
    finished(plr, "completed")
    assert not cfg.exists()


def test_a_saved_mesh_writes_no_cfg(plr, tmp_path):
    added(plr)
    tick(plr, **printing_status(100, layer=2))
    assert plr.job["mesh"]["saved"] is True
    assert not (tmp_path / "printer_data" / "config" / "helix-plr" / "mesh.cfg").exists()


def test_overflowing_objects_go_to_the_job_record(plr, helix):
    added(plr)
    s = printing_status(100)
    s["exclude_object"]["objects"] = [{"name": "obj%d" % i, "polygon": [[i, i]] * 30}
                                      for i in range(30)]
    tick(plr, **s)
    assert plr.store.load()["overflow"] == ["macro_vars", "objects"]
    job = run(helix.database.get_item("helix_plr", "job"))
    assert len(job["overflow"]["objects"]) == 30


def test_klippy_ready_subscribes_and_checks_virtual_sdcard(plr, helix):
    apis = helix.klippy_apis
    apis.get_object_list.return_value = ["print_stats", "toolhead", "extruder", "webhooks"]
    apis.subscribe_objects.return_value = {"toolhead": {"extruder": "extruder"}}
    run(plr.on_klippy_ready())
    assert plr.has_virtual_sdcard is False
    subs = apis.subscribe_objects.call_args[0][0]
    assert sorted(subs) == ["extruder", "print_stats", "toolhead"]
    assert plr.status == {"toolhead": {"extruder": "extruder"}}


def test_every_write_is_synced(store, monkeypatch):
    synced = []
    real = os.fdatasync
    monkeypatch.setattr(helix_plr.os, "fdatasync", lambda fd: (synced.append(fd), real(fd)))
    store.write(payload(1))
    store.write(payload(2))
    assert len(synced) == 2


def test_idle_updates_do_not_read_the_switch(plr):
    added(plr)

    async def boom():
        raise AssertionError("switch read while idle")

    plr.saving_enabled = boom
    tick(plr, **printing_status(100, state="standby"))
    tick(plr, **printing_status(100, state="complete"))
