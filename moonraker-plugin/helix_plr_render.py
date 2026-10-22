# SPDX-License-Identifier: GPL-3.0-or-later
"""
Power-loss recovery: the generic resume sequence as G-code (§ 6.3 of
docs/devel/plans/2026-10-21-plugin-power-loss-recovery-design.md).

The plugin renders and sends this sequence itself (ruling 5). It runs in two
phases around the app's confirmations:

- prepare: bed (and chamber) to temperature with the nozzle cold, declare the
  physical Z the snapshot measured, lift, home X and Y, and for tier 1 stop
  above the resume point so the user can jog down to the part.
- commit: settle the Z reference, restore the print's state, heat and purge
  the nozzle away from the part, return to the resume point, then the single
  start command (M23 / M26 / M24).

Pure: every function takes a ResumePlan and returns lines. Python 3.8, stdlib only.
"""

from __future__ import annotations

from typing import Any, Dict, List, NamedTuple, Optional, Sequence, Tuple

from helix_gcode_reader import ModalState

# Kept between the lifted nozzle and the top of the Z travel.
SAG_ALLOWANCE_MM = 1.0

# The least cold lift that clears the part before XY homing drags across it.
# Less room than this under the top of travel and PLR is not offered.
MIN_LIFT_MM = 2.0

# mm/min
Z_FEED = 300
TRAVEL_FEED = 6000
PURGE_FEED = 1200
RETRACT_FEED = 2100

# Generic purge: a line along a bed edge clear of every object.
PURGE_MARGIN_MM = 5.0
PURGE_LINE_MM = 60.0
PURGE_Z_MM = 0.3
PURGE_E_MM = 12.0
PURGE_RETRACT_MM = 0.8
# Primes the nozzle at the resume point after the purge's retract.
PRIME_E_MM = PURGE_RETRACT_MM
# Heat-up prime when no purge location is known: at the XY home position.
HOME_PRIME_E_MM = 5.0

CONFIRMATIONS = ("part_attached", "nozzle_clear", "z_confirmed", "z_jogged",
                 "accept_no_mesh", "accept_cold_bed")

# Klipper ends an extended command's arguments at any of these.
_UNSAFE_PARAM_CHARS = set("#*;\n\r\"")


class UnsafeValue(ValueError):
    """A value from a file or status that cannot be placed in a command."""


def gcode_param(value: Any) -> str:
    """A value as an extended-command parameter: quoted when it has spaces,
    refused when it carries a character Klipper's parser would cut at."""
    text = str(value)
    if not text or any(c in _UNSAFE_PARAM_CHARS for c in text):
        raise UnsafeValue("unsafe parameter %r" % text)
    return '"%s"' % text if any(c.isspace() for c in text) else text


def _f(value: float) -> str:
    text = "%.4f" % value
    text = text.rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


class ResumePlan(NamedTuple):
    plan_id: str
    filename: str  # gcodes-relative path of the file being resumed
    start_name: str  # what M23 selects: the file itself, or a root-level link to it
    history_name: Optional[str]  # name history should carry when start_name is a link
    resume_offset: int
    state: ModalState  # modal state just before the resume line
    layer: Optional[int]
    total_layers: Optional[int]
    tier: int
    z_physical: float  # live_pos.z at the snapshot: kinematic frame
    lift_mm: float
    snapshot: Dict[str, Any]
    object_defines: List[str]
    excluded: List[str]
    mesh: Optional[Dict[str, Any]]  # the job record's mesh
    mesh_loadable: Optional[str]  # profile BED_MESH_PROFILE LOAD can use, else None
    purge_xy: Optional[Tuple[float, float, float, float]]
    nozzle_target: float
    bed_target: float
    chamber: Optional[Tuple[str, float]]  # (heater name, target)
    macro_vars: Dict[str, Dict[str, Any]]
    method: str
    retrace_estimate_mm: float
    skip_estimate_mm: float
    warnings: List[str]
    requires: List[str]


# --------------------------------------------------------------------------
# Geometry helpers
# --------------------------------------------------------------------------


