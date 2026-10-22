# SPDX-License-Identifier: GPL-3.0-or-later
"""
File-fits-printer check: the HelixPrint plugin's `fit_check_blocks_autostart`
feature.

One pure checker compares a file's analysis (helix_analysis' `bounds` and
`header`) with the printer's limits from Klipper's configfile.settings, and
gives findings {kind, severity, detail}:

    out_of_bounds           block  a move outside an axis' travel, or a delta's radius
    temp_over_max           block  a heater target above its max_temp
    too_many_tools          block  a tool number past the printer's last tool
    nozzle_mismatch         warn   the slicer's nozzle differs from the extruder's
    printer_model_mismatch  warn   the slicer's printer is not this printer

The screen runs the same checker (src/printer/fit_check.cpp) over the same
record, or over what it reads itself; tests/fixtures/fit_check/cases.json pins
both to one table.

When the user opts in, a slicer's "upload and print" of a plain G-code file,
or of a .gcode.3mf the converter extracts, is not started while a blocking
finding stands. The file stays uploaded. The screen sees no such start, so
this is the only place it can be refused.

Everything above FitCheck is pure and touches no Moonraker object.
Python 3.8, stdlib only.
"""

from __future__ import annotations

import logging
import math
import re
from typing import Any, Callable, Dict, List, Optional

import helix_file_queue as file_queue

FIT_CHECK_FEATURE = "fit_check_blocks_autostart"
FIT_CHECK_FEATURE_VERSION = 1

REFUSED_EVENT = "helix:fit_check_refused"

# How long an upload waits for its file's analysis before starting
# unchecked. The slicer's upload request is held open meanwhile.
RECORD_WAIT_S = 120.0

# Positions and temperatures within this of a limit are at it: the record
# rounds to 4 decimals, and a gcode step is 0.001 mm.
LIMIT_EPSILON = 1e-4

# Nozzle diameters this close are the same nozzle.
NOZZLE_TOLERANCE = 0.005

AXES = ("x", "y", "z")

KIND_OUT_OF_BOUNDS = "out_of_bounds"
KIND_TEMP_OVER_MAX = "temp_over_max"
KIND_TOO_MANY_TOOLS = "too_many_tools"
KIND_NOZZLE_MISMATCH = "nozzle_mismatch"
KIND_PRINTER_MODEL_MISMATCH = "printer_model_mismatch"

SEVERITY_BLOCK = "block"
SEVERITY_WARN = "warn"

_EXTRUDER = re.compile(r"^extruder(\d*)$")
_TOOL_MACRO = re.compile(r"^gcode_macro t(\d+)$")


# ============================================================================
# Printer limits
# ============================================================================


