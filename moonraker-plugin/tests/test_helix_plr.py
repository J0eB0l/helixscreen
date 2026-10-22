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
        "version": 1, "available": True, "enabled": False, "reason": None, "saving": False}


def test_plr_feature_follows_storage_tier_and_switch(helix, fake_sys):
    db = helix.database
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    helix.plr.sys_root = fake_sys.root
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 1}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert entry["enabled"] is True and entry["saving"] is True
    run(db.insert_item("helix-screen", "plr_user", {"enabled": False}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert entry["enabled"] is False and entry["saving"] is False
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
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 2, "z_holds_unpowered": True}))
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


# --------------------------------------------------------------------------
# Layer follower
# --------------------------------------------------------------------------

import io  # noqa: E402

import helix_gcode_reader  # noqa: E402
from helix_plr import LayerFollower  # noqa: E402


def layered_gcode(layers=40, lines=30):
    out = ["M83"]
    for n in range(1, layers + 1):
        out.append(";LAYER_CHANGE")
        out.append("G1 Z%.1f" % (n * 0.2))
        out.extend("G1 X%d Y%d E.05" % (i, n) for i in range(lines))
    return ("\n".join(out) + "\n").encode()


class NoThrottle:
    def __init__(self):
        self.calls = 0

    def pace(self, n):
        self.calls += 1


@pytest.fixture
def gfile(tmp_path):
    path = tmp_path / "gcodes" / "part.gcode"
    path.parent.mkdir()
    path.write_bytes(layered_gcode())
    return path


def ident(path):
    return helix_plr.file_identity(path)


def full_scan_starts(data):
    starts = {}
    helix_gcode_reader.scan_to_offset(
        io.BytesIO(data), len(data),
        on_layer_start=lambda layer, off, st: starts.__setitem__(layer, (off, st)))
    return starts