def lift_height(z_lift_mm: float, z_physical: float, position_max_z: Optional[float]) -> float:
    """The cold lift: the profile's height, kept under the top of travel."""
    if position_max_z is None:
        return max(0.0, z_lift_mm)
    return max(0.0, min(z_lift_mm, position_max_z - z_physical - SAG_ALLOWANCE_MM))


def _object_boxes(objects: Sequence[Any]) -> List[Tuple[float, float, float, float]]:
    boxes = []
    for obj in objects or ():
        if not isinstance(obj, dict):
            continue
        poly = obj.get("polygon")
        if isinstance(poly, list) and poly:
            xs = [p[0] for p in poly if isinstance(p, (list, tuple)) and len(p) >= 2]
            ys = [p[1] for p in poly if isinstance(p, (list, tuple)) and len(p) >= 2]
            if xs and ys:
                boxes.append((min(xs), min(ys), max(xs), max(ys)))
                continue
        center = obj.get("center")
        if isinstance(center, (list, tuple)) and len(center) >= 2:
            boxes.append((center[0], center[1], center[0], center[1]))
    return boxes


def generic_purge_xy(objects: List[dict], bed_min: Tuple[float, float],
                     bed_max: Tuple[float, float],
                     margin_mm: float = PURGE_MARGIN_MM) -> Optional[Tuple[float, float, float, float]]:
    """A purge line (x0, y0, x1, y1) along a bed edge, `margin_mm` from the
    edge and from every object's bounding box: front, back, left, right in
    that order. None when no edge is clear or no object is known (the part's
    footprint is then unknown)."""
    boxes = _object_boxes(objects)
    if not boxes:
        return None
    x0, y0 = bed_min[0] + margin_mm, bed_min[1] + margin_mm
    x1, y1 = bed_max[0] - margin_mm, bed_max[1] - margin_mm
    if x1 <= x0 or y1 <= y0:
        return None
    lx = min(PURGE_LINE_MM, x1 - x0)
    ly = min(PURGE_LINE_MM, y1 - y0)
    candidates = [
        (x0, y0, x0 + lx, y0),  # front
        (x0, y1, x0 + lx, y1),  # back
        (x0, y0, x0, y0 + ly),  # left
        (x1, y0, x1, y0 + ly),  # right
    ]
    for cx0, cy0, cx1, cy1 in candidates:
        lo_x, hi_x = min(cx0, cx1) - margin_mm, max(cx0, cx1) + margin_mm
        lo_y, hi_y = min(cy0, cy1) - margin_mm, max(cy0, cy1) + margin_mm
        if all(b[2] < lo_x or b[0] > hi_x or b[3] < lo_y or b[1] > hi_y for b in boxes):
            return (cx0, cy0, cx1, cy1)
    return None


def mesh_z_at(mesh: Optional[Dict[str, Any]], x: float, y: float) -> Optional[float]:
    """Bilinear height of the snapshot's probed mesh at (x, y), clamped to its
    bounds; None without a usable mesh."""
    if not isinstance(mesh, dict):
        return None
    points = mesh.get("points")
    lo, hi = mesh.get("min"), mesh.get("max")
    if not points or not lo or not hi or not isinstance(points[0], list):
        return None
    rows, cols = len(points), len(points[0])
    if rows < 1 or cols < 1:
        return None

    def axis(v, a, b, n):
        if n == 1 or b <= a:
            return 0, 0, 0.0
        t = (min(max(v, a), b) - a) / (b - a) * (n - 1)
        i = min(int(t), n - 2)
        return i, i + 1, t - i

    i0, i1, fx = axis(x, lo[0], hi[0], cols)
    j0, j1, fy = axis(y, lo[1], hi[1], rows)
    top = points[j0][i0] * (1 - fx) + points[j0][i1] * fx
    bottom = points[j1][i0] * (1 - fx) + points[j1][i1] * fx
    return top * (1 - fy) + bottom * fy


# --------------------------------------------------------------------------
# Scripts
# --------------------------------------------------------------------------


def _resume_xyz(state: ModalState) -> Tuple[float, float, float]:
    gx, gy, gz, _ = state.gcode_position()
    return gx, gy, gz