def _number(value: Any) -> Optional[float]:
    """A finite JSON number as float; anything else (bool included) is None."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    f = float(value)
    return f if math.isfinite(f) else None


def _section(settings: Dict[str, Any], name: str) -> Optional[Dict[str, Any]]:
    section = settings.get(name)
    return section if isinstance(section, dict) else None


def _option(section: Optional[Dict[str, Any]], key: str,
            default: Optional[float] = None) -> Optional[float]:
    """A numeric option: `default` when the section leaves it out, None when
    it holds something that is not a number."""
    if section is None:
        return None
    if key not in section:
        return default
    return _number(section[key])


def machine_limits(settings: Any) -> Dict[str, Any]:
    """The limits a file is checked against, from configfile.settings.

    A delta has a print radius and its height from the towers. Every other
    kinematics reads stepper_x / stepper_y / stepper_z where they exist, and a
    dual carriage widens the axis it moves on. `tool_count` counts the
    extruders and the T<n> macros the config defines; a filament system's
    own T commands are not in the config, so callers that may have one
    replace it. `model` is never in the config."""
    if not isinstance(settings, dict):
        settings = {}
    printer = _section(settings, "printer") or {}
    kinematics = printer.get("kinematics")
    kinematics = kinematics.lower() if isinstance(kinematics, str) else ""
    axis_min = [None, None, None]  # type: List[Optional[float]]
    axis_max = [None, None, None]  # type: List[Optional[float]]
    print_radius = None  # type: Optional[float]

    if kinematics == "delta":
        print_radius = _number(printer.get("print_radius"))
        if print_radius is None:
            print_radius = _number(printer.get("delta_radius"))
        axis_min[2] = _option(printer, "minimum_z_position", 0.0)
        endstops = [_option(_section(settings, "stepper_" + t), "position_endstop")
                    for t in ("a", "b", "c")]
        known = [e for e in endstops if e is not None]
        axis_max[2] = min(known) if known else None
    else:
        for i, axis in enumerate(AXES):
            stepper = _section(settings, "stepper_" + axis)
            axis_min[i] = _option(stepper, "position_min", 0.0)
            axis_max[i] = _option(stepper, "position_max")
        carriage = _section(settings, "dual_carriage")
        if carriage is not None:
            axis = carriage.get("axis", "x")
            i = AXES.index(axis) if axis in ("x", "y") else None
            low = _option(carriage, "position_min", 0.0)
            high = _option(carriage, "position_max")
            if i is not None:
                if low is not None and axis_min[i] is not None:
                    axis_min[i] = min(axis_min[i], low)
                if high is not None and axis_max[i] is not None:
                    axis_max[i] = max(axis_max[i], high)

    extruders = []  # type: List[tuple]
    tool_macros = []  # type: List[int]
    for name, section in settings.items():
        match = _EXTRUDER.match(name)
        if match and isinstance(section, dict):
            extruders.append((int(match.group(1) or 0), section))
            continue
        match = _TOOL_MACRO.match(name)
        if match:
            tool_macros.append(int(match.group(1)))
    extruders.sort(key=lambda e: e[0])
    max_temps = [t for t in (_number(s.get("max_temp")) for _i, s in extruders) if t is not None]
    nozzles = [n for n in (_number(s.get("nozzle_diameter")) for _i, s in extruders)
               if n is not None]
    tool_count = max(len(extruders), max(tool_macros) + 1 if tool_macros else 0)

    return {
        "kinematics": kinematics,
        "axis_min": axis_min,
        "axis_max": axis_max,
        "print_radius": print_radius,
        "max_extruder_temp": max(max_temps) if max_temps else None,
        "max_bed_temp": _option(_section(settings, "heater_bed"), "max_temp"),
        "tool_count": tool_count or None,
        "nozzle_diameters": nozzles,
        "model": "",
    }


# ============================================================================
# The file
# ============================================================================


def fit_file_from_record(record: Any) -> Dict[str, Any]:
    """The checker's file input from a helix_analysis record."""
    record = record if isinstance(record, dict) else {}
    bounds = record.get("bounds") if isinstance(record.get("bounds"), dict) else {}
    header = record.get("header") if isinstance(record.get("header"), dict) else {}
    tools = [t.get("tool") for t in record.get("tools") or [] if isinstance(t, dict)]
    return {
        "print": bounds.get("print"),
        "travel": bounds.get("travel"),
        "max_hotend_temp": bounds.get("max_hotend_temp"),
        "max_bed_temp": bounds.get("max_bed_temp"),
        "tools": sorted(t for t in tools if isinstance(t, int) and not isinstance(t, bool)),
        "nozzle_diameters": list(header.get("nozzle_diameters") or []),
        "printer_model": header.get("printer_model"),
    }


# ============================================================================
# The checker
# ============================================================================


def _finding(kind: str, severity: str, **detail: Any) -> Dict[str, Any]:
    return {"kind": kind, "severity": severity, "detail": detail}


def _box_axis(box: Any, end: str, i: int) -> Optional[float]:
    if not isinstance(box, dict):
        return None
    values = box.get(end)
    if not isinstance(values, list) or len(values) <= i:
        return None
    return _number(values[i])


def _model_key(name: Any) -> str:
    """A printer name compared without case, spaces or punctuation."""
    return "".join(c for c in name.lower() if c.isalnum()) if isinstance(name, str) else ""


