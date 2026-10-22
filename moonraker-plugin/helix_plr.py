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
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Tuple

# helix_print.py loads its siblings into sys.modules in dependency order.
import helix_gcode_reader
import helix_plr_render
import helix_plr_scan

try:
    import helix_plr_qidi
except ImportError:  # the QIDI adapter is optional; generic PLR runs without it
    helix_plr_qidi = None

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
# Layer checkpoints (§ 6.2, O3: T1 failed, so they are in v1)
# ============================================================================

# The follower runs when the print has moved this far past it, or this long
# after its last run.
FOLLOW_MIN_BYTES = 64 * 1024
FOLLOW_INTERVAL_S = 10.0


class LayerFollower:
    """Builds the printing file's layer index (helix_gcode_reader's sidecar)
    up to file_position while the print runs, so a resume scan starts at a
    layer instead of byte 0. It reads only bytes Klipper has already read,
    under a DutyCycleLimiter. step() and finish() run on the follower thread.
    """

    def __init__(self, path: Path, index_path: str, identity: Dict[str, Any],
                 limiter: Optional[Any] = None) -> None:
        self.path = path
        self.index_path = index_path
        self.identity = identity
        self.limiter = limiter
        self.offset = 0
        self.state = None  # type: Optional[Any]
        self.complete = False
        self._loaded = False
        self._written = -1
        self._writer = None  # type: Optional[Any]

    def _load(self) -> None:
        self._loaded = True
        summary = helix_gcode_reader.read_layer_index(self.index_path, self.identity)
        if summary is not None:
            self.complete = summary.complete
            if summary.last is not None:
                self.offset = summary.last.offset
                self.state = summary.last.state
                self._written = summary.last.offset
        self._writer = helix_gcode_reader.LayerIndexWriter(self.index_path, self.identity)
        if self.limiter is None:
            self.limiter = helix_gcode_reader.DutyCycleLimiter()

    def _on_layer(self, layer: int, offset: int, state: Any) -> None:
        if offset > self._written:
            self._writer.append(layer, offset, state)
            self._written = offset

    def step(self, file_position: int) -> None:
        if not self._loaded:
            self._load()
        if self.complete or file_position <= self.offset:
            return
        with open(str(self.path), "rb") as f:
            result = helix_gcode_reader.scan_to_offset(
                f, file_position, self.offset, self.state,
                throttle=self.limiter.pace, on_layer_start=self._on_layer,
            )
        self.offset = result.offset
        self.state = result.state_at_head

    def finish(self, file_size: Optional[int]) -> None:
        """Close the index. A print that completed (file_size given) and was
        followed to the end marks it complete."""
        if self._writer is None:
            return
        if not self.complete and file_size is not None and self.offset >= file_size:
            self._writer.finish(self.offset)
            self.complete = True
        self._writer.close()


# ============================================================================
# Offer rules (§ 8)
# ============================================================================

# With no layer count, the offer waits for this much print time instead of layer 2 (O5).
OFFER_MIN_PRINT_S = 120.0

# A bed this far under its target when the plan is made has cooled long enough
# that adhesion is in doubt: the plan asks for accept_cold_bed (O6).
COLD_BED_DIFF_C = 10.0


class OfferInputs(NamedTuple):
    newest_job_status: Optional[str]  # Moonraker history
    snapshot_job_id: Optional[str]
    newest_job_id: Optional[str]
    identity_ok: bool
    layer: Optional[int]
    tier: int
    klippy_ready: bool
    print_duration: Optional[float] = None


class OfferVerdict(NamedTuple):
    available: bool
    blocked_by: List[str]

    def to_dict(self) -> Dict[str, Any]:
        return {"available": self.available, "blocked_by": list(self.blocked_by)}