class HeatStep(NamedTuple):
    """§ 6.3 step 2, one heater: the set command, then the plugin waits for
    `sensor` (a Klipper object) to reach `minimum`. The wait is not a G-code
    (M190, TEMPERATURE_WAIT): those hold Klipper's G-code lock, so an abort's
    heaters-off could not run until the target was reached."""
    step: str
    lines: List[str]
    sensor: str
    minimum: float
    wait: bool


# A heater counts as at temperature this close under its target.
HEAT_TOLERANCE_C = 1.0


def render_heat_wait(plan: ResumePlan, skip_chamber_wait: bool = False) -> List[HeatStep]:
    """Bed, then chamber, to temperature; the nozzle stays cold. Every
    backend's prepare starts with it, and every commit repeats it."""
    steps = []  # type: List[HeatStep]
    if plan.bed_target > 0:
        steps.append(HeatStep("bed_wait", ["M140 S%s" % _f(plan.bed_target)], "heater_bed",
                              plan.bed_target - HEAT_TOLERANCE_C, True))
    if plan.chamber is not None:
        name, target = plan.chamber
        steps.append(HeatStep(
            "chamber_wait",
            ["SET_HEATER_TEMPERATURE HEATER=%s TARGET=%s" % (gcode_param(name), _f(target))],
            "heater_generic " + name, target - HEAT_TOLERANCE_C, not skip_chamber_wait))
    return steps


def render_prepare_script(plan: ResumePlan, profile: Any,
                          lifted_z: Optional[float] = None,
                          z_homed: bool = False) -> List[Tuple[str, List[str]]]:
    """The generic prepare's motion after the heat wait: declare the measured
    Z, lift cold, home XY (tier 1: stop above the resume point).

    `lifted_z` is set when an earlier prepare of this snapshot already lifted
    the nozzle: the lift is not repeated, and the Z declared is the lifted
    one, or nothing when Klipper still has Z homed (`z_homed`).
    SET_HOMED=Z leaves X and Y unhomed, so an XY homing macro that homes only
    unhomed axes still homes them."""
    steps = []  # type: List[Tuple[str, List[str]]]
    if lifted_z is None:
        lift = ["SET_KINEMATIC_POSITION Z=%s SET_HOMED=Z" % _f(plan.z_physical)]
        lift += ["G91", "G1 Z%s F%d" % (_f(plan.lift_mm), Z_FEED), "G90"]
        steps.append(("lift", lift))
    elif not z_homed:
        steps.append(("lift", ["SET_KINEMATIC_POSITION Z=%s SET_HOMED=Z" % _f(lifted_z)]))
    steps.append(("home_xy", [line for line in profile.xy_home_gcode.splitlines() if line.strip()]))
    if plan.tier == 1:
        x, y, _ = _resume_xyz(plan.state)
        steps.append(("position", ["G90", "G1 X%s Y%s F%d" % (_f(x), _f(y), TRAVEL_FEED)]))
    return steps


