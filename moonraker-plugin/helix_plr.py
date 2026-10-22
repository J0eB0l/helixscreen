# SPDX-License-Identifier: GPL-3.0-or-later
"""
Power-loss recovery for any Klipper printer: the HelixPrint plugin's PLR module.

Design: docs/devel/plans/2026-10-21-plugin-power-loss-recovery-design.md.

This module holds the save side (storage-safety verdict, PLR profile and
switch, the two-slot snapshot store, the save policy) and the offer rules.
helix_plr_scan.py finds the resume point, helix_plr_render.py renders the
resume scripts, and HelixPlr at the bottom is the Moonraker-facing component
helix_print.py creates. Everything above HelixPlr is pure and touches no
Moonraker object. Python 3.8, stdlib only.
"""

from __future__ import annotations

import json
import logging
import os
import struct
import time
import zlib
from pathlib import Path
from typing import Any, Callable, Dict, NamedTuple, Optional, Tuple

# ============================================================================
# Storage safety (§ 7.4)
# ============================================================================

STORAGE_CLASSES = ("emmc", "sd", "nand", "ssd", "unknown")

# Runtime evidence that outranks the printer DB hint.
_DEFINITE_RUNTIME = ("emmc", "sd", "ssd")

# Wear-tolerant whatever the source; raw NAND counts only on the DB's word.
_WEAR_TOLERANT = ("emmc", "ssd")


class StorageVerdict(NamedTuple):
    storage_class: str  # "emmc" | "ssd" | "nand" | "sd" | "unknown"
    wear_tolerant: bool
    source: str  # "runtime" | "printer_db" | "none"
    device: Optional[str]  # e.g. "mmcblk0"

    def to_dict(self) -> Dict[str, Any]:
        return dict(self._asdict())


def _read_text(path: Path) -> Optional[str]:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def classify_block_device(sys_root: Path, st_dev: int) -> Tuple[str, Optional[str]]:
    """(storage class, disk name) of the block device behind `st_dev`.

    `sys_root` is "/sys" on a host and a fake tree in tests. A partition
    resolves to its disk. A filesystem with no block device behind it
    (overlay, tmpfs, NFS) is "unknown".
    """
    node = sys_root / "dev" / "block" / ("%d:%d" % (os.major(st_dev), os.minor(st_dev)))
    try:
        disk = node.resolve(strict=True)
    except (OSError, RuntimeError):
        return "unknown", None
    if (disk / "partition").exists():
        disk = disk.parent
    name = disk.name
    if name.startswith("mmcblk"):
        kind = _read_text(disk / "device" / "type")
        if kind == "MMC":
            return "emmc", name
        if kind == "SD":
            return "sd", name
        return "unknown", name
    if name.startswith("nvme"):
        return "ssd", name
    if name.startswith("sd"):
        removable = _read_text(disk / "removable")
        rotational = _read_text(disk / "queue" / "rotational")
        if removable == "0" and rotational == "0":
            return "ssd", name
        return "unknown", name
    if name.startswith("ubi") or name.startswith("mtdblock"):
        return "nand", name
    return "unknown", name


def storage_verdict(
    runtime_class: str, runtime_device: Optional[str], db_hint: str
) -> StorageVerdict:
    """Runtime evidence wins when it is definite (emmc, sd, ssd); otherwise
    the printer DB's hint decides. Raw NAND is wear-tolerant only when the DB
    says the model has it."""
    if runtime_class in _DEFINITE_RUNTIME:
        return StorageVerdict(
            runtime_class, runtime_class in _WEAR_TOLERANT, "runtime", runtime_device
        )
    if db_hint in STORAGE_CLASSES and db_hint != "unknown":
        return StorageVerdict(
            db_hint, db_hint in _WEAR_TOLERANT or db_hint == "nand", "printer_db",
            runtime_device,
        )
    if runtime_class == "nand":
        return StorageVerdict("nand", False, "runtime", runtime_device)
    return StorageVerdict("unknown", False, "none", runtime_device)


def saving_enabled(verdict: StorageVerdict, tier: int, user_enabled: Optional[bool]) -> bool:
    """The user's switch when set; otherwise on only for wear-tolerant
    storage on a printer listed at tier 1 or above."""
    if user_enabled is not None:
        return user_enabled
    return verdict.wear_tolerant and tier >= 1


# ============================================================================
# PLR profile and switch (§ 7.2)
# ============================================================================