def offer_verdict(i: OfferInputs) -> OfferVerdict:
    """Every rule of § 8; blocked_by lists each one that fails."""
    blocked = []
    if i.newest_job_status != "interrupted":
        blocked.append("not_interrupted")
    if i.snapshot_job_id is None or i.snapshot_job_id != i.newest_job_id:
        blocked.append("no_snapshot")
    if not i.identity_ok:
        blocked.append("file_changed")
    if i.layer is not None:
        if i.layer < 2:
            blocked.append("too_early")
    elif (i.print_duration or 0.0) < OFFER_MIN_PRINT_S:
        blocked.append("too_early")
    if i.tier < 1:
        blocked.append("not_supported")
    if not i.klippy_ready:
        blocked.append("klippy_not_ready")
    return OfferVerdict(not blocked, blocked)


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
        self.follower = None  # type: Optional[LayerFollower]
        self._follow_future = None  # type: Optional[Any]
        self._follow_at = float("-inf")
        self._follow_executor = None  # type: Optional[Any]
        self.qidi = None  # type: Optional[Any]
        self.klippy_ready = False
        self.homing = HomingCheck(True, None)
        self.axis_min = None  # type: Optional[list]
        self.axis_max = None  # type: Optional[list]
        self._plan = None  # type: Optional[Any]
        self._plan_seq = None  # type: Optional[int]
        self._plan_executor = None  # type: Optional[Any]
        self._pending = None  # type: Optional[Any]
        self._pending_id = None  # type: Optional[str]
        self._sequence = None  # type: Optional[str]
        self._abort = False
        self._bed_now = None  # type: Optional[float]
        self._plan_file_size = None  # type: Optional[int]
        self._resume_history_name = None  # type: Optional[str]
        self._resume_job_id = None  # type: Optional[str]

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
        self.server.register_event_handler("server:klippy_shutdown", self.on_klippy_gone)
        self.server.register_event_handler("server:klippy_disconnect", self.on_klippy_gone)
        self.register_endpoints()

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
        self.klippy_ready = True
        try:
            await self._refresh_homing()
        except Exception as e:
            logging.warning("HelixPrint: PLR could not read the homing config: %s", e)
        try:
            status = await apis.subscribe_objects(snapshot_objects(objects))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not subscribe: %s", e)
            return
        self.status = {}
        self._merge(status)
        await self._detect_qidi(apis)

    async def _detect_qidi(self, apis: Any) -> None:
        """Select the QIDI print_state adapter when Klipper is QIDI's fork AND
        this Moonraker carries QIDI's resume endpoint. snapshot_objects
        subscribes gcode_move and virtual_sdcard whole, so the adapter's
        EXTRA_STATUS_FIELDS arrive with every update."""
        self.qidi = None
        if helix_plr_qidi is None:
            return
        gcode_help = {}  # type: Any
        send = getattr(apis, "_send_klippy_request", None)
        if send is not None:
            try:
                gcode_help = await send("gcode/help", {}, {})
            except Exception:
                gcode_help = {}
        if helix_plr_qidi.qidi_print_state_present(gcode_help, self.status.get("gcode_move")) \
                and helix_plr_qidi.moonraker_resumes_print_state(apis, self.database):
            self.qidi = helix_plr_qidi.QidiPrintStateAdapter(
                self.database, apis, mark=self.helix.mark_helix_initiated,
                unmark=self.helix.unmark_helix_initiated)
            logging.info("HelixPrint: PLR resumes through QIDI's print_state")

    def _qidi_status(self) -> Dict[str, Any]:
        fields = helix_plr_qidi.EXTRA_STATUS_FIELDS if helix_plr_qidi is not None else {}
        out = {}
        for obj, names in fields.items():
            have = _obj(self.status, obj)
            out[obj] = {n: have[n] for n in names if n in have}
        return out

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
        await self._note_print_path()
        record = build_record(self.status, now, self._macro_vars)
        reason = self.policy.decide(now, state, record)
        if reason is None:
            return
        await self._persist(now, record)
        await self._maybe_save_mesh(record)
        if self.qidi is not None:
            await self.qidi.on_snapshot(record, self._qidi_status(),
                                        force=reason in ("event", "pause"))
        self._maybe_follow(now, record.get("file_position"))

    async def _note_print_path(self) -> None:
        """Record the file Klipper is reading (virtual_sdcard.file_path).
        file_position counts its bytes, and on some firmware it is a copy (a
        cache file, or a plate extracted from a .3mf), not the gcodes file."""
        job = self.job
        path = _obj(self.status, "virtual_sdcard").get("file_path")
        if job is None or "print_path" in job or not isinstance(path, str) or not path:
            return
        try:
            identity = file_identity(Path(path))
        except OSError:
            return
        job["print_path"] = path
        job["print_identity"] = identity
        await self._ns_put(JOB_KEY, job)

    def print_target(self) -> Optional[Tuple[Path, Optional[str], Dict[str, Any]]]:
        """(file to scan, its layer index, the identity that index is for):
        the file Klipper read when known, else the job's file under gcodes."""
        gc_path = getattr(self.helix, "gc_path", None)
        job = self.job
        if gc_path is None or job is None:
            return None
        printed = job.get("print_path")
        if printed and job.get("print_identity") and Path(printed).exists():
            path = Path(printed).resolve()
            identity = job["print_identity"]
        elif job.get("file_hash"):
            relpath = _resolve_link(gc_path, job.get("filename", ""))
            if relpath is None:
                return None
            path = gc_path / relpath
            identity = job
        else:
            return None
        try:
            rel = str(path.relative_to(gc_path.resolve()))
        except ValueError:
            rel = str(Path(EXTERNAL_INDEX_DIR) / path.name)
        return path, helix_gcode_reader.layer_index_path(gc_path, rel), identity

    # -- layer checkpoints ---------------------------------------------------

    def _make_follower(self) -> Optional[LayerFollower]:
        target = self.print_target()
        if target is None:
            return None
        path, index_path, identity = target
        return LayerFollower(path, index_path, identity)

    def _executor(self) -> Any:
        if self._follow_executor is None:
            from concurrent.futures import ThreadPoolExecutor
            self._follow_executor = ThreadPoolExecutor(
                max_workers=1, thread_name_prefix="helix-plr-follower",
                initializer=helix_gcode_reader.lower_thread_priority)
        return self._follow_executor

    def _maybe_follow(self, now: float, file_position: Any) -> None:
        """Start a follower step when the print has moved far enough or long
        enough past the last one. One step runs at a time."""
        if not isinstance(file_position, int):
            return
        if self._follow_future is not None and not self._follow_future.done():
            return
        if self.follower is None:
            self.follower = self._make_follower()
            if self.follower is None:
                return
        behind = file_position - self.follower.offset
        if behind <= 0 or (behind < FOLLOW_MIN_BYTES and now - self._follow_at < FOLLOW_INTERVAL_S):
            return
        self._follow_at = now
        future = self._executor().submit(self.follower.step, file_position)
        future.add_done_callback(self._follow_done)
        self._follow_future = future

    @staticmethod
    def _follow_done(future: Any) -> None:
        if not future.cancelled() and future.exception() is not None:
            logging.warning("HelixPrint: PLR layer follower failed: %s", future.exception())

    async def _stop_follower(self, file_size: Optional[int]) -> None:
        follower = self.follower
        self.follower = None
        if follower is None:
            return
        import asyncio
        if self._follow_future is not None:
            try:
                await asyncio.wrap_future(self._follow_future)
            except Exception:
                pass
        self._follow_future = None
        try:
            await asyncio.wrap_future(self._executor().submit(follower.finish, file_size))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not close the layer index: %s", e)

    def close(self) -> None:
        for executor in (self._follow_executor, self._plan_executor):
            if executor is not None:
                executor.shutdown(wait=False)
        self.store.close()

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
        self.job["mesh"] = mesh
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
        await self._stop_follower(None)
        self._follow_at = self.clock()
        self.policy.reset()
        self._switch = (False, float("-inf"))
        if self.store_error is None:
            try:
                self.store.clear()
            except OSError as e:
                logging.warning("HelixPrint: PLR could not clear the snapshot: %s", e)
        self._remove_mesh_cfg()
        if self.qidi is not None:
            self.qidi.on_job_start()
        record = {"schema": SNAPSHOT_SCHEMA, "job_id": job_id, "filename": filename,
                  "started_at": self.clock()}
        if filename == RESUME_LINK and self._resume_history_name:
            record["history_name"] = self._resume_history_name
            self._resume_job_id = job_id
        gc_path = getattr(self.helix, "gc_path", None)
        if gc_path is not None and filename:
            try:
                record.update(file_identity(gc_path / filename))
            except OSError as e:
                logging.warning("HelixPrint: PLR could not read %s: %s", filename, e)
        self.job = record
        await self._ns_put(JOB_KEY, record)

    async def _job_finished(self, job: Dict[str, Any]) -> None:
        if self._resume_job_id is not None and str(job["job_id"]) == self._resume_job_id:
            await self._finish_resume_link(job)
        if self.job is None or str(job["job_id"]) != self.job.get("job_id"):
            return
        status = job.get("status")
        await self._stop_follower(
            self.job.get("file_size") if status == "completed" else None)
        if status in _CLEARED_STATUSES:
            if self.qidi is not None:
                self.qidi.on_job_end()
            await self.discard()
            return
        self.job["finished_status"] = status
        await self._ns_put(JOB_KEY, self.job)

    async def _finish_resume_link(self, job: Dict[str, Any]) -> None:
        """History names a resumed job after RESUME_LINK; name it after the
        file instead, and remove the link once the job is over."""
        import types
        info = types.SimpleNamespace(
            job_id=self._resume_job_id, original_filename=self._resume_history_name,
            temp_filename=RESUME_LINK, symlink_filename=RESUME_LINK,
            modifications=["plr_resume"])
        self._resume_job_id = None
        patch = getattr(self.helix, "_patch_history_entry", None)
        if patch is not None and info.original_filename:
            await patch(info, job.get("status", ""))
        if job.get("status") in _CLEARED_STATUSES:
            gc_path = getattr(self.helix, "gc_path", None)
            if gc_path is not None and (gc_path / RESUME_LINK).is_symlink():
                try:
                    (gc_path / RESUME_LINK).unlink()
                except OSError:
                    pass

    async def discard(self) -> None:
        """Delete the snapshot, the job record and the mesh file. The layer
        index stays: it describes the file, not the job."""
        await self._stop_follower(None)
        self.job = None
        self.policy.reset()
        if self.store_error is None:
            try:
                self.store.clear()
            except OSError as e:
                logging.warning("HelixPrint: PLR could not clear the snapshot: %s", e)
        self._remove_mesh_cfg()
        await self._ns_delete(JOB_KEY)

    # =========================================================================
    # Resume: offer, plan, prepare, commit (§ 6.3, § 8, § 9)
    # =========================================================================

    def register_endpoints(self) -> None:
        for path, methods, handler in (
            ("/server/helix/plr/status", ["GET"], self.handle_status),
            ("/server/helix/plr/plan", ["POST"], self.handle_plan),
            ("/server/helix/plr/prepare", ["POST"], self.handle_prepare),
            ("/server/helix/plr/commit", ["POST"], self.handle_commit),
            ("/server/helix/plr/abort", ["POST"], self.handle_abort),
            ("/server/helix/plr/discard", ["POST"], self.handle_discard),
        ):
            self.server.register_endpoint(path, methods, handler)
        register = getattr(self.server, "register_notification", None)
        if register is not None:
            register(PROGRESS_EVENT)
            register(PLAN_READY_EVENT)

    def _error(self, code: str, message: str, status: int = 400) -> Exception:
        error = self.server.error("%s: %s" % (code, message), status)
        error.plr_code = code
        return error

    def _notify(self, event: str, data: Dict[str, Any]) -> None:
        send = getattr(self.server, "send_event", None)
        if send is not None:
            send(event, data)

    def on_klippy_gone(self, *args: Any) -> None:
        self.klippy_ready = False

    async def _query(self, objects: Dict[str, Any]) -> Dict[str, Any]:
        apis = self.helix.klippy_apis
        if apis is None:
            return {}
        try:
            result = await apis.query_objects(objects)
        except Exception as e:
            raise self._error("plr_klippy_error", str(e), 503)
        return result if isinstance(result, dict) else {}

    async def _refresh_homing(self) -> None:
        status = await self._query({"configfile": ["settings"],
                                    "toolhead": ["axis_minimum", "axis_maximum"]})
        settings = _obj(status, "configfile").get("settings")
        kinematics = ""
        if isinstance(settings, dict) and isinstance(settings.get("printer"), dict):
            kinematics = str(settings["printer"].get("kinematics", ""))
        self.homing = check_xy_homing(settings, kinematics)
        th = _obj(status, "toolhead")
        self.axis_min = _vec(th.get("axis_minimum"), 3)
        self.axis_max = _vec(th.get("axis_maximum"), 3)

    async def _newest_job_status(self, newest: Optional[Dict[str, Any]]) -> Optional[str]:
        if not newest:
            return None
        job_id = newest.get("job_id")
        history = getattr(self.helix, "history", None)
        if history is not None and hasattr(history, "get_job"):
            try:
                job = await history.get_job(job_id)
            except Exception:
                job = None
            if isinstance(job, dict) and job.get("status"):
                return job["status"]
        if self.job is not None and self.job.get("job_id") == job_id \
                and self.job.get("finished_status"):
            return self.job["finished_status"]
        state = _obj(self.status, "print_stats").get("state")
        return "in_progress" if state in ("printing", "paused") else "interrupted"

    def _identity_ok(self) -> bool:
        job = self.job
        gc_path = getattr(self.helix, "gc_path", None)
        if job is None or gc_path is None or not job.get("file_hash"):
            return False
        try:
            now = file_identity(gc_path / job.get("filename", ""))
        except OSError:
            return False
        return all(now[k] == job.get(k) for k in ("file_size", "file_mtime", "file_hash"))

    def backend(self, profile: PlrProfile) -> Tuple[str, int]:
        """(backend, tier) PLR acts with. On QIDI's fork with QIDI's resume
        endpoint the adapter resumes, at tier 2: QIDI's sequence declares the
        snapshot's Z itself. A printer the profile does not list stays at 0."""
        if profile.backend == "vendor":
            return "none", 0
        if self.qidi is not None:
            listed = profile.backend == "qidi_print_state" or profile.tier >= 1
            return ("qidi_print_state", 2) if listed else ("none", 0)
        if profile.backend != "generic":
            return "none", 0
        tier, _ = effective_tier(profile, self.homing)
        return ("generic" if tier >= 1 else "none"), tier

    async def _offer(self, profile: PlrProfile,
                     snapshot: Optional[Dict[str, Any]]) -> Tuple[OfferVerdict, int]:
        _, tier = self.backend(profile)
        newest = await self._ns_get(NEWEST_JOB_KEY)
        inputs = OfferInputs(
            newest_job_status=await self._newest_job_status(newest),
            snapshot_job_id=self.job.get("job_id") if (self.job and snapshot) else None,
            newest_job_id=newest.get("job_id") if isinstance(newest, dict) else None,
            identity_ok=self._identity_ok(),
            layer=snapshot.get("layer") if snapshot else None,
            tier=tier,
            klippy_ready=self.klippy_ready,
            print_duration=snapshot.get("print_duration") if snapshot else None,
        )
        verdict = offer_verdict(inputs)
        if self.qidi is not None and tier >= 1 and "no_snapshot" not in verdict.blocked_by \
                and not await self.qidi.has_resume_rows():
            # § 10: QIDI resumes from its print_state rows, so they must exist too.
            blocked = [b for b in verdict.blocked_by] + ["no_snapshot"]
            order = ["not_interrupted", "no_snapshot", "file_changed", "too_early",
                     "not_supported", "klippy_not_ready"]
            verdict = OfferVerdict(False, sorted(blocked, key=order.index))
        return verdict, tier

    def _load_snapshot(self) -> Optional[Dict[str, Any]]:
        if self.store_error is not None:
            return None
        try:
            return self.store.load()
        except OSError:
            return None

    async def handle_status(self, web_request: Any) -> Dict[str, Any]:
        profile = await self.profile()
        snapshot = self._load_snapshot()
        verdict, tier = await self._offer(profile, snapshot)
        snap = None
        if snapshot is not None and self.job is not None:
            live = snapshot.get("live_pos") or [None, None, None]
            saved_at = snapshot.get("saved_at")
            snap = {
                "job_id": self.job.get("job_id"),
                "filename": self.job.get("history_name") or self.job.get("filename"),
                "layer": snapshot.get("layer"),
                "z": live[2],
                "saved_at": saved_at,
                "age_s": (self.clock() - saved_at) if isinstance(saved_at, (int, float)) else None,
            }
        backend, _ = self.backend(profile)
        return {
            "saving": await self.saving_enabled() and self.availability()[0],
            "storage": (await self.verdict(profile)).to_dict(),
            "tier": tier,
            "backend": backend,
            "snapshot": snap,
            "offer": verdict.to_dict(),
        }

    # -- plan ----------------------------------------------------------------

    def _plan_submit(self, fn: Callable[..., Any], *args: Any) -> Any:
        if self._plan_executor is None:
            from concurrent.futures import ThreadPoolExecutor
            self._plan_executor = ThreadPoolExecutor(
                max_workers=1, thread_name_prefix="helix-plr-plan")
        return self._plan_executor.submit(fn, *args)

    async def handle_plan(self, web_request: Any) -> Dict[str, Any]:
        if self._sequence in ("preparing", "committing"):
            raise self._error("plr_busy", "a resume is running", 409)
        snapshot = self._load_snapshot()
        profile = await self.profile()
        if self._plan is not None and snapshot is not None \
                and self._plan_seq == snapshot.get("seq") and self._sequence is None:
            return self._plan_reply(self._plan)
        if self._pending is not None and not self._pending.done():
            return {"pending": True, "plan_id": self._pending_id}
        await self._refresh_homing()
        verdict, tier = await self._offer(profile, snapshot)
        if not verdict.available:
            raise self._error("plr_no_plan", ",".join(verdict.blocked_by))
        state = _obj(await self._query({"print_stats": ["state"]}), "print_stats").get("state")
        if state in ("printing", "paused"):
            raise self._error("plr_busy", "printer is %s" % state, 409)
        self._plan = None
        self._sequence = None
        plan_id = "%s-%d-%d" % (self.job["job_id"], snapshot["seq"], int(self.clock() * 1000))
        target = self.print_target()
        if target is None:
            raise self._error("plr_scan_failed", "the printed file is gone", 400)
        path, index_path, identity = target
        import asyncio
        future = self._plan_submit(
            _scan_and_locate, path, snapshot["file_position"], index_path,
            identity, snapshot, profile.resume_skip_fraction)
        self._pending = future
        self._pending_id = plan_id
        try:
            scan, point = await asyncio.wait_for(asyncio.shield(asyncio.wrap_future(future)),
                                                 PLAN_SYNC_WAIT_S)
        except asyncio.TimeoutError:
            async def finish() -> None:
                try:
                    found = await asyncio.wrap_future(future)
                    await self._finish_plan(plan_id, snapshot, profile, tier, *found)
                except Exception as e:
                    logging.warning("HelixPrint: PLR plan %s failed: %s", plan_id, e)
                self._notify(PLAN_READY_EVENT, {"plan_id": plan_id})
            asyncio.ensure_future(finish())
            return {"pending": True, "plan_id": plan_id}
        except Exception as e:
            self._pending = None
            raise self._error("plr_scan_failed", str(e), 500)
        self._pending = None
        return self._plan_reply(await self._finish_plan(plan_id, snapshot, profile, tier, scan, point))

    async def _finish_plan(self, plan_id: str, snapshot: Dict[str, Any], profile: PlrProfile,
                           tier: int, scan: Any, point: Any) -> Any:
        if point is None:
            raise self._error("plr_scan_failed", "no resume point and no layer start", 500)
        snap_tool = snapshot.get("active_extruder")
        if (snap_tool not in (None, "extruder")) or (point.state.active_tool not in (None, 0)):
            raise self._error("plr_scan_failed", "multi_material: active tool %s / %s"
                              % (snap_tool, point.state.active_tool), 400)
        status = await self._query({"heater_bed": ["temperature", "target"],
                                    "bed_mesh": ["profiles"]})
        job = self.job or {}
        warnings = list(point.warnings)
        qidi = self.qidi is not None
        mesh = job.get("mesh")
        profiles = _obj(status, "bed_mesh").get("profiles")
        profiles = profiles if isinstance(profiles, dict) else {}
        mesh_loadable = None
        # QIDI's own sequence loads the mesh and purges; only the generic one does here.
        if isinstance(mesh, dict) and not qidi:
            if mesh.get("saved") and mesh.get("profile") in profiles:
                mesh_loadable = mesh["profile"]
            elif MESH_SECTION in profiles:
                mesh_loadable = MESH_SECTION
            else:
                warnings.append("mesh_unavailable")
        heaters = snapshot.get("heaters") or {}
        bed_target = (heaters.get("heater_bed") or {}).get("target") or 0.0
        nozzle = (heaters.get("extruder") or {}).get("target") or 0.0
        nozzle = max(nozzle, point.state.heater_targets.get("extruder", 0.0))
        if nozzle <= 0.0:
            raise self._error("plr_scan_failed", "no nozzle temperature to resume at", 400)
        chamber = None
        for name, heater in sorted(heaters.items()):
            if name.startswith("heater_generic ") and "chamber" in name \
                    and (heater or {}).get("target"):
                chamber = (name.split(" ", 1)[1], heater["target"])
                break
        objects = snapshot.get("objects")
        if objects is None or "objects" in (snapshot.get("overflow") or []):
            objects = (job.get("overflow") or {}).get("objects") or []
        macro_vars = snapshot.get("macro_vars")
        if "macro_vars" in (snapshot.get("overflow") or []):
            macro_vars = (job.get("overflow") or {}).get("macro_vars")
        purge = None
        if not profile.purge_gcode and not qidi:
            if self.axis_min and self.axis_max:
                purge = helix_plr_render.generic_purge_xy(
                    objects, tuple(self.axis_min[:2]), tuple(self.axis_max[:2]))
            if purge is None:
                warnings.append("no_purge_location")
        live = snapshot.get("live_pos") or [0.0, 0.0, 0.0, 0.0]
        bed_now = _num(_obj(status, "heater_bed").get("temperature"))
        bed_cold = bool(bed_target and bed_now is not None and bed_now < bed_target - COLD_BED_DIFF_C)
        if bed_cold:
            warnings.append("cold_bed")
        filename = job.get("filename", "")
        start_name, history_name = self._start_name(filename)
        plan = helix_plr_render.ResumePlan(
            plan_id=plan_id, filename=job.get("history_name") or filename,
            start_name=start_name, history_name=history_name,
            resume_offset=point.offset, state=point.state,
            layer=snapshot.get("layer"), total_layers=snapshot.get("total_layers"),
            tier=tier, z_physical=live[2],
            lift_mm=helix_plr_render.lift_height(
                profile.z_lift_mm, live[2], self.axis_max[2] if self.axis_max else None),
            snapshot=snapshot, object_defines=list(scan.object_defines),
            excluded=list(snapshot.get("excluded") or []), mesh=mesh,
            mesh_loadable=mesh_loadable, purge_xy=purge, nozzle_target=nozzle,
            bed_target=bed_target, chamber=chamber, macro_vars=macro_vars or {},
            method=point.method, retrace_estimate_mm=point.retrace_estimate_mm,
            skip_estimate_mm=point.skip_estimate_mm, warnings=warnings,
            requires=helix_plr_render.required_confirmations(tier, warnings, bed_cold),
        )
        self._plan = plan
        self._plan_seq = snapshot.get("seq")
        target = self.print_target()
        self._plan_file_size = os.stat(str(target[0])).st_size if target else None
        self._bed_now = bed_now
        return plan

    def _start_name(self, filename: str) -> Tuple[str, Optional[str]]:
        """M23 finds files at the gcodes root only; a file elsewhere, or one
        whose name M23 would cut, starts through RESUME_LINK."""
        if "/" not in filename and _m23_safe(filename):
            return filename, (self.job or {}).get("history_name")
        return RESUME_LINK, (self.job or {}).get("history_name") or filename

    def _plan_reply(self, plan: Any) -> Dict[str, Any]:
        _, _, z_gcode, _ = plan.state.gcode_position()
        return {
            "plan_id": plan.plan_id,
            "tier": plan.tier,
            "filename": plan.filename,
            "method": plan.method,
            "resume_offset": plan.resume_offset,
            "layer": plan.layer,
            "z_gcode": z_gcode,
            "z_physical": plan.z_physical,
            "retrace_estimate_mm": plan.retrace_estimate_mm,
            "bed": {"target": plan.bed_target, "current": self._bed_now},
            "restores": helix_plr_render.restores_summary(plan),
            "warnings": list(plan.warnings),
            "requires": list(plan.requires),
        }

    def _current_plan(self, plan_id: str) -> Any:
        plan = self._plan
        if plan is None:
            raise self._error("plr_no_plan", "no plan; request one first")
        snapshot = self._load_snapshot()
        if plan_id != plan.plan_id or snapshot is None or snapshot.get("seq") != self._plan_seq:
            raise self._error("plr_stale_plan", "plan %s is not the latest" % plan_id, 409)
        return plan

    # -- prepare / commit ----------------------------------------------------

    async def _run_steps(self, plan_id: str, steps: List[Tuple[str, List[str]]]) -> None:
        apis = self.helix.klippy_apis
        for step, lines in steps:
            if self._abort:
                raise self._error("plr_klippy_error", "aborted", 409)
            self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "running"})
            try:
                if lines:
                    await apis.run_gcode("\n".join(lines))
            except Exception as e:
                self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "failed"})
                raise self._error("plr_klippy_error", str(e), 500)
            self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "done"})

    async def _heaters_off(self, plan: Any) -> None:
        lines = ["M104 S0", "M140 S0"]
        if plan is not None and plan.chamber is not None:
            lines.append("SET_HEATER_TEMPERATURE HEATER=%s TARGET=0" % plan.chamber[0])
        try:
            await self.helix.klippy_apis.run_gcode("\n".join(lines))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not turn the heaters off: %s", e)

    async def _check_ready(self) -> None:
        if not self.klippy_ready:
            raise self._error("plr_klippy_error", "Klipper is not ready", 503)
        state = _obj(await self._query({"print_stats": ["state"]}), "print_stats").get("state")
        if state in ("printing", "paused"):
            raise self._error("plr_busy", "printer is %s" % state, 409)

    async def handle_prepare(self, web_request: Any) -> Dict[str, Any]:
        plan_id = _param(web_request, "plan_id", "")
        skip_chamber = bool(_param(web_request, "skip_chamber_wait", False))
        if self._sequence in ("preparing", "committing"):
            raise self._error("plr_busy", "a resume is running", 409)
        plan = self._current_plan(plan_id)
        if plan.tier < 1:
            raise self._error("plr_no_plan", "not_supported")
        await self._check_ready()
        profile = await self.profile()
        self._sequence = "preparing"
        self._abort = False
        try:
            if self.qidi is not None:
                await self.qidi.prepare(self._heat_wait(plan, skip_chamber))
            else:
                await self._run_steps(plan_id, helix_plr_render.render_prepare_script(
                    plan, profile, skip_chamber))
        except Exception:
            self._sequence = "failed"
            await self._heaters_off(plan)
            raise
        self._sequence = "prepared"
        return {"ok": True}

    async def handle_commit(self, web_request: Any) -> Dict[str, Any]:
        plan_id = _param(web_request, "plan_id", "")
        given = _param(web_request, "confirmations", {})
        z_reference = _param(web_request, "z_reference", "")
        plan = self._current_plan(plan_id)
        if self._sequence != "prepared":
            raise self._error("plr_busy", "prepare has not finished", 409)
        missing = [c for c in plan.requires if not (isinstance(given, dict) and given.get(c) is True)]
        if missing:
            raise self._error("plr_missing_confirmation", ",".join(missing))
        expected = "part_top" if plan.tier == 1 else "trusted"
        if z_reference != expected:
            raise self._error("plr_missing_confirmation",
                              "z_reference must be %s at tier %d" % (expected, plan.tier))
        await self._check_ready()
        profile = await self.profile()
        self._sequence = "committing"
        self._abort = False
        try:
            if self.qidi is not None:
                await self._commit_qidi(plan)
            else:
                await self._run_steps(plan_id, helix_plr_render.render_commit_script(
                    plan, profile, z_reference))
                await self._start(plan)
        except Exception:
            self._sequence = "failed"
            await self._heaters_off(plan)
            raise
        self._sequence = "done"
        self._plan = None
        return {"ok": True, "job_started": True}

    def _heat_wait(self, plan: Any, skip_chamber: bool = False) -> Callable[[], Any]:
        """§ 6.3 step 2 as the coroutine the QIDI adapter awaits."""
        async def wait() -> None:
            await self._run_steps(plan.plan_id, helix_plr_render.render_heat_wait(plan, skip_chamber))
        return wait

    async def _commit_qidi(self, plan: Any) -> None:
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "running"})
        try:
            await self.qidi.commit(plan.snapshot, plan.resume_offset, self._plan_file_size,
                                   self._heat_wait(plan), plan.state, self._qidi_status())
        except Exception as e:
            self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "failed"})
            if getattr(e, "plr_code", None):
                raise
            raise self._error("plr_klippy_error", str(e), 500)
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "done"})

    async def _start(self, plan: Any) -> None:
        """Mark the job helix-initiated, then the single start command."""
        gc_path = self.helix.gc_path
        link = None
        if plan.start_name == RESUME_LINK:
            link = gc_path / RESUME_LINK
            target = (gc_path / (self.job or {}).get("filename", plan.filename)).resolve()
            try:
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(target)
            except OSError as e:
                raise self._error("plr_klippy_error", "cannot link %s: %s" % (RESUME_LINK, e), 500)
            self._resume_history_name = plan.history_name
        self.helix.mark_helix_initiated(plan.start_name, "plr_resume", plan.history_name)
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "running"})
        try:
            await self.helix.klippy_apis.run_gcode(helix_plr_render.render_start(plan))
        except Exception as e:
            self.helix.unmark_helix_initiated(plan.start_name)
            if link is not None:
                try:
                    link.unlink()
                except OSError:
                    pass
            self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "failed"})
            raise self._error("plr_klippy_error", str(e), 500)
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "done"})

    async def handle_abort(self, web_request: Any) -> Dict[str, Any]:
        plan_id = _param(web_request, "plan_id", "")
        plan = self._plan
        if plan is None or plan.plan_id != plan_id:
            raise self._error("plr_stale_plan", "plan %s is not the latest" % plan_id, 409)
        self._abort = True
        self._sequence = "aborted"
        if self.qidi is not None:
            self.qidi.abort()
        await self._heaters_off(plan)
        return {"ok": True}

    async def handle_discard(self, web_request: Any) -> Dict[str, Any]:
        if self._sequence in ("preparing", "committing"):
            raise self._error("plr_busy", "a resume is running", 409)
        self._plan = None
        self._sequence = None
        if self.qidi is not None:
            try:
                await self.qidi.discard()
            except Exception as e:
                raise self._error("plr_klippy_error", "print_state not deleted: %s" % e, 500)
        await self.discard()
        return {"ok": True}