def _restore_lines(plan: ResumePlan) -> List[str]:
    snap = plan.snapshot
    out = []
    if plan.mesh_loadable:
        out.append("BED_MESH_PROFILE LOAD=%s" % gcode_param(plan.mesh_loadable))
    offset = snap.get("gcode_offset")
    if offset:
        out.append("SET_GCODE_OFFSET X=%s Y=%s Z=%s" % (_f(offset[0]), _f(offset[1]), _f(offset[2])))
    for extruder, pa in sorted((snap.get("pa") or {}).items()):
        parts = ["SET_PRESSURE_ADVANCE EXTRUDER=%s" % gcode_param(extruder)]
        if pa.get("advance") is not None:
            parts.append("ADVANCE=%s" % _f(pa["advance"]))
        if pa.get("smooth_time") is not None:
            parts.append("SMOOTH_TIME=%s" % _f(pa["smooth_time"]))
        if len(parts) > 1:
            out.append(" ".join(parts))
    limits = snap.get("limits") or {}
    parts = ["SET_VELOCITY_LIMIT"]
    for key, name in (("velocity", "VELOCITY"), ("accel", "ACCEL"),
                      ("scv", "SQUARE_CORNER_VELOCITY"), ("min_cruise_ratio", "MINIMUM_CRUISE_RATIO")):
        if limits.get(key) is not None:
            parts.append("%s=%s" % (name, _f(limits[key])))
    if len(parts) > 1:
        out.append(" ".join(parts))
    retraction = snap.get("retraction")
    if retraction:
        parts = ["SET_RETRACTION"]
        for key, name in (("length", "RETRACT_LENGTH"), ("speed", "RETRACT_SPEED"),
                          ("extra", "UNRETRACT_EXTRA_LENGTH"), ("unretract_speed", "UNRETRACT_SPEED")):
            if retraction.get(key) is not None:
                parts.append("%s=%s" % (name, _f(retraction[key])))
        if len(parts) > 1:
            out.append(" ".join(parts))
    if snap.get("speed_factor") is not None:
        out.append("M220 S%s" % _f(snap["speed_factor"] * 100.0))
    if snap.get("extrude_factor") is not None:
        out.append("M221 S%s" % _f(snap["extrude_factor"] * 100.0))
    for name, heater in sorted((snap.get("heaters") or {}).items()):
        target = heater.get("target") if isinstance(heater, dict) else None
        if not target:
            continue
        if plan.chamber is not None and name.endswith(" " + plan.chamber[0]):
            continue
        if name.startswith("temperature_fan "):
            out.append("SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=%s TARGET=%s"
                       % (gcode_param(name.split(" ", 1)[1]), _f(target)))
        elif name.startswith("heater_generic "):
            out.append("SET_HEATER_TEMPERATURE HEATER=%s TARGET=%s"
                       % (gcode_param(name.split(" ", 1)[1]), _f(target)))
    out.extend(plan.object_defines)
    for name in plan.excluded:
        out.append("EXCLUDE_OBJECT NAME=%s" % gcode_param(name))
    info = ["SET_PRINT_STATS_INFO"]
    if plan.total_layers is not None:
        info.append("TOTAL_LAYER=%d" % plan.total_layers)
    if plan.layer is not None:
        info.append("CURRENT_LAYER=%d" % plan.layer)
    if len(info) > 1:
        out.append(" ".join(info))
    for macro, values in sorted(plan.macro_vars.items()):
        for var, value in sorted(values.items()):
            out.append("SET_GCODE_VARIABLE MACRO=%s VARIABLE=%s VALUE=%s" % (
                gcode_param(macro), gcode_param(var), gcode_param(_macro_value(value))))
    return out


def _macro_value(value: Any) -> str:
    import json
    return json.dumps(value, separators=(",", ":")) if not isinstance(value, str) \
        else json.dumps(value)


def _safe_z(plan: ResumePlan) -> float:
    """The lifted height in the restored G-code frame."""
    return _resume_xyz(plan.state)[2] + plan.lift_mm