def test_follower_builds_the_index_in_steps(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    f = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    for pos in (1000, 5000, 5001, len(data) // 2, len(data)):
        f.step(pos)
    assert f.offset == len(data)
    f.finish(len(data))
    summary = helix_gcode_reader.read_layer_index(index, ident(gfile))
    assert summary.complete is True
    expected = full_scan_starts(data)
    for layer in (1, 17, 40):
        cp = helix_gcode_reader.read_layer_index(index, ident(gfile),
                                                 max_offset=expected[layer][0])
        assert cp.at_or_before.layer == layer
        assert cp.at_or_before.offset == expected[layer][0]
        assert cp.at_or_before.state == expected[layer][1]


def test_follower_never_reads_past_file_position(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    f = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    f.step(3000)
    assert f.offset <= 3000
    summary = helix_gcode_reader.read_layer_index(index, ident(gfile))
    assert summary.last.offset < 3000


def test_follower_resumes_from_its_index_after_a_restart(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    a = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    a.step(len(data) // 2)
    a.finish(len(data))
    b = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    b.step(0)
    assert b.offset == helix_gcode_reader.read_layer_index(index, ident(gfile)).last.offset
    b.step(len(data))
    b.finish(len(data))
    lines = open(index).read().splitlines()
    layers = [json.loads(l)["layer"] for l in lines[1:] if "layer" in json.loads(l)]
    assert layers == list(range(1, 41))          # no layer twice, none missing


def test_a_complete_index_is_not_followed_again(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    a = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    a.step(len(data))
    a.finish(len(data))
    throttle = NoThrottle()
    b = LayerFollower(gfile, index, ident(gfile), limiter=throttle)
    b.step(len(data))
    assert b.complete is True and b.offset < len(data) and throttle.calls == 0


def test_follower_is_throttled(gfile, monkeypatch):
    monkeypatch.setattr(helix_gcode_reader, "CHECK_INTERVAL_BYTES", 256)
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    throttle = NoThrottle()
    LayerFollower(gfile, index, ident(gfile), limiter=throttle).step(len(data))
    assert throttle.calls >= len(data) // 256 - 2 > 10


class SyncExecutor:
    """Runs follower steps at once on the calling thread."""

    def submit(self, fn, *args):
        from concurrent.futures import Future
        fut = Future()
        try:
            fut.set_result(fn(*args))
        except Exception as e:
            fut.set_exception(e)
        return fut

    def shutdown(self, wait=True):
        pass


def test_component_follows_the_print_and_closes_the_index(plr, helix, monkeypatch):
    monkeypatch.setattr(helix_plr, "FOLLOW_MIN_BYTES", 1000)
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    size = path.stat().st_size
    plr._follow_executor = SyncExecutor()
    added(plr)
    plr.follower = None
    tick(plr, **printing_status(500))
    assert plr.follower is not None and plr.follower.offset == 0   # under FOLLOW_MIN_BYTES
    tick(plr, 0.1, **printing_status(size // 2, layer=2))
    assert 0 < plr.follower.offset <= size // 2
    tick(plr, 0.1, **printing_status(size, layer=3))
    follower = plr.follower
    finished(plr, "completed")
    index = helix_gcode_reader.layer_index_path(helix.gc_path, "sub/part.gcode")
    assert helix_gcode_reader.read_layer_index(index, ident(path)).complete is True
    assert plr.follower is None and follower.complete is True


def test_component_follows_on_the_interval_too(plr, helix, monkeypatch):
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    plr._follow_executor = SyncExecutor()
    added(plr)
    tick(plr, **printing_status(600))
    assert plr.follower.offset == 0
    tick(plr, helix_plr.FOLLOW_INTERVAL_S, **printing_status(700))
    assert 0 < plr.follower.offset <= 700


def test_a_cancelled_print_leaves_the_index_open(plr, helix):
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    plr._follow_executor = SyncExecutor()
    added(plr)
    tick(plr, helix_plr.FOLLOW_INTERVAL_S, **printing_status(path.stat().st_size))
    finished(plr, "cancelled")
    index = helix_gcode_reader.layer_index_path(helix.gc_path, "sub/part.gcode")
    summary = helix_gcode_reader.read_layer_index(index, ident(path))
    assert summary.last is not None and summary.complete is False


# --------------------------------------------------------------------------
# Offer rules
# --------------------------------------------------------------------------

from helix_plr import OfferInputs, offer_verdict  # noqa: E402

OK_INPUTS = OfferInputs("interrupted", "A1", "A1", True, 5, 2, True, 300.0)


@pytest.mark.parametrize(
    "change, blocked",
    [
        ({}, []),
        ({"newest_job_status": "klippy_shutdown"}, ["not_interrupted"]),
        ({"newest_job_status": "completed"}, ["not_interrupted"]),
        ({"snapshot_job_id": None}, ["no_snapshot"]),
        ({"snapshot_job_id": "A0"}, ["no_snapshot"]),
        ({"identity_ok": False}, ["file_changed"]),
        ({"layer": 1}, ["too_early"]),
        ({"layer": None, "print_duration": 119.0}, ["too_early"]),
        ({"layer": None, "print_duration": 120.0}, []),
        ({"tier": 0}, ["not_supported"]),
        ({"klippy_ready": False}, ["klippy_not_ready"]),
        ({"tier": 0, "klippy_ready": False}, ["not_supported", "klippy_not_ready"]),
    ],
)
def test_offer_verdict(change, blocked):
    verdict = offer_verdict(OK_INPUTS._replace(**change))
    assert verdict.blocked_by == blocked and verdict.available is (not blocked)


# --------------------------------------------------------------------------
# Endpoints: status, plan, prepare, commit, abort, discard
# --------------------------------------------------------------------------

from helix_plr import RESUME_LINK  # noqa: E402


class Req:
    def __init__(self, **params):
        self.params = params

    def get(self, key, default=None):
        return self.params.get(key, default)


def straight_layers():
    out = ["M83", "G1 F600"]
    for n in range(1, 6):
        out += [";LAYER_CHANGE", "SET_PRINT_STATS_INFO CURRENT_LAYER=%d" % n,
                "G1 Z%.1f" % (n * 0.2)]
        out += ["G1 X%d Y10 E.05" % i for i in range(1, 101)]
    return ("\n".join(out) + "\n").encode()


@pytest.fixture
def resume(plr, helix):
    """A tier-2 printer after a power cut during layer 4 of sub/part.gcode."""
    data = straight_layers()
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(data)
    plr._follow_executor = SyncExecutor()
    plr._plan_executor = SyncExecutor()
    events = []
    helix.server.send_event = lambda name, payload: events.append((name, payload))
    plr.events = events
    added(plr, job_id="0000B2")
    helix.history.add_job("0000B2", filename="sub/part.gcode", status="interrupted")
    # The nozzle on X=50 of layer 4, the head 20 lines (1 mm of E) further on.
    layer4 = data.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    head = data.index(b"G1 X70 Y10", layer4)
    record = build_record(printing_status(head, layer=4), 1000.0)
    record.update({
        "toolhead_pos": [69.0, 10.0, 0.8, 1000.0],
        "live_pos": [49.5, 10.0, 0.8, 1000.0 - 20 * 0.05 - 0.025],
        "gcode_offset": [0.0, 0.0, 0.0, 0.0], "extrude_factor": 1.0, "speed_factor": 1.0,
        "active_extruder": "extruder", "retraction": None, "objects": [
            {"name": "A", "polygon": [[40, 0], [120, 0], [120, 40], [40, 40]]}],
        "excluded": [], "macro_vars": {}})
    record["heaters"]["heater_generic chamber"] = {"target": 0.0, "temp": 25.0}
    plr.store.write(fit_record(record)[0])
    plr.klippy_ready = True
    plr.policy.reset()

    printer = FakePrinter()
    helix.klippy_apis.query_objects.side_effect = printer.query
    helix.klippy_apis.run_gcode.side_effect = printer.run_gcode
    plr.printer = printer

    async def no_wait(seconds):
        plr.clock.t += seconds

    plr._sleep = no_wait
    return data


class FakePrinter:
    """Klipper as PLR sees it: heaters that reach a target once set (when
    `heats`), and homed axes that follow G28 X Y and SET_KINEMATIC_POSITION."""

    def __init__(self):
        self.bed = 55.0
        self.chamber = 25.0
        self.heats = True
        self.homed = ""
        self.state = "standby"
        self.fail_on = None

    async def run_gcode(self, script):
        if self.fail_on and script.startswith(self.fail_on):
            raise RuntimeError(self.fail_message)
        for line in script.splitlines():
            words = line.split()
            if not words:
                continue
            if words[0] == "M140" and self.heats:
                self.bed = float(words[1][1:])
            elif words[0] == "SET_HEATER_TEMPERATURE" and self.heats:
                self.chamber = float(line.split("TARGET=")[1])
            elif words[0] == "G28":
                self.homed = "".join(sorted(set(self.homed) | {"x", "y"}))
            elif words[0] == "SET_KINEMATIC_POSITION":
                homed = line.split("SET_HOMED=")[1].split()[0].lower() if "SET_HOMED=" in line \
                    else "xyz"
                self.homed = "".join(sorted(set(self.homed) | set(homed)))
        return "ok"

    def fail(self, prefix, message):
        self.fail_on, self.fail_message = prefix, message

    async def query(self, objects):
        out = {}
        if "print_stats" in objects:
            out["print_stats"] = {"state": self.state}
        if "heater_bed" in objects:
            out["heater_bed"] = {"temperature": self.bed, "target": 0.0}
        if "heater_generic chamber" in objects:
            out["heater_generic chamber"] = {"temperature": self.chamber}
        if "bed_mesh" in objects:
            out["bed_mesh"] = {"profiles": {"default": {}}}
        if "toolhead" in objects:
            out["toolhead"] = {"axis_minimum": [0, 0, 0, 0], "axis_maximum": [220, 220, 250, 0],
                               "homed_axes": self.homed}
        if "configfile" in objects:
            out["configfile"] = {"settings": {
                "printer": {"kinematics": "corexy"},
                "bed_mesh": {"mesh_min": [10.0, 10.0], "mesh_max": [210.0, 210.0]},
                "stepper_x": {"position_endstop": 220.0},
                "stepper_y": {"position_endstop": 220.0}}}
        return out


HEATERS_OFF = "M104 S0\nM140 S0\nSET_HEATER_TEMPERATURE HEATER=chamber TARGET=0\nM107"


def sent(helix):
    return [c.args[0] for c in helix.klippy_apis.run_gcode.call_args_list]


def test_status_offers_the_interrupted_job(plr, resume):
    reply = run(plr.handle_status(Req()))
    assert reply["offer"] == {"available": True, "blocked_by": []}
    assert (reply["tier"], reply["backend"], reply["saving"]) == (2, "generic", True)
    assert reply["snapshot"]["job_id"] == "0000B2" and reply["snapshot"]["layer"] == 4
    assert reply["snapshot"]["z"] == 0.8 and reply["snapshot"]["age_s"] == 0.0
    assert reply["storage"]["storage_class"] in ("emmc", "sd", "ssd", "nand", "unknown")


def test_status_blocks_a_job_that_finished_normally(plr, helix, resume):
    helix.history.jobs.clear()
    helix.history.add_job("0000B2", status="completed")
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["not_interrupted"]


def test_status_blocks_an_edited_file(plr, helix, resume):
    (helix.gc_path / "sub" / "part.gcode").write_bytes(resume + b"G1 X1\n")
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["file_changed"]


def test_a_qidi_profile_without_qidis_firmware_is_not_resumed(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 2, "backend": "qidi_print_state"}))
    reply = run(plr.handle_status(Req()))
    assert reply["backend"] == "none" and reply["tier"] == 0
    assert reply["offer"]["blocked_by"] == ["not_supported"]


def test_status_for_an_unlisted_printer(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    reply = run(plr.handle_status(Req()))
    assert reply["backend"] == "none" and reply["offer"]["blocked_by"] == ["not_supported"]


def test_plan_finds_the_measured_point(plr, resume):
    reply = run(plr.handle_plan(Req()))
    assert reply["method"] == "measured"
    layer4 = resume.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    # Skip half a second at 10 mm/s past X=49.5: the line ending at X=55.
    assert reply["resume_offset"] == resume.index(b"G1 X55 Y10", layer4)
    assert reply["filename"] == "sub/part.gcode" and reply["layer"] == 4
    assert reply["z_gcode"] == pytest.approx(0.8) and reply["z_physical"] == 0.8
    assert reply["requires"] == ["part_attached", "z_confirmed"]
    assert reply["warnings"] == [] and reply["bed"] == {"target": 60.0, "current": 55.0}
    assert "gcode_offset" in reply["restores"] and reply["plan_id"].startswith("0000B2-")
    plr.clock.t += 5.0
    assert run(plr.handle_plan(Req()))["plan_id"] == reply["plan_id"]   # same snapshot


def test_plan_refused_without_an_offer(plr, helix, resume):
    helix.history.jobs.clear()
    helix.history.add_job("0000B2", status="completed")
    with pytest.raises(Exception, match="plr_no_plan: not_interrupted"):
        run(plr.handle_plan(Req()))


def test_plan_refuses_multi_material(plr, resume):
    record = plr.store.load()
    record["active_extruder"] = "extruder1"
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: multi_material"):
        run(plr.handle_plan(Req()))


def test_plan_warns_about_a_lost_mesh_and_a_cold_bed(plr, helix, resume):
    plr.job["mesh"] = {"profile": "adaptive", "saved": False, "min": [0, 0], "max": [1, 1],
                       "points": [[0.0]]}

    base = plr.printer.query

    async def query(objects):
        out = await base(objects)
        out.update({k: v for k, v in {"heater_bed": {"temperature": 30.0},
                                       "bed_mesh": {"profiles": {}}}.items() if k in objects})
        return out

    helix.klippy_apis.query_objects.side_effect = query
    reply = run(plr.handle_plan(Req()))
    assert reply["warnings"] == ["mesh_unavailable", "cold_bed"]
    assert reply["requires"] == ["part_attached", "z_confirmed", "accept_no_mesh",
                                 "accept_cold_bed"]


def test_include_file_mesh_needs_no_extra_confirmation(plr, helix, resume):
    plr.job["mesh"] = {"profile": "adaptive", "saved": False, "min": [0, 0], "max": [1, 1],
                       "points": [[0.0]]}
    original = helix.klippy_apis.query_objects.side_effect

    async def query(objects):
        out = await original(objects)
        if "bed_mesh" in out:
            out["bed_mesh"] = {"profiles": {"helix_plr": {}}}
        return out

    helix.klippy_apis.query_objects.side_effect = query
    reply = run(plr.handle_plan(Req()))
    assert "mesh_unavailable" not in reply["warnings"] and "bed_mesh" in reply["restores"]


def full_resume(plr, plan_id, **commit):
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    params = {"plan_id": plan_id, "confirmations": {"part_attached": True, "z_confirmed": True},
              "z_reference": "trusted"}
    params.update(commit)
    return run(plr.handle_commit(Req(**params)))


def test_prepare_then_commit_starts_the_job(plr, helix, resume, monkeypatch):
    marks = []
    monkeypatch.setattr(helix, "mark_helix_initiated", lambda *a: marks.append(a))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    assert run(plr.handle_prepare(Req(plan_id=plan_id))) == {"ok": True}
    prepare = sent(helix)
    assert prepare[0] == "M140 S60"
    assert prepare[1].startswith("SET_KINEMATIC_POSITION Z=0.8 SET_HOMED=Z\nG91\nG1 Z5 F300")
    assert prepare[2] == "G28 X Y"
    assert not any("M104" in s or "M109" in s for s in prepare)
    reply = run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                      confirmations={"part_attached": True, "z_confirmed": True})))
    assert reply == {"ok": True, "job_started": True}
    start = sent(helix)[-1]
    assert start.startswith("M23 %s\nM26 S" % RESUME_LINK) and start.endswith("\nM24")
    assert marks == [(RESUME_LINK, "plr_resume", "sub/part.gcode")]
    link = helix.gc_path / RESUME_LINK
    assert link.is_symlink() and link.resolve() == (helix.gc_path / "sub" / "part.gcode").resolve()
    steps = [p["step"] for e, p in plr.events if e == "helix_print:plr_progress"
             and p["state"] == "done"]
    assert steps == ["bed_wait", "lift", "home_xy", "bed_wait", "restore", "heat_nozzle",
                     "move_to_resume", "start"]
    commit = sent(helix)[len(prepare):]
    assert commit[0] == "M140 S60"                          # the bed is re-asserted first


def test_commit_refuses_missing_confirmations_and_the_wrong_z(plr, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="plr_missing_confirmation: z_confirmed"):
        run(plr.handle_commit(Req(plan_id=plan_id, confirmations={"part_attached": True},
                                  z_reference="trusted")))
    with pytest.raises(Exception, match="plr_missing_confirmation"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="part_top",
                                  confirmations={"part_attached": True, "z_confirmed": True})))


def test_commit_before_prepare_is_refused(plr, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    with pytest.raises(Exception, match="plr_busy"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))


def test_a_stale_plan_is_refused(plr, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    with pytest.raises(Exception, match="plr_stale_plan"):
        run(plr.handle_prepare(Req(plan_id="other")))
    plr.store.write(fit_record(plr.store.load())[0])          # a newer snapshot
    with pytest.raises(Exception, match="plr_stale_plan"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))


def test_a_printing_printer_is_busy(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]

    async def printing(objects):
        return {"print_stats": {"state": "printing"}}

    helix.klippy_apis.query_objects.side_effect = printing
    with pytest.raises(Exception, match="plr_busy"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))


def test_a_failed_start_unmarks_and_turns_heaters_off(plr, helix, resume, monkeypatch):
    unmarked = []
    monkeypatch.setattr(helix, "unmark_helix_initiated", lambda name: unmarked.append(name))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))

    plr.printer.fail("M23", "SD busy")
    with pytest.raises(Exception, match="plr_klippy_error: SD busy"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))
    assert unmarked == [RESUME_LINK]
    assert not (helix.gc_path / RESUME_LINK).exists()
    assert sent(helix)[-1] == HEATERS_OFF