# helix-screen namespace keys: the app writes the resolved profile, and the
# user's on/off switch is PLR's only opt-in store.
PLR_PROFILE_KEY = "plr_profile"
PLR_USER_KEY = "plr_user"

BACKENDS = ("generic", "qidi_print_state", "vendor")

DEFAULT_XY_HOME_GCODE = "G28 X Y"
DEFAULT_Z_LIFT_MM = 5.0
RESUME_SKIP_FRACTION_DEFAULT = 0.5


class PlrProfile(NamedTuple):
    tier: int = 0
    backend: str = "generic"
    storage_hint: str = "unknown"
    z_holds_unpowered: bool = False
    xy_home_gcode: str = DEFAULT_XY_HOME_GCODE
    z_lift_mm: float = DEFAULT_Z_LIFT_MM
    purge_gcode: Optional[str] = None
    resume_skip_fraction: float = RESUME_SKIP_FRACTION_DEFAULT
    macro_vars: Dict[str, Tuple[str, ...]] = {}


def _num(value: Any) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if value != value or value in (float("inf"), float("-inf")):
        return None
    return float(value)


def parse_profile(record: Any) -> PlrProfile:
    """The app's plr_profile record. A missing record is tier 0, and a key
    that is missing or of the wrong type takes its default."""
    if not isinstance(record, dict):
        return PlrProfile()
    d = PlrProfile()
    tier = record.get("tier")
    tier = tier if isinstance(tier, int) and not isinstance(tier, bool) else d.tier
    backend = record.get("backend")
    storage = record.get("storage_hint")
    holds = record.get("z_holds_unpowered")
    xy_home = record.get("xy_home_gcode")
    lift = _num(record.get("z_lift_mm"))
    purge = record.get("purge_gcode")
    skip = _num(record.get("resume_skip_fraction"))
    macro_vars = {}  # type: Dict[str, Tuple[str, ...]]
    raw_vars = record.get("macro_vars")
    if isinstance(raw_vars, dict):
        for macro, names in raw_vars.items():
            if isinstance(macro, str) and isinstance(names, list):
                macro_vars[macro] = tuple(n for n in names if isinstance(n, str))
    return PlrProfile(
        tier=max(0, min(3, tier)),
        backend=backend if backend in BACKENDS else d.backend,
        storage_hint=storage if storage in STORAGE_CLASSES else d.storage_hint,
        z_holds_unpowered=holds if isinstance(holds, bool) else d.z_holds_unpowered,
        xy_home_gcode=xy_home if isinstance(xy_home, str) and xy_home.strip() else d.xy_home_gcode,
        z_lift_mm=lift if lift is not None and lift >= 0.0 else d.z_lift_mm,
        purge_gcode=purge if isinstance(purge, str) and purge.strip() else None,
        resume_skip_fraction=(
            skip if skip is not None and 0.0 <= skip <= 1.0 else d.resume_skip_fraction
        ),
        macro_vars=macro_vars,
    )


def user_enabled(record: Any) -> Optional[bool]:
    """plr_user.enabled, or None when the user has not chosen."""
    if isinstance(record, dict) and isinstance(record.get("enabled"), bool):
        return record["enabled"]
    return None


# ============================================================================
# XY-homing safety (§ 7.3)
# ============================================================================


class HomingCheck(NamedTuple):
    xy_home_safe: bool
    reason: Optional[str]  # "override_homes_z" | "delta_kinematics" | None


def _homes_z(gcode: str) -> bool:
    """Whether homing_override gcode homes Z: a G28 naming Z, or a bare G28."""
    for line in gcode.splitlines():
        words = line.split(";", 1)[0].upper().split()
        if not words or words[0] != "G28":
            continue
        axes = [w for w in words[1:] if w[:1] in ("X", "Y", "Z")]
        if not axes or any(w[:1] == "Z" for w in axes):
            return True
    return False


def check_xy_homing(configfile_settings: Any, kinematics: str) -> HomingCheck:
    """Whether `G28 X Y` homes only X and Y.

    A delta cannot home XY alone. A [homing_override] that runs for X or Y
    and whose gcode homes Z would bring the nozzle down onto the part.
    """
    if kinematics in ("delta", "rotary_delta", "deltesian"):
        return HomingCheck(False, "delta_kinematics")
    override = None
    if isinstance(configfile_settings, dict):
        override = configfile_settings.get("homing_override")
    if not isinstance(override, dict):
        return HomingCheck(True, None)
    axes = override.get("axes", "xyz")
    if isinstance(axes, list):
        axes = "".join(str(a) for a in axes)
    axes = str(axes).lower()
    if "x" not in axes and "y" not in axes:
        return HomingCheck(True, None)
    gcode = override.get("gcode", "")
    if isinstance(gcode, list):
        gcode = "\n".join(str(g) for g in gcode)
    if _homes_z(str(gcode)):
        return HomingCheck(False, "override_homes_z")
    return HomingCheck(True, None)