# Notifications to clients: notify_plr_progress, notify_plr_plan_ready.
PROGRESS_EVENT = "helix_print:plr_progress"
PLAN_READY_EVENT = "helix_print:plr_plan_ready"

# A plan whose scan takes longer replies {"pending": true} and announces itself.
PLAN_SYNC_WAIT_S = 10.0

# The include-file mesh section (§ 6.4).
MESH_SECTION = "helix_plr"

# A root-level link M23 can select when the file is in a subfolder.
RESUME_LINK = "helix_plr_resume.gcode"

# Layer indexes of printed files outside the gcodes root (a firmware's cache
# copy), under <gcodes>/.helix_index/.
EXTERNAL_INDEX_DIR = ".external"


def _param(web_request: Any, key: str, default: Any) -> Any:
    getter = getattr(web_request, "get", None)
    if getter is not None:
        try:
            return getter(key, default)
        except Exception:
            return default
    return default


def _m23_safe(name: str) -> bool:
    return bool(name) and name == name.strip() and not name.startswith(".") \
        and not any(c in name for c in "#*;\n\r")


def _resolve_link(gc_path: Path, filename: str) -> Optional[str]:
    """The gcodes-relative path a (possibly linked) file really is."""
    try:
        real = (gc_path / filename).resolve()
        return str(real.relative_to(gc_path.resolve()))
    except (OSError, ValueError):
        return None


def _scan_and_locate(path: Path, file_position: int, index_path: Optional[str],
                     identity: Optional[Dict[str, Any]], snapshot: Dict[str, Any],
                     skip_fraction: float) -> Tuple[Any, Any]:
    """The resume scan, on the plan thread."""
    with open(str(path), "rb") as f:
        scan = helix_plr_scan.scan_for_resume(f, file_position, index_path, identity)
        point = helix_plr_scan.resume_point(f, scan, snapshot, skip_fraction, SAVE_INTERVAL_S)
    return scan, point