def test_a_klipper_error_in_prepare_fails_safe(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]

    plr.printer.fail("G28", "Endstop x still triggered")
    with pytest.raises(Exception, match="plr_klippy_error: Endstop"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert sent(helix)[-1] == HEATERS_OFF
    assert plr.store.load() is not None


def test_abort_turns_heaters_off_and_keeps_the_snapshot(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    assert run(plr.handle_abort(Req(plan_id=plan_id))) == {"ok": True}
    assert sent(helix)[-1] == HEATERS_OFF
    assert plr.store.load() is not None
    with pytest.raises(Exception, match="plr_no_plan"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))


def test_discard_deletes_the_snapshot(plr, helix, resume):
    run(plr.handle_plan(Req()))
    assert run(plr.handle_discard(Req())) == {"ok": True}
    assert plr.store.load() is None
    assert run(plr.handle_status(Req()))["snapshot"] is None
    with pytest.raises(Exception, match="plr_no_plan"):
        run(plr.handle_prepare(Req(plan_id="x")))


def test_a_long_scan_replies_pending_and_announces_the_plan(plr, resume, monkeypatch):
    import threading
    gate = threading.Event()

    class SlowExecutor:
        def __init__(self):
            from concurrent.futures import ThreadPoolExecutor
            self.pool = ThreadPoolExecutor(1)

        def submit(self, fn, *args):
            def slow():
                gate.wait(5)
                return fn(*args)
            return self.pool.submit(slow)

        def shutdown(self, wait=True):
            self.pool.shutdown(wait)

    plr._plan_executor = SlowExecutor()
    monkeypatch.setattr(helix_plr, "PLAN_SYNC_WAIT_S", 0.05)

    async def scenario():
        first = await plr.handle_plan(Req())
        plr.clock.t += 1.0
        again = await plr.handle_plan(Req())
        gate.set()
        import asyncio
        for _ in range(100):
            if any(e == "helix_print:plr_plan_ready" for e, _ in plr.events):
                break
            await asyncio.sleep(0.02)
        return first, again, await plr.handle_plan(Req())

    first, again, ready = run(scenario())
    assert first["pending"] is True and again == first
    assert ("helix_print:plr_plan_ready", {"plan_id": first["plan_id"]}) in plr.events
    assert ready["plan_id"] == first["plan_id"] and ready["method"] == "measured"


def test_a_root_level_file_starts_without_a_link(plr, helix, resume, monkeypatch):
    (helix.gc_path / "part.gcode").write_bytes(resume)
    added(plr, job_id="0000C3", filename="part.gcode")
    helix.history.add_job("0000C3", filename="part.gcode", status="interrupted")
    layer4 = resume.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    rec = build_record(printing_status(resume.index(b"G1 X70 Y10", layer4), layer=4), 1000.0)
    rec.update({"toolhead_pos": [69.0, 10.0, 0.8, 1000.0],
                "live_pos": [49.5, 10.0, 0.8, 1000.0 - 20 * 0.05 - 0.025],
                "gcode_offset": [0, 0, 0, 0], "extrude_factor": 1.0, "objects": [],
                "excluded": [], "macro_vars": {}})
    plr.store.write(fit_record(rec)[0])
    marks = []
    monkeypatch.setattr(helix, "mark_helix_initiated", lambda *a: marks.append(a))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    full_resume(plr, plan_id)
    assert sent(helix)[-1].startswith("M23 part.gcode\n")
    assert marks == [("part.gcode", "plr_resume", None)]
    assert not (helix.gc_path / RESUME_LINK).exists()


def test_the_resumed_job_is_named_after_the_file_and_the_link_removed(plr, helix, resume):
    patched = []

    async def patch(info, state):
        patched.append((info.job_id, info.original_filename, state))

    helix._patch_history_entry = patch
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    full_resume(plr, plan_id)
    added(plr, job_id="0000D4", filename=RESUME_LINK)
    assert plr.job["history_name"] == "sub/part.gcode"
    finished(plr, "completed", job_id="0000D4")
    assert patched == [("0000D4", "sub/part.gcode", "completed")]
    assert not (helix.gc_path / RESUME_LINK).exists()


def test_the_plr_endpoints_are_registered(helix):
    paths = helix.server.endpoints
    for name in ("status", "plan", "prepare", "commit", "abort", "discard"):
        assert "/server/helix/plr/" + name in paths


# --------------------------------------------------------------------------
# QIDI print_state backend through the core
# --------------------------------------------------------------------------

from unittest.mock import AsyncMock  # noqa: E402

import helix_plr_qidi  # noqa: E402


class ConfigTable:
    """QIDI's config-table methods on its Moonraker database."""

    def __init__(self, db):
        self.rows = {}
        db.update_value_from_config = self.update
        db.delete_value_from_config = self.delete
        db.get_value_from_config = self.get

    async def update(self, section, key, value):
        assert section == "print_state"
        self.rows[key] = value

    async def delete(self, section, key):
        if key is None:
            self.rows = {}

    async def get(self, section, key):
        value = self.rows.get(key)

        class Cursor:
            async def fetchone(self_inner):
                return {"value": value} if value is not None else None

        return Cursor()


@pytest.fixture
def qidi_printer(plr, helix, tmp_path):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 2, "backend": "qidi_print_state",
                                    "z_holds_unpowered": True}))
    table = ConfigTable(helix.database)
    apis = helix.klippy_apis
    apis.power_off_print = AsyncMock(return_value="ok")
    apis._send_klippy_request = AsyncMock(
        return_value={"SDCARD_POWER_OFF_CONTINUES_PINT_FILE": "resume"})
    apis.get_object_list.return_value = ["print_stats", "virtual_sdcard", "gcode_move",
                                         "toolhead", "extruder", "heater_bed"]
    apis.subscribe_objects.return_value = {
        "gcode_move": {"speed_factor_2": 1.5 / 60.0, "speed_2": 2.0, "base_position": [0, 0, 0, 0]},
        "virtual_sdcard": {"plate_index": 1}}
    run(plr.on_klippy_ready())
    cache = tmp_path / ".cache"
    cache.mkdir()
    plr.qidi._cache_dir = str(cache)
    plr.qidi._clock = plr.clock
    return table, cache