def effective_tier(profile: PlrProfile, homing: HomingCheck) -> Tuple[int, Optional[str]]:
    """The tier PLR acts at: 0 when `G28 X Y` is unsafe and the profile names
    no XY homing command of its own."""
    if not homing.xy_home_safe and profile.xy_home_gcode == DEFAULT_XY_HOME_GCODE:
        return 0, homing.reason
    return profile.tier, None


# ============================================================================
# Snapshot record (§ 4.1)
# ============================================================================

SNAPSHOT_SCHEMA = 1

# Persist at most this often while file_position moves (ruling 2).
SAVE_INTERVAL_S = 1.0

# file_hash reads this much from each end of the file.
HASH_SPAN_BYTES = 64 * 1024

# Klipper objects a snapshot reads, besides the per-name families below.
SNAPSHOT_OBJECTS = (
    "print_stats", "virtual_sdcard", "toolhead", "motion_report", "gcode_move",
    "heater_bed", "fan", "firmware_retraction", "exclude_object", "bed_mesh",
)
_HEATER_PREFIXES = ("heater_generic ", "temperature_fan ")
_FAN_PREFIXES = ("fan_generic ",)


def _is_extruder(name: str) -> bool:
    """extruder, extruder1, ...; not extruder_stepper."""
    return name.startswith("extruder") and name[len("extruder"):].isdigit() \
        or name == "extruder"


def _is_heater(name: str) -> bool:
    return _is_extruder(name) or name == "heater_bed" or name.startswith(_HEATER_PREFIXES)


def snapshot_objects(object_list: Any) -> Dict[str, Any]:
    """The subscription for a printer's object list: every object a snapshot
    reads, with all fields (None)."""
    names = set(SNAPSHOT_OBJECTS)
    for name in object_list or ():
        if not isinstance(name, str):
            continue
        if _is_heater(name) or name.startswith(_FAN_PREFIXES) \
                or name.startswith("gcode_macro "):
            names.add(name)
    available = set(object_list or ())
    return {n: None for n in sorted(names) if n in available or not available}


def file_hash(path: Path) -> str:
    """sha1 of the first and last HASH_SPAN_BYTES (the whole file when shorter)."""
    import hashlib
    h = hashlib.sha1()
    with open(str(path), "rb") as f:
        h.update(f.read(HASH_SPAN_BYTES))
        size = os.fstat(f.fileno()).st_size
        if size > HASH_SPAN_BYTES:
            f.seek(max(HASH_SPAN_BYTES, size - HASH_SPAN_BYTES))
            h.update(f.read(HASH_SPAN_BYTES))
    return h.hexdigest()


def file_identity(path: Path) -> Dict[str, Any]:
    st = os.stat(str(path))
    return {"file_size": st.st_size, "file_mtime": st.st_mtime, "file_hash": file_hash(path)}


def _vec(value: Any, n: int = 4) -> Optional[list]:
    if isinstance(value, (list, tuple)) and len(value) >= n:
        out = [_num(v) for v in value[:n]]
        if all(v is not None for v in out):
            return out
    return None


def _obj(status: Dict[str, Any], name: str) -> Dict[str, Any]:
    value = status.get(name)
    return value if isinstance(value, dict) else {}