def _purge_lines(plan: ResumePlan, profile: Any) -> List[str]:
    target = _f(plan.nozzle_target)
    if profile.purge_gcode:
        return ["M109 S%s" % target] + [l for l in profile.purge_gcode.splitlines() if l.strip()]
    if plan.purge_xy is None:
        # No known part-free spot: heat and prime where XY homing left the
        # nozzle, lifted above the part.
        return ["M109 S%s" % target, "M83",
                "G1 E%s F%d" % (_f(HOME_PRIME_E_MM), PURGE_FEED // 4),
                "G1 E-%s F%d" % (_f(PURGE_RETRACT_MM), RETRACT_FEED)]
    x0, y0, x1, y1 = plan.purge_xy
    return [
        "G90",
        "G1 X%s Y%s F%d" % (_f(x0), _f(y0), TRAVEL_FEED),
        "G1 Z%s F%d" % (_f(PURGE_Z_MM), Z_FEED),
        "M109 S%s" % target,
        "M83",
        "G1 X%s Y%s E%s F%d" % (_f(x1), _f(y1), _f(PURGE_E_MM), PURGE_FEED),
        "G1 E-%s F%d" % (_f(PURGE_RETRACT_MM), RETRACT_FEED),
        "G1 X%s Y%s F%d" % (_f(x0), _f(y0), TRAVEL_FEED),
        "G1 Z%s F%d" % (_f(_safe_z(plan)), Z_FEED),
    ]


def render_commit_script(plan: ResumePlan, profile: Any,
                         z_reference: str) -> List[Tuple[str, List[str]]]:
    """Steps of the commit phase, up to but not including the start command.
    z_reference is "trusted" (tier 2: keep the frame prepare declared) or
    "part_top" (tier 1: the nozzle was jogged onto the part)."""
    steps = []  # type: List[Tuple[str, List[str]]]
    rx, ry, rz = _resume_xyz(plan.state)
    if z_reference == "part_top":
        offset_z = (plan.snapshot.get("gcode_offset") or [0.0, 0.0, 0.0])[2]
        mesh_z = mesh_z_at(plan.mesh, rx + (plan.snapshot.get("gcode_offset") or [0.0])[0],
                           ry + (plan.snapshot.get("gcode_offset") or [0.0, 0.0])[1]) or 0.0
        z = rz + offset_z + mesh_z
        lines = ["SET_KINEMATIC_POSITION Z=%s SET_HOMED=Z" % _f(z)]
        lines += ["G91", "G1 Z%s F%d" % (_f(plan.lift_mm), Z_FEED), "G90"]
        steps.append(("z_reference", lines))
    steps.append(("restore", _restore_lines(plan)))
    steps.append(("heat_nozzle", _purge_lines(plan, profile)))
    state = plan.state
    ret = ["G90",
           "G1 Z%s F%d" % (_f(_safe_z(plan)), Z_FEED),
           "G1 X%s Y%s F%d" % (_f(rx), _f(ry), TRAVEL_FEED),
           "G1 Z%s F%d" % (_f(rz), Z_FEED),
           "M83",
           "G1 E%s F%d" % (_f(PRIME_E_MM), RETRACT_FEED),
           "G90" if state.absolute_coord else "G91",
           "M82" if state.absolute_extrude else "M83",
           "G92 E%s" % _f(state.gcode_position()[3]),
           "G1 F%s" % _f(state.speed * 60.0)]
    for name, speed in sorted((plan.snapshot.get("fans") or {}).items()):
        if speed is None:
            continue
        if name == "fan":
            ret.append("M106 S%s" % _f(round(speed * 255.0, 1)))
        elif name.startswith("fan_generic "):
            ret.append("SET_FAN_SPEED FAN=%s SPEED=%s" % (gcode_param(name.split(" ", 1)[1]), _f(speed)))
    if state.retracted:
        ret.append("G10")
    steps.append(("move_to_resume", ret))
    return steps


def render_start(plan: ResumePlan) -> str:
    """The single start command: select, seek, start."""
    return "\n".join(["M23 %s" % plan.start_name, "M26 S%d" % plan.resume_offset, "M24"])


def required_confirmations(tier: int, warnings: Sequence[str], bed_cold: bool) -> List[str]:
    out = ["part_attached", "nozzle_clear", "z_jogged"] if tier == 1 \
        else ["part_attached", "z_confirmed"]
    if "mesh_unavailable" in warnings:
        out.append("accept_no_mesh")
    if bed_cold:
        out.append("accept_cold_bed")
    return out


def restores_summary(plan: ResumePlan) -> List[str]:
    """What commit restores, as short names for the app to list."""
    snap = plan.snapshot
    out = []
    if plan.mesh_loadable:
        out.append("bed_mesh")
    if snap.get("gcode_offset"):
        out.append("gcode_offset")
    if snap.get("pa"):
        out.append("pressure_advance")
    if any(v is not None for v in (snap.get("limits") or {}).values()):
        out.append("velocity_limits")
    if snap.get("retraction"):
        out.append("retraction")
    if snap.get("speed_factor") is not None:
        out.append("speed_factor")
    if snap.get("extrude_factor") is not None:
        out.append("extrude_factor")
    if plan.object_defines:
        out.append("objects")
    if plan.excluded:
        out.append("excluded_objects")
    if snap.get("fans"):
        out.append("fans")
    if plan.macro_vars:
        out.append("macro_vars")
    return out