def test_qidi_is_selected_only_with_both_checks(plr, helix, qidi_printer):
    assert isinstance(plr.qidi, helix_plr_qidi.QidiPrintStateAdapter)
    del helix.klippy_apis.power_off_print
    run(plr._detect_qidi(helix.klippy_apis))
    assert plr.qidi is None


def test_qidi_on_mainline_klipper_is_not_selected(plr, helix):
    helix.klippy_apis.power_off_print = AsyncMock()
    ConfigTable(helix.database)
    helix.klippy_apis._send_klippy_request = AsyncMock(return_value={"G28": "home"})
    run(plr._detect_qidi(helix.klippy_apis))
    assert plr.qidi is None


def test_qidi_loop_writes_print_state_rows(plr, qidi_printer):
    table, cache = qidi_printer
    added(plr, job_id="0000E5")
    tick(plr, 0.5, **printing_status(100))
    assert table.rows == {}                      # QIDI deletes the section at start; wait
    tick(plr, 3.0, **printing_status(200))
    assert table.rows["file_position"] == "200"
    assert table.rows["speed_factor"].startswith("0.025")       # QIDI's own speed_factor_2


@pytest.fixture
def qidi_resume(plr, helix, resume, qidi_printer):
    table, cache = qidi_printer
    (cache / "part.gcode").write_bytes(resume)
    table.rows["file_position"] = "1"
    return table, cache