def build_record(status: Dict[str, Any], now: float,
                 macro_vars: Optional[Dict[str, Tuple[str, ...]]] = None) -> Dict[str, Any]:
    """The fast and slow blocks of a snapshot from Klipper's merged status.
    `seq` is the store's to add; identity and mesh live in the job record."""
    ps = _obj(status, "print_stats")
    info = ps.get("info") if isinstance(ps.get("info"), dict) else {}
    vsd = _obj(status, "virtual_sdcard")
    th = _obj(status, "toolhead")
    gm = _obj(status, "gcode_move")
    heaters = {}
    fans = {}
    pa = {}
    macros = {}
    for name in sorted(status):
        obj = _obj(status, name)
        if _is_heater(name):
            heaters[name] = {"target": _num(obj.get("target")), "temp": _num(obj.get("temperature"))}
        if _is_extruder(name):
            pa[name] = {"advance": _num(obj.get("pressure_advance")),
                        "smooth_time": _num(obj.get("smooth_time"))}
        if name == "fan" or name.startswith(_FAN_PREFIXES):
            fans[name] = _num(obj.get("speed"))
        if name.startswith("gcode_macro ") and macro_vars:
            macro = name[len("gcode_macro "):]
            wanted = macro_vars.get(macro)
            if wanted:
                macros[macro] = {v: obj[v.lower()] for v in wanted if v.lower() in obj}
    fr = status.get("firmware_retraction")
    retraction = None
    if isinstance(fr, dict) and fr:
        retraction = {"length": _num(fr.get("retract_length")),
                      "speed": _num(fr.get("retract_speed")),
                      "extra": _num(fr.get("unretract_extra_length")),
                      "unretract_speed": _num(fr.get("unretract_speed"))}
    eo = _obj(status, "exclude_object")
    limits = {"velocity": _num(th.get("max_velocity")), "accel": _num(th.get("max_accel")),
              "scv": _num(th.get("square_corner_velocity")),
              "min_cruise_ratio": _num(th.get("minimum_cruise_ratio"))}
    layer = info.get("current_layer")
    total = info.get("total_layer")
    return {
        "schema": SNAPSHOT_SCHEMA,
        "saved_at": now,
        "file_position": vsd.get("file_position") if isinstance(vsd.get("file_position"), int) else None,
        "toolhead_pos": _vec(th.get("position")),
        "live_pos": _vec(_obj(status, "motion_report").get("live_position")),
        "gcode_pos": _vec(gm.get("gcode_position")),
        "layer": layer if isinstance(layer, int) else None,
        "total_layers": total if isinstance(total, int) else None,
        "print_duration": _num(ps.get("print_duration")),
        "filament_used": _num(ps.get("filament_used")),
        "gcode_offset": _vec(gm.get("homing_origin")),
        "abs_coord": gm.get("absolute_coordinates"),
        "abs_extrude": gm.get("absolute_extrude"),
        "speed": _num(gm.get("speed")),
        "speed_factor": _num(gm.get("speed_factor")),
        "extrude_factor": _num(gm.get("extrude_factor")),
        "heaters": heaters,
        "fans": fans,
        "active_extruder": th.get("extruder") if isinstance(th.get("extruder"), str) else None,
        "limits": limits,
        "pa": pa,
        "retraction": retraction,
        "objects": eo.get("objects") if isinstance(eo.get("objects"), list) else [],
        "excluded": eo.get("excluded_objects") if isinstance(eo.get("excluded_objects"), list) else [],
        "macro_vars": macros,
        "power_fail_seen": False,
    }


