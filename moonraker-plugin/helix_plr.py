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

import os
from pathlib import Path
from typing import Any, Dict, NamedTuple, Optional, Tuple

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
# The Moonraker-facing component
# ============================================================================

PLR_FEATURE = "plr"
PLR_FEATURE_VERSION = 1

# The app's settings namespace, where plr_profile and plr_user live.
HELIX_SCREEN_NAMESPACE = "helix-screen"


class HelixPlr:
    """PLR inside a running Moonraker. helix_print.py creates one in its
    component_init, after its database and Klipper APIs are resolved, and
    reaches PLR only through this class."""

    def __init__(self, helix: Any, data_dir: Path, sys_root: Path = Path("/sys")) -> None:
        self.helix = helix
        self.server = helix.server
        self.database = helix.database
        self.data_dir = data_dir
        self.sys_root = sys_root
        self.storage = StorageVerdict("unknown", False, "none", None)
        self.runtime_storage = ("unknown", None)  # type: Tuple[str, Optional[str]]
        self.has_virtual_sdcard = True
        self.store_error = None  # type: Optional[str]

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