def test_qidi_status_and_offer(plr, helix, qidi_resume):
    table, _ = qidi_resume
    reply = run(plr.handle_status(Req()))
    assert (reply["backend"], reply["tier"]) == ("qidi_print_state", 2)
    assert reply["offer"] == {"available": True, "blocked_by": []}
    table.rows.clear()
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["no_snapshot"]


def test_qidi_unlisted_printer_stays_off(plr, helix, qidi_resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    reply = run(plr.handle_status(Req()))
    assert (reply["backend"], reply["tier"]) == ("none", 0)


def test_qidi_plan_prepare_commit(plr, helix, qidi_resume, monkeypatch):
    table, cache = qidi_resume
    marks = []
    plr.qidi._mark = lambda *a: marks.append(a)
    reply = run(plr.handle_plan(Req()))
    assert reply["tier"] == 2 and reply["method"] == "measured"
    assert "mesh_unavailable" not in reply["warnings"] and "no_purge_location" not in reply["warnings"]
    assert plr._plan.purge_xy is None and plr._plan.mesh_loadable is None   # QIDI's sequence purges
    run(plr.handle_prepare(Req(plan_id=reply["plan_id"])))
    assert sent(helix) == ["M140 S60"]                    # the heat wait only
    done = run(plr.handle_commit(Req(plan_id=reply["plan_id"], z_reference="trusted",
                                     confirmations={"part_attached": True, "z_confirmed": True})))
    assert done == {"ok": True, "job_started": True}
    assert helix.klippy_apis.power_off_print.await_count == 1
    assert table.rows["file_position"] == str(reply["resume_offset"] + 4096)
    assert marks == [(str(cache / "part.gcode"), "plr_resume")]
    assert sent(helix)[-1] == "M140 S60"                  # waited again inside commit
    assert not any(s.startswith("M23") for s in sent(helix))


def test_qidi_commit_failure_is_a_klippy_error_with_heaters_off(plr, helix, qidi_resume):
    helix.klippy_apis.power_off_print.side_effect = RuntimeError("no file")
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="plr_klippy_error: no file"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))
    assert sent(helix)[-1] == HEATERS_OFF


def test_qidi_abort_keeps_the_rows_and_turns_heaters_off(plr, helix, qidi_resume):
    table, _ = qidi_resume
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_abort(Req(plan_id=plan_id)))
    assert table.rows == {"file_position": "1"}
    assert sent(helix)[-1] == HEATERS_OFF


def test_qidi_discard_deletes_rows_and_snapshot(plr, helix, qidi_resume):
    table, _ = qidi_resume
    run(plr.handle_discard(Req()))
    assert table.rows == {} and plr.store.load() is None