def check_fit(machine: Dict[str, Any], facts: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every way the file `facts` describe does not fit `machine`, in a fixed order: bounds by
    axis (min before max, radius last), temperatures (hotend, bed), tools,
    nozzles by extruder, model. A limit or a file value that is unknown
    checks nothing."""
    findings = []  # type: List[Dict[str, Any]]

    box = facts.get("travel") if isinstance(facts.get("travel"), dict) else facts.get("print")
    axis_min = machine.get("axis_min") or [None, None, None]
    axis_max = machine.get("axis_max") or [None, None, None]
    for i, axis in enumerate(AXES):
        low, limit_low = _box_axis(box, "min", i), _number(axis_min[i])
        if low is not None and limit_low is not None and low < limit_low - LIMIT_EPSILON:
            findings.append(_finding(KIND_OUT_OF_BOUNDS, SEVERITY_BLOCK,
                                     axis=axis, side="min", value=low, limit=limit_low))
        high, limit_high = _box_axis(box, "max", i), _number(axis_max[i])
        if high is not None and limit_high is not None and high > limit_high + LIMIT_EPSILON:
            findings.append(_finding(KIND_OUT_OF_BOUNDS, SEVERITY_BLOCK,
                                     axis=axis, side="max", value=high, limit=limit_high))
    radius = _number(machine.get("print_radius"))
    if radius is not None:
        # The box's farthest edge from the centre. Its corners are farther,
        # but a round part need not fill them, so only an edge is certain.
        edges = [abs(v) for v in (_box_axis(box, end, i) for end in ("min", "max")
                                  for i in (0, 1)) if v is not None]
        reach = max(edges) if edges else None
        if reach is not None and reach > radius + LIMIT_EPSILON:
            findings.append(_finding(KIND_OUT_OF_BOUNDS, SEVERITY_BLOCK,
                                     axis="radius", side="max", value=reach, limit=radius))

    for heater, file_key, machine_key in (("extruder", "max_hotend_temp", "max_extruder_temp"),
                                          ("heater_bed", "max_bed_temp", "max_bed_temp")):
        value, limit = _number(facts.get(file_key)), _number(machine.get(machine_key))
        if value is not None and limit is not None and value > limit + LIMIT_EPSILON:
            findings.append(_finding(KIND_TEMP_OVER_MAX, SEVERITY_BLOCK,
                                     heater=heater, value=value, limit=limit))

    tool_count = machine.get("tool_count")
    tools = [t for t in facts.get("tools") or [] if isinstance(t, int) and not isinstance(t, bool)]
    if isinstance(tool_count, int) and not isinstance(tool_count, bool) and tools:
        highest = max(tools)
        if highest >= tool_count:
            findings.append(_finding(KIND_TOO_MANY_TOOLS, SEVERITY_BLOCK,
                                     tool=highest, limit=tool_count))

    file_nozzles = facts.get("nozzle_diameters") or []
    machine_nozzles = machine.get("nozzle_diameters") or []
    for tool in range(min(len(file_nozzles), len(machine_nozzles))):
        want, have = _number(file_nozzles[tool]), _number(machine_nozzles[tool])
        if want is not None and have is not None and abs(want - have) > NOZZLE_TOLERANCE:
            findings.append(_finding(KIND_NOZZLE_MISMATCH, SEVERITY_WARN,
                                     tool=tool, file=want, printer=have))

    file_model, machine_model = facts.get("printer_model"), machine.get("model")
    a, b = _model_key(file_model), _model_key(machine_model)
    if a and b and a not in b and b not in a:
        findings.append(_finding(KIND_PRINTER_MODEL_MISMATCH, SEVERITY_WARN,
                                 file=file_model, printer=machine_model))
    return findings


def blocking(findings: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    return [f for f in findings if f.get("severity") == SEVERITY_BLOCK]


def may_autostart(enabled: bool, findings: Optional[List[Dict[str, Any]]]) -> bool:
    """Whether an upload's requested print starts: always while the feature is
    off, and when the file could not be checked (no analysis, no limits);
    otherwise only with no blocking finding."""
    if not enabled or findings is None:
        return True
    return not blocking(findings)


def describe(finding: Dict[str, Any]) -> str:
    """One line for the log."""
    d = finding.get("detail") or {}
    kind = finding.get("kind")
    if kind == KIND_OUT_OF_BOUNDS:
        return "%s %s %s past the printer's %s" % (d.get("axis"), d.get("side"), d.get("value"),
                                                   d.get("limit"))
    if kind == KIND_TEMP_OVER_MAX:
        return "%s %s above max_temp %s" % (d.get("heater"), d.get("value"), d.get("limit"))
    if kind == KIND_TOO_MANY_TOOLS:
        return "tool T%s on a printer with %s" % (d.get("tool"), d.get("limit"))
    return "%s %s" % (kind, d)


# ============================================================================
# Moonraker side
# ============================================================================


def _wants_gcode_print(form_args: Any) -> bool:
    if not isinstance(form_args, dict):
        return False
    filename = str(form_args.get("filename", "")).lower()
    root = str(form_args.get("root", "gcodes")).lower()
    wants_print = str(form_args.get("print", "false")) == "true"
    return root == "gcodes" and wants_print and filename.endswith((".gcode", ".g", ".gco"))


class FitCheck:
    """Holds a slicer's upload-and-print until the file is checked."""

    def __init__(self, helix: Any) -> None:
        self.helix = helix
        self.server = helix.server
        self._original_finalize = None  # type: Optional[Callable]
        self._converter = None  # type: Optional[Any]

    def availability(self) -> tuple:
        analysis = getattr(self.helix, "analysis", None)
        if analysis is None or not analysis.availability()[0]:
            return False, "needs_file_analysis"
        if self._original_finalize is None:
            return False, "upload_hook_unsupported"
        return True, None

    def start(self) -> None:
        file_manager = getattr(self.helix, "file_manager", None)
        method = getattr(file_manager, "finalize_upload", None)
        if file_queue.upload_hook_shape_ok(method):
            self._original_finalize = method
            file_manager.finalize_upload = self._finalize_upload_hook
        else:
            logging.warning("HelixPrint: Moonraker's finalize_upload is not the expected "
                            "`async (form_args)`; fit check cannot hold uploads")
        converter = getattr(self.helix, "converter", None)
        if converter is not None and getattr(converter, "active", False):
            converter.start_gate = self.may_start
            self._converter = converter
        register = getattr(self.server, "register_notification", None)
        if register is not None:
            register(REFUSED_EVENT)

    def close(self) -> None:
        if self._original_finalize is not None:
            self.helix.file_manager.finalize_upload = self._original_finalize
            self._original_finalize = None
        if self._converter is not None:
            self._converter.start_gate = None
            self._converter = None

    async def _limits(self) -> Optional[Dict[str, Any]]:
        apis = getattr(self.helix, "klippy_apis", None)
        if apis is None:
            return None
        try:
            status = await apis.query_objects({"configfile": ["settings"]})
        except Exception as e:
            logging.info("HelixPrint: fit check cannot read the printer's config: %s", e)
            return None
        settings = ((status or {}).get("configfile") or {}).get("settings")
        if not isinstance(settings, dict):
            return None
        limits = machine_limits(settings)
        # A filament system's T commands are not in the config, so the count
        # of extruders and T macros cannot say a tool is missing.
        limits["tool_count"] = None
        return limits

    async def findings_for(self, path: str) -> Optional[List[Dict[str, Any]]]:
        """The findings for `path`, or None when it cannot be checked."""
        analysis = getattr(self.helix, "analysis", None)
        if analysis is None:
            return None
        record = await analysis.record_for(path, RECORD_WAIT_S)
        if record is None:
            logging.info("HelixPrint: no analysis of %s; starting it unchecked", path)
            return None
        limits = await self._limits()
        if limits is None:
            return None
        return check_fit(limits, fit_file_from_record(record))

    async def may_start(self, path: str) -> bool:
        """Whether a print the slicer asked for may start `path`."""
        enabled = await self.helix.feature_enabled(FIT_CHECK_FEATURE)
        if not enabled:
            return True
        findings = await self.findings_for(path)
        if may_autostart(enabled, findings):
            return True
        refused = blocking(findings or [])
        logging.warning("HelixPrint: not starting %s, it does not fit this printer: %s",
                        path, "; ".join(describe(f) for f in refused))
        send = getattr(self.server, "send_event", None)
        if send is not None:
            send(REFUSED_EVENT, {"filename": path, "findings": refused})
        return False

    async def _finalize_upload_hook(self, form_args: Dict[str, Any]) -> Dict[str, Any]:
        original = self._original_finalize
        assert original is not None
        if not _wants_gcode_print(form_args) \
                or not await self.helix.feature_enabled(FIT_CHECK_FEATURE):
            return await original(form_args)
        stored_args = dict(form_args)
        stored_args.pop("print", None)
        result = await original(stored_args)
        item = (result or {}).get("item") or {}
        path = str(item.get("path", "")).lstrip("/")
        if not path:
            return result
        result = dict(result)
        if await self.may_start(path):
            result.update(await file_queue.start_or_queue(
                self.server, self.helix.klippy_apis, self.helix.file_manager, path,
                form_args.get("current_user")))
        else:
            result.update(file_queue.print_result())
        return result