def mesh_record(status: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """The active bed mesh (§ 6.4): its profile name, whether that names a
    stored profile, and what an unsaved mesh needs to be written back."""
    bm = _obj(status, "bed_mesh")
    name = bm.get("profile_name")
    matrix = bm.get("probed_matrix")
    if not isinstance(matrix, list) or not matrix or not isinstance(matrix[0], list) \
            or not matrix[0]:
        return None
    profiles = bm.get("profiles") if isinstance(bm.get("profiles"), dict) else {}
    saved = bool(name) and name in profiles
    params = None
    if saved and isinstance(profiles[name], dict):
        params = profiles[name].get("mesh_params")
    return {
        "profile": name if isinstance(name, str) else "",
        "saved": saved,
        "min": _vec(bm.get("mesh_min"), 2),
        "max": _vec(bm.get("mesh_max"), 2),
        "params": params,
        "points": matrix,
    }


def mesh_cfg(mesh: Dict[str, Any], section: str = "helix_plr") -> Optional[str]:
    """An unsaved mesh as a `[bed_mesh <section>]` block in Klipper's autosave
    profile format. Klipper does not publish the live mesh's algorithm, so it
    is the one Klipper accepts for the point count (lagrange up to 6 points
    per axis, bicubic above)."""
    points = mesh.get("points")
    lo = mesh.get("min")
    hi = mesh.get("max")
    if not points or lo is None or hi is None:
        return None
    y_count = len(points)
    x_count = len(points[0])
    algo = "lagrange" if max(x_count, y_count) <= 6 else "bicubic"
    rows = ["\t  " + ", ".join("%.6f" % v for v in row) for row in points]
    lines = [
        "[bed_mesh %s]" % section,
        "version = 1",
        "points =",
    ] + rows + [
        "x_count = %d" % x_count, "y_count = %d" % y_count,
        "mesh_x_pps = 2", "mesh_y_pps = 2", "algo = %s" % algo, "tension = 0.2",
        "min_x = %.6f" % lo[0], "max_x = %.6f" % hi[0],
        "min_y = %.6f" % lo[1], "max_y = %.6f" % hi[1],
    ]
    return "\n".join(lines) + "\n"


# Fields whose change is saved at once rather than at the next interval.
_EVENT_KEYS = ("layer", "excluded", "speed_factor", "extrude_factor", "gcode_offset")


def _event_view(record: Dict[str, Any]) -> tuple:
    targets = tuple(sorted((k, v.get("target")) for k, v in record.get("heaters", {}).items()))
    fans = tuple(sorted(record.get("fans", {}).items()))
    return tuple(repr(record.get(k)) for k in _EVENT_KEYS) + (targets, fans)


class SavePolicy:
    """When to persist (§ 5.1): while printing, at most every interval while
    file_position moves, and at once on a layer change, an exclude, a change
    of factors, offset, heater targets or fans, and on entering pause."""

    def __init__(self, interval_s: float = SAVE_INTERVAL_S) -> None:
        self.interval_s = interval_s
        self.reset()

    def reset(self) -> None:
        self._last_at = None  # type: Optional[float]
        self._last_position = None  # type: Optional[int]
        self._last_view = None  # type: Optional[tuple]
        self._last_state = None  # type: Optional[str]

    def decide(self, now: float, state: str, record: Dict[str, Any]) -> Optional[str]:
        prev_state = self._last_state
        self._last_state = state
        if state == "paused":
            return "pause" if prev_state == "printing" else None
        if state != "printing":
            return None
        view = _event_view(record)
        if self._last_view is not None and view != self._last_view:
            return "event"
        if self._last_at is None:
            return "first"
        if record.get("file_position") != self._last_position \
                and now - self._last_at >= self.interval_s:
            return "interval"
        return None

    def saved(self, now: float, record: Dict[str, Any]) -> None:
        self._last_at = now
        self._last_position = record.get("file_position")
        self._last_view = _event_view(record)


# ============================================================================
# Two-slot snapshot store (§ 4.2)
# ============================================================================

SLOT_SIZE = 4096
SLOT_MAGIC = b"HPLR"
# magic, seq, payload length, crc32 of (seq, length, payload)
_SLOT_HEADER = struct.Struct("<4sQII")
SLOT_PAYLOAD_MAX = SLOT_SIZE - _SLOT_HEADER.size

# Moved to the job record when the slot cannot hold them.
OVERFLOW_KEYS = ("objects", "macro_vars")


def _slot_crc(seq: int, payload: bytes) -> int:
    return zlib.crc32(struct.pack("<QI", seq, len(payload)) + payload) & 0xFFFFFFFF


def encode_slot(seq: int, payload: bytes) -> bytes:
    if len(payload) > SLOT_PAYLOAD_MAX:
        raise ValueError("payload of %d bytes exceeds a slot" % len(payload))
    header = _SLOT_HEADER.pack(SLOT_MAGIC, seq, len(payload), _slot_crc(seq, payload))
    return (header + payload).ljust(SLOT_SIZE, b"\0")


def decode_slot(data: bytes) -> Optional[Tuple[int, bytes]]:
    """(seq, payload) of a valid slot, else None."""
    if len(data) < _SLOT_HEADER.size:
        return None
    magic, seq, length, crc = _SLOT_HEADER.unpack_from(data)
    if magic != SLOT_MAGIC:
        return None
    payload = bytes(data[_SLOT_HEADER.size:_SLOT_HEADER.size + length])
    if _slot_crc(seq, payload) != crc:
        return None
    return seq, payload


def fit_record(record: Dict[str, Any]) -> Tuple[bytes, Dict[str, Any]]:
    """The slot payload, and what moved out of it to fit (OVERFLOW_KEYS)."""
    payload = json.dumps(record, separators=(",", ":")).encode("utf-8")
    if len(payload) <= SLOT_PAYLOAD_MAX:
        return payload, {}
    slim = dict(record)
    moved = {k: slim.pop(k) for k in OVERFLOW_KEYS if k in slim}
    slim["overflow"] = sorted(moved)
    payload = json.dumps(slim, separators=(",", ":")).encode("utf-8")
    if len(payload) > SLOT_PAYLOAD_MAX:
        raise ValueError("snapshot of %d bytes does not fit a slot" % len(payload))
    return payload, moved


class SlotStore:
    """Two fixed SLOT_SIZE slots written alternately with pwrite + fdatasync.

    The file is preallocated and never resized, so a save writes one data
    block and no inode metadata. A cut mid-write can only tear the slot being
    written; the other holds the previous snapshot.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self._fd = None  # type: Optional[int]
        self.seq = 0

    def open(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(self.path), os.O_RDWR | os.O_CREAT, 0o644)
        try:
            if os.fstat(fd).st_size != 2 * SLOT_SIZE:
                os.ftruncate(fd, 0)
                os.pwrite(fd, b"\0" * (2 * SLOT_SIZE), 0)
                os.fsync(fd)
                _fsync_dir(self.path.parent)
        except OSError:
            os.close(fd)
            raise
        self._fd = fd
        latest = self._read_latest()
        self.seq = latest[0] if latest is not None else 0

    def close(self) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def _read_latest(self) -> Optional[Tuple[int, bytes]]:
        best = None
        for slot in (0, 1):
            found = decode_slot(os.pread(self._fd, SLOT_SIZE, slot * SLOT_SIZE))
            if found is not None and (best is None or found[0] > best[0]):
                best = found
        return best

    def load(self) -> Optional[Dict[str, Any]]:
        """The newest valid snapshot, with its seq; None when neither slot holds one."""
        latest = self._read_latest()
        if latest is None:
            return None
        try:
            record = json.loads(latest[1].decode("utf-8"))
        except ValueError:
            return None
        if not isinstance(record, dict):
            return None
        record["seq"] = latest[0]
        return record

    def write(self, payload: bytes) -> int:
        seq = self.seq + 1
        os.pwrite(self._fd, encode_slot(seq, payload), (seq % 2) * SLOT_SIZE)
        os.fdatasync(self._fd)
        self.seq = seq
        return seq

    def clear(self) -> None:
        os.pwrite(self._fd, b"\0" * (2 * SLOT_SIZE), 0)
        os.fdatasync(self._fd)


def _fsync_dir(path: Path) -> None:
    try:
        fd = os.open(str(path), os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


# ============================================================================
# The Moonraker-facing component
# ============================================================================

PLR_FEATURE = "plr"
PLR_FEATURE_VERSION = 1

# The app's settings namespace, where plr_profile and plr_user live.
HELIX_SCREEN_NAMESPACE = "helix-screen"

# PLR's own namespace: `job` (identity, mesh, overflow, finish status) and
# `newest_job` (the last job history added, whatever its client).
PLR_NAMESPACE = "helix_plr"
JOB_KEY = "job"
NEWEST_JOB_KEY = "newest_job"

SNAPSHOT_FILE = Path("helix-plr") / "snapshot.bin"
MESH_CFG_FILE = Path("helix-plr") / "mesh.cfg"

# How long the switch's answer is reused by the 4 Hz status path.
SWITCH_CACHE_S = 5.0

# History statuses after which the snapshot is of no further use.
_CLEARED_STATUSES = ("completed", "cancelled")


class HelixPlr:
    """PLR inside a running Moonraker. helix_print.py creates one in its
    component_init, after its database and Klipper APIs are resolved, and
    reaches PLR only through this class."""

    def __init__(self, helix: Any, data_dir: Path, sys_root: Path = Path("/sys"),
                 clock: Callable[[], float] = time.time) -> None:
        self.helix = helix
        self.server = helix.server
        self.database = helix.database
        self.data_dir = data_dir
        self.sys_root = sys_root
        self.clock = clock
        self.storage = StorageVerdict("unknown", False, "none", None)
        self.runtime_storage = ("unknown", None)  # type: Tuple[str, Optional[str]]
        self.has_virtual_sdcard = True
        self.store_error = None  # type: Optional[str]
        self.store = SlotStore(data_dir / SNAPSHOT_FILE)
        self.policy = SavePolicy()
        # Klipper's status, merged from the subscription's updates.
        self.status = {}  # type: Dict[str, Any]
        self.job = None  # type: Optional[Dict[str, Any]]
        self._switch = (False, float("-inf"))  # (value, read at)
        self._writing = False
        self._macro_vars = {}  # type: Dict[str, Tuple[str, ...]]

    def detect_storage(self) -> Tuple[str, Optional[str]]:
        try:
            st_dev = os.stat(str(self.data_dir)).st_dev
        except OSError:
            return "unknown", None
        return classify_block_device(self.sys_root, st_dev)

    async def _setting(self, key: str) -> Any:
        if self.database is None:
            return None
        try:
            return await self.database.get_item(HELIX_SCREEN_NAMESPACE, key, None)
        except Exception:
            return None

    async def profile(self) -> PlrProfile:
        return parse_profile(await self._setting(PLR_PROFILE_KEY))

    async def verdict(self, profile: Optional[PlrProfile] = None) -> StorageVerdict:
        if profile is None:
            profile = await self.profile()
        cls, dev = self.runtime_storage
        return storage_verdict(cls, dev, profile.storage_hint)

    async def saving_enabled(self) -> bool:
        """The `plr` feature's switch: saving_enabled() over the storage
        verdict, the profile's tier and plr_user.enabled."""
        profile = await self.profile()
        verdict = await self.verdict(profile)
        self.storage = verdict
        return saving_enabled(verdict, profile.tier, user_enabled(await self._setting(PLR_USER_KEY)))

    def availability(self) -> Tuple[bool, Optional[str]]:
        import sys
        if sys.version_info < (3, 8):
            return False, "python_too_old"
        if not self.has_virtual_sdcard:
            return False, "no_virtual_sdcard"
        if self.store_error is not None:
            return False, "store_unwritable"
        return True, None

    def start(self) -> None:
        self.runtime_storage = self.detect_storage()
        try:
            self.store.open()
        except OSError as e:
            self.store_error = str(e)
            logging.warning("HelixPrint: PLR snapshot store unwritable: %s", e)
        self.server.register_event_handler("server:status_update", self.on_status_update)
        self.server.register_event_handler("server:klippy_ready", self.on_klippy_ready)
        self.server.register_event_handler("history:history_changed", self.on_history_changed)

    async def load_job(self) -> None:
        """Read the job record back after a restart."""
        record = await self._ns_get(JOB_KEY)
        self.job = record if isinstance(record, dict) else None

    # -- database ------------------------------------------------------------

    async def _ns_get(self, key: str) -> Any:
        if self.database is None:
            return None
        try:
            return await self.database.get_item(PLR_NAMESPACE, key, None)
        except Exception:
            return None

    async def _ns_put(self, key: str, value: Any) -> None:
        if self.database is None:
            return
        try:
            await self.database.insert_item(PLR_NAMESPACE, key, value)
        except Exception as e:
            logging.warning("HelixPrint: PLR could not store %s: %s", key, e)

    async def _ns_delete(self, key: str) -> None:
        if self.database is None:
            return
        try:
            await self.database.delete_item(PLR_NAMESPACE, key)
        except Exception:
            pass

    # -- Klipper -------------------------------------------------------------

    async def on_klippy_ready(self) -> None:
        apis = self.helix.klippy_apis
        if apis is None:
            return
        try:
            objects = await apis.get_object_list()
        except Exception as e:
            logging.warning("HelixPrint: PLR could not list Klipper objects: %s", e)
            return
        self.has_virtual_sdcard = "virtual_sdcard" in objects
        try:
            status = await apis.subscribe_objects(snapshot_objects(objects))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not subscribe: %s", e)
            return
        self.status = {}
        self._merge(status)

    def _merge(self, status: Any) -> None:
        if not isinstance(status, dict):
            return
        for name, fields in status.items():
            if isinstance(fields, dict):
                self.status.setdefault(name, {}).update(fields)

    async def _saving(self) -> bool:
        value, read_at = self._switch
        now = self.clock()
        if now - read_at >= SWITCH_CACHE_S:
            value = await self.saving_enabled() and self.availability()[0]
            self._macro_vars = (await self.profile()).macro_vars
            self._switch = (value, now)
        return value

    async def on_status_update(self, status: Dict[str, Any], *args: Any) -> None:
        self._merge(status)
        if self.job is None or self._writing or self.store_error is not None:
            return
        state = _obj(self.status, "print_stats").get("state", "")
        if state not in ("printing", "paused"):
            self.policy.decide(self.clock(), state, {})
            return
        if not await self._saving():
            return
        now = self.clock()
        record = build_record(self.status, now, self._macro_vars)
        if self.policy.decide(now, state, record) is None:
            return
        await self._persist(now, record)
        await self._maybe_save_mesh(record)

    async def _persist(self, now: float, record: Dict[str, Any]) -> None:
        try:
            payload, moved = fit_record(record)
        except ValueError as e:
            logging.warning("HelixPrint: PLR snapshot skipped: %s", e)
            return
        self._writing = True
        try:
            await self.server.get_event_loop().run_in_thread(self.store.write, payload)
        except OSError as e:
            logging.warning("HelixPrint: PLR snapshot write failed: %s", e)
            return
        finally:
            self._writing = False
        self.policy.saved(now, record)
        if moved and self.job is not None and self.job.get("overflow") != moved:
            self.job["overflow"] = moved
            await self._ns_put(JOB_KEY, self.job)

    async def _maybe_save_mesh(self, record: Dict[str, Any]) -> None:
        """Once per job, when layer 1 is done (§ 5.1, § 6.4)."""
        if self.job is None or "mesh" in self.job:
            return
        layer = record.get("layer")
        if not isinstance(layer, int) or layer < 2:
            return
        mesh = mesh_record(self.status)
        self.job["mesh"] = None if mesh is None else {
            k: v for k, v in mesh.items() if k != "points"}
        if mesh is not None and not mesh["saved"]:
            text = mesh_cfg(mesh)
            if text is not None:
                await self._write_mesh_cfg(text)
        await self._ns_put(JOB_KEY, self.job)

    def _config_dir(self) -> Optional[Path]:
        fm = getattr(self.helix, "file_manager", None)
        if fm is None:
            return None
        path = fm.get_directory("config")
        return Path(path) if path else None

    async def _write_mesh_cfg(self, text: str) -> None:
        config = self._config_dir()
        if config is None:
            return
        path = config / MESH_CFG_FILE

        def write() -> None:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_suffix(".tmp")
            tmp.write_text(text)
            os.replace(str(tmp), str(path))

        try:
            await self.server.get_event_loop().run_in_thread(write)
        except OSError as e:
            logging.warning("HelixPrint: PLR could not write %s: %s", path, e)

    def _remove_mesh_cfg(self) -> None:
        config = self._config_dir()
        if config is not None:
            try:
                (config / MESH_CFG_FILE).unlink()
            except OSError:
                pass

    # -- history -------------------------------------------------------------

    async def on_history_changed(self, payload: Dict[str, Any]) -> None:
        job = payload.get("job") if isinstance(payload, dict) else None
        if not isinstance(job, dict) or not job.get("job_id"):
            return
        action = payload.get("action")
        if action == "added":
            await self._job_added(job)
        elif action == "finished":
            await self._job_finished(job)

    async def _job_added(self, job: Dict[str, Any]) -> None:
        job_id = str(job["job_id"])
        filename = job.get("filename", "")
        await self._ns_put(NEWEST_JOB_KEY, {"job_id": job_id, "filename": filename})
        self.policy.reset()
        self._switch = (False, float("-inf"))
        if self.store_error is None:
            try:
                self.store.clear()
            except OSError as e:
                logging.warning("HelixPrint: PLR could not clear the snapshot: %s", e)
        self._remove_mesh_cfg()
        record = {"schema": SNAPSHOT_SCHEMA, "job_id": job_id, "filename": filename,
                  "started_at": self.clock()}
        gc_path = getattr(self.helix, "gc_path", None)
        if gc_path is not None and filename:
            try:
                record.update(file_identity(gc_path / filename))
            except OSError as e:
                logging.warning("HelixPrint: PLR could not read %s: %s", filename, e)
        self.job = record
        await self._ns_put(JOB_KEY, record)

    async def _job_finished(self, job: Dict[str, Any]) -> None:
        if self.job is None or str(job["job_id"]) != self.job.get("job_id"):
            return
        status = job.get("status")
        if status in _CLEARED_STATUSES:
            await self.discard()
            return
        self.job["finished_status"] = status
        await self._ns_put(JOB_KEY, self.job)

    async def discard(self) -> None:
        """Delete the snapshot, the job record and the mesh file."""
        self.job = None
        self.policy.reset()
        if self.store_error is None:
            try:
                self.store.clear()
            except OSError as e:
                logging.warning("HelixPrint: PLR could not clear the snapshot: %s", e)
        self._remove_mesh_cfg()
        await self._ns_delete(JOB_KEY)