def test_scan_follows_the_file_klipper_reads(plr, helix, resume):
    """A firmware that prints from a copy (QIDI's .cache, a .3mf's plate file)
    counts file_position in that copy; the scan and index key on it."""
    copy = helix.gc_path.parent / "copy.gcode"
    copy.write_bytes(b";plate header\n" * 10 + resume)
    record = plr.store.load()
    s = printing_status(100, layer=4)
    s["virtual_sdcard"]["file_path"] = str(copy)
    tick(plr, 0.1, **s)
    assert plr.job["print_path"] == str(copy)
    assert plr.job["print_identity"]["file_size"] == copy.stat().st_size
    path, index, ident = plr.print_target()
    assert path == copy.resolve() and ".helix_index/.external/copy.gcode" in index
    shift = len(b";plate header\n" * 10)
    record["file_position"] += shift
    plr.store.write(fit_record(record)[0])
    # The generic start would select the job's file, not the copy the offset is in.
    with pytest.raises(Exception, match="plr_scan_failed: printed_copy"):
        run(plr.handle_plan(Req()))
    scan, point = plr._plan_submit(
        helix_plr._scan_and_locate, path, record["file_position"], index, ident, record,
        0.5).result()
    layer4 = resume.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    assert point.offset == resume.index(b"G1 X55 Y10", layer4) + shift


# --------------------------------------------------------------------------
# Sequence safety: single flight, abort, a machine that changed since prepare
# --------------------------------------------------------------------------

COMMIT_OK = {"part_attached": True, "z_confirmed": True}


def commit(plr, plan_id):
    return run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                     confirmations=COMMIT_OK)))


def test_two_prepares_at_once_run_one(plr, helix, resume):
    """Two screens, or a retry, tapping Prepare together: the second is
    refused before either sends anything."""
    import asyncio
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    query = plr.printer.query

    async def yielding(objects):
        await asyncio.sleep(0)
        return await query(objects)

    helix.klippy_apis.query_objects.side_effect = yielding

    async def both():
        return await asyncio.gather(plr.handle_prepare(Req(plan_id=plan_id)),
                                    plr.handle_prepare(Req(plan_id=plan_id)),
                                    return_exceptions=True)

    results = run(both())
    assert sum(1 for r in results if r == {"ok": True}) == 1
    assert sum(1 for r in results if "plr_busy" in str(r)) == 1
    assert sum(1 for s in sent(helix) if s.startswith("SET_KINEMATIC_POSITION")) == 1


def test_abort_between_commit_steps_sends_nothing_more(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    real = plr.printer.run_gcode

    async def gcode(script):
        if script.startswith("SET_GCODE_OFFSET"):          # the restore step
            await plr.handle_abort(Req(plan_id=plan_id))
        return await real(script)

    helix.klippy_apis.run_gcode.side_effect = gcode
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert not any("M109" in s for s in sent(helix))


def test_abort_during_the_last_prepare_step_stays_aborted(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    real = plr.printer.run_gcode

    async def gcode(script):
        if script.startswith("G28"):
            await plr.handle_abort(Req(plan_id=plan_id))
        return await real(script)

    helix.klippy_apis.run_gcode.side_effect = gcode
    with pytest.raises(Exception, match="aborted"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert plr._sequence == "aborted"
    with pytest.raises(Exception, match="plr_no_plan"):
        commit(plr, plan_id)


def test_abort_during_the_last_commit_step_never_starts_the_print(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    real = plr.printer.run_gcode

    async def gcode(script):
        if "G92 E" in script:                       # move_to_resume, the last step
            await plr.handle_abort(Req(plan_id=plan_id))
        return await real(script)

    helix.klippy_apis.run_gcode.side_effect = gcode
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert not any(s.startswith("M23") for s in sent(helix))
    assert not (helix.gc_path / RESUME_LINK).exists()


def test_abort_during_a_heat_wait_stops_before_any_motion(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False

    async def abort_while_waiting(seconds):
        await plr.handle_abort(Req(plan_id=plan_id))

    plr._sleep = abort_while_waiting
    with pytest.raises(Exception, match="aborted"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert not any(s.startswith(("SET_KINEMATIC", "G28")) for s in sent(helix))


def test_a_heat_wait_times_out(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False
    with pytest.raises(Exception, match="plr_klippy_error: bed_wait timed out"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert sent(helix)[-1] == HEATERS_OFF


def test_the_chamber_wait_is_polled_and_skippable(plr, helix, resume):
    record = plr.store.load()
    record["heaters"]["heater_generic chamber"] = {"target": 45.0, "temp": 25.0}
    plr.store.write(fit_record(record)[0])
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False
    plr.printer.bed = 60.0
    with pytest.raises(Exception, match="chamber_wait timed out"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id, skip_chamber_wait=True)))
    assert "SET_HEATER_TEMPERATURE HEATER=chamber TARGET=45" in sent(helix)


def test_commit_after_a_klipper_restart_needs_a_new_prepare(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.on_klippy_gone()
    plr.klippy_ready = True
    with pytest.raises(Exception, match="plr_stale_plan"):
        commit(plr, plan_id)
    assert plr._sequence is None
    assert not any(s.startswith("M23") for s in sent(helix))


def test_commit_after_motors_off_needs_a_new_prepare(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.homed = ""                          # idle_timeout's M84
    with pytest.raises(Exception, match="plr_stale_plan"):
        commit(plr, plan_id)


def test_a_retried_prepare_never_redeclares_the_snapshot_z(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.fail("G28", "Endstop x still triggered")
    with pytest.raises(Exception):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.fail_on = None
    before = len(sent(helix))
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    retry = sent(helix)[before:]
    assert not any(s.startswith("SET_KINEMATIC_POSITION") for s in retry)   # Z still homed
    # Klipper restarted after the lift: the nozzle is at the lifted height, not the snapshot's.
    plr.printer.homed = ""
    before = len(sent(helix))
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    retry = sent(helix)[before:]
    assert "SET_KINEMATIC_POSITION Z=5.8 SET_HOMED=Z" in retry
    assert not any("G1 Z5 F300" in s for s in retry)


def test_no_room_to_lift_means_no_offer(plr, helix, resume):
    plr.axis_max = [220.0, 220.0, 2.5]
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["no_lift_room"]


@pytest.mark.parametrize("lift, expected", [(0, 5.0), (-3, 5.0), (0.5, 2.0), (8, 8.0)])
def test_profile_lift_is_never_below_the_minimum(lift, expected):
    assert parse_profile({"z_lift_mm": lift}).z_lift_mm == expected


def test_a_snapshot_without_a_live_position_is_not_planned(plr, resume):
    record = plr.store.load()
    record["live_pos"] = None
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: no_live_position"):
        run(plr.handle_plan(Req()))


def test_a_tier_three_profile_acts_at_tier_two(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 3, "z_holds_unpowered": True}))
    assert run(plr.handle_status(Req()))["tier"] == 2


def test_a_snapshot_of_another_job_is_not_offered(plr, resume):
    record = plr.store.load()
    record["job_id"] = "0000ZZ"
    plr.store.write(fit_record(record)[0])
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["no_snapshot"]


def test_a_failed_background_plan_reports_its_error_once(plr, resume, monkeypatch):
    import threading
    gate = threading.Event()
    record = plr.store.load()
    record["active_extruder"] = "extruder1"
    plr.store.write(fit_record(record)[0])
    submits = []

    class SlowExecutor:
        def __init__(self):
            from concurrent.futures import ThreadPoolExecutor
            self.pool = ThreadPoolExecutor(1)

        def submit(self, fn, *args):
            submits.append(fn)

            def slow():
                gate.wait(5)
                return fn(*args)
            return self.pool.submit(slow)

        def shutdown(self, wait=True):
            self.pool.shutdown(wait)

    plr._plan_executor = SlowExecutor()
    monkeypatch.setattr(helix_plr, "PLAN_SYNC_WAIT_S", 0.05)

    async def scenario():
        import asyncio
        first = await plr.handle_plan(Req())
        gate.set()
        for _ in range(100):
            if any(e == "helix_print:plr_plan_ready" for e, _ in plr.events):
                break
            await asyncio.sleep(0.02)
        errors = []
        for _ in range(2):
            try:
                await plr.handle_plan(Req())
            except Exception as e:
                errors.append(str(e))
        return first, errors

    first, errors = run(scenario())
    ready = [p for e, p in plr.events if e == "helix_print:plr_plan_ready"]
    assert first["pending"] is True and "multi_material" in ready[0]["error"]
    assert len(errors) == 2 and all("multi_material" in e for e in errors)
    assert len(submits) == 1


def test_qidi_abort_before_quidis_resume_call_stops_it(plr, helix, qidi_resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    write = plr.qidi._write

    async def write_then_abort(rows):
        ok = await write(rows)
        await plr.handle_abort(Req(plan_id=plan_id))
        return ok

    plr.qidi._write = write_then_abort
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert helix.klippy_apis.power_off_print.await_count == 0


def test_qidi_refuses_a_cache_file_that_is_not_the_snapshots(plr, helix, qidi_resume):
    _, cache = qidi_resume
    (cache / "part.gcode").write_bytes(b"G1 X1\n" * 10)
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="plr_scan_failed: qidi_cache_mismatch"):
        commit(plr, plan_id)
    assert helix.klippy_apis.power_off_print.await_count == 0


# --------------------------------------------------------------------------
# Part top, bed limits, homing overrides
# --------------------------------------------------------------------------

from helix_plr_scan import ResumePoint, state_before  # noqa: E402


def point_at(data, needle):
    scan = helix_gcode_reader.scan_to_offset(io.BytesIO(data), len(data))
    offset = data.index(needle)
    return scan, ResumePoint(offset, state_before(io.BytesIO(data), scan, offset),
                             "measured", 0.0, 0.0, [])


def test_part_top_skips_a_z_hop_at_the_cut():
    data = b"M83\nG1 Z.4\nG1 X10 Y10 E1\nG1 X20 Y10 E1\nG1 Z.8\nG1 X50 Y50\nG1 Z.4\n"
    scan, point = point_at(data, b"G1 X50 Y50")
    assert point.state.gcode_position()[2] == pytest.approx(0.8)
    assert helix_plr._part_top_z(scan, point) == pytest.approx(0.4)


def test_part_top_skips_the_next_layers_z():
    data = b"M83\nG1 Z.4\nG1 X10 Y10 E1\n;LAYER_CHANGE\nG1 Z.6\nG1 X20 Y20 E1\n"
    scan, point = point_at(data, b"G1 X20 Y20 E1")
    assert helix_plr._part_top_z(scan, point) == pytest.approx(0.4)


def test_tier1_without_any_extrusion_before_the_point_is_refused(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 1}))
    import helix_plr as module
    real = module._part_top_z
    module._part_top_z = lambda scan, point: None
    try:
        with pytest.raises(Exception, match="plr_scan_failed: no_part_top"):
            run(plr.handle_plan(Req()))
    finally:
        module._part_top_z = real


def test_the_purge_line_runs_inside_the_bed_not_the_travel(plr, resume):
    run(plr.handle_plan(Req()))
    x0, y0, x1, y1 = plr._plan.purge_xy
    assert (x0, y0) == (15.0, 205.0)     # back edge: mesh_max 210 - margin, not the axis 220
    assert plr._plan.park_xy == (220.0, 220.0)


@pytest.mark.parametrize(
    "override, reason",
    [
        ({"axes": "xyz", "gcode": "G28 X\nG28 Y", "set_position_z": 0.0}, "override_sets_z"),
        ({"axes": "xyz", "gcode": "_HOME_X\n_HOME_Y\n_HOME_Z"}, "override_unknown"),
        ({"axes": "xyz", "gcode": "{% if 'x' not in printer.toolhead.homed_axes %}\n"
                                  "G28 X\n{% endif %}\nG90\nG1 X100 F6000\nG28 Y"}, None),
        ({"axes": "xy", "gcode": "SET_KINEMATIC_POSITION Z=0\nG28 X Y"}, "override_unknown"),
    ],
)
def test_homing_overrides_that_may_move_z(override, reason):
    check = check_xy_homing({"homing_override": override}, "corexy")
    assert check == HomingCheck(reason is None, reason)


def test_qidi_refuses_a_landing_below_the_plastic(plr, helix, qidi_resume):
    record = plr.store.load()
    record["gcode_offset"] = [0.0, 0.0, -0.1, 0.0]
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: qidi_landing_below_part"):
        run(plr.handle_plan(Req()))
    record["gcode_offset"] = [0.0, 0.0, -0.04, 0.0]
    plr.store.write(fit_record(record)[0])
    assert run(plr.handle_plan(Req()))["method"] == "measured"


def test_qidi_landing_counts_the_mesh(plr, helix, qidi_resume):
    plr.job["mesh"] = {"points": [[-0.2, -0.2], [-0.2, -0.2]], "min": [0, 0], "max": [220, 220]}
    with pytest.raises(Exception, match="qidi_landing_below_part"):
        run(plr.handle_plan(Req()))


# --------------------------------------------------------------------------
# Resumed job's mesh, index pruning, one frame at a time
# --------------------------------------------------------------------------


def test_a_resumed_job_keeps_the_mesh_it_resumed_with(plr, helix, resume, tmp_path):
    cfg = tmp_path / "printer_data" / "config" / "helix-plr" / "mesh.cfg"
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text("[bed_mesh helix_plr]\n")
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    full_resume(plr, plan_id)
    added(plr, job_id="0000F6", filename=RESUME_LINK)
    assert cfg.exists()
    assert plr.job["history_name"] == "sub/part.gcode"
    added(plr, job_id="0000F7", filename=RESUME_LINK)        # a later print, not a resume
    assert not cfg.exists()


def test_layer_indexes_of_deleted_files_and_old_ones_are_pruned(tmp_path):
    gcodes = tmp_path / "gcodes"
    gcodes.mkdir()
    import os as _os
    paths = []
    for i in range(6):
        (gcodes / ("f%d.gcode" % i)).write_text("G1 X1\n")
        index = helix_gcode_reader.layer_index_path(gcodes, "f%d.gcode" % i)
        _os.makedirs(_os.path.dirname(index), exist_ok=True)
        open(index, "w").write("{}\n")
        _os.utime(index, (1000 + i, 1000 + i))
        paths.append(index)
    (gcodes / "f5.gcode").unlink()                         # the newest index's file is gone
    removed = helix_gcode_reader.prune_layer_indexes(gcodes, keep=3)
    assert sorted(removed) == sorted(paths[:2] + [paths[5]])
    assert all(_os.path.exists(p) for p in paths[2:5])


def test_two_status_frames_at_once_save_one_snapshot(plr):
    import asyncio
    added(plr)
    saving = plr._saving

    async def slow_saving():
        await asyncio.sleep(0)
        return await saving()

    plr._saving = slow_saving
    writes = []
    write = plr.store.write
    plr.store.write = lambda payload: (writes.append(payload), write(payload))[1]

    async def both():
        await asyncio.gather(plr.on_status_update(printing_status(100)),
                             plr.on_status_update(printing_status(200, layer=2)))

    run(both())
    assert len(writes) == 1           # two at once would also race on the slot seq


# --------------------------------------------------------------------------
# Backend selection, QIDI routing details, the offer outliving the switch
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "profile, qidi, safe, expected",
    [
        ({"tier": 0}, False, True, ("none", 0)),
        ({"tier": 0, "backend": "qidi_print_state"}, True, True, ("none", 0)),
        ({"tier": 1}, False, True, ("generic", 1)),
        ({"tier": 2}, False, True, ("generic", 1)),                      # Z may sag: jog
        ({"tier": 2, "z_holds_unpowered": True}, False, True, ("generic", 2)),
        ({"tier": 3, "z_holds_unpowered": True}, False, True, ("generic", 2)),
        ({"tier": 2, "z_holds_unpowered": True}, False, False, ("none", 0)),  # unsafe G28 X Y
        ({"tier": 2, "z_holds_unpowered": True}, True, True, ("generic", 2)),  # QIDI fw, generic
        ({"tier": 2, "z_holds_unpowered": True, "backend": "qidi_print_state"}, True, False,
         ("qidi_print_state", 2)),
        ({"tier": 2, "z_holds_unpowered": True, "backend": "qidi_print_state"}, False, True,
         ("none", 0)),                                                   # no QIDI endpoint
        ({"tier": 1, "backend": "qidi_print_state"}, True, True, ("none", 0)),   # no jog on QIDI
        ({"tier": 2, "backend": "qidi_print_state"}, True, True, ("none", 0)),   # Z unproven
        ({"tier": 2, "z_holds_unpowered": True, "backend": "vendor"}, True, True, ("none", 0)),
    ],
)
def test_backend_selection(plr, profile, qidi, safe, expected):
    plr.qidi = object() if qidi else None
    plr.homing = HomingCheck(True, None) if safe else HomingCheck(False, "override_homes_z")
    assert plr.backend(parse_profile(profile)) == expected


def test_qidi_refuses_a_cache_file_other_than_the_one_printed(plr, helix, qidi_resume):
    _, cache = qidi_resume
    other = cache.parent / "elsewhere.gcode"
    other.write_bytes((cache / "part.gcode").read_bytes())
    plr.job["print_path"] = str(other)
    plr.job["print_identity"] = helix_plr.file_identity(other)
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="qidi_cache_mismatch"):
        commit(plr, plan_id)


def test_a_failing_qidi_row_write_does_not_stop_the_snapshot_loop(plr, helix, qidi_printer):
    async def boom(*a, **k):
        raise RuntimeError("bad row")

    plr.qidi.on_snapshot = boom
    added(plr, job_id="0000G1")
    tick(plr, 3.0, **printing_status(100))
    tick(plr, 1.0, **printing_status(200))
    assert plr.store.load()["file_position"] == 200


def test_a_skipped_chamber_wait_is_not_waited_for_in_commit(plr, helix, resume):
    record = plr.store.load()
    record["heaters"]["heater_generic chamber"] = {"target": 45.0, "temp": 25.0}
    plr.store.write(fit_record(record)[0])
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False
    plr.printer.bed = 60.0
    run(plr.handle_prepare(Req(plan_id=plan_id, skip_chamber_wait=True)))
    assert commit(plr, plan_id) == {"ok": True, "job_started": True}


def test_turning_saving_off_keeps_an_existing_offer(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_user", {"enabled": False}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert entry["enabled"] is True                  # a snapshot is waiting
    assert entry["saving"] is False                  # what the settings switch shows
    assert run(plr.handle_status(Req()))["offer"]["available"] is True
    run(plr.handle_discard(Req()))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False
