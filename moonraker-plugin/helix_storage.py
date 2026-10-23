# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixPrint storage report and guard.

`server.helix.storage` reports what fills the printer's disks, by consumer:
the plugin's own scratch (`.helix_temp`, the app's staging directories and the
print links), .3mf conversion outputs, file analysis sidecars, history photos,
Moonraker timelapse renders, logs, Moonraker's thumbnails, the user's own
G-code, and power-loss recovery's snapshot and mesh. `server.helix.storage_cleanup
{consumer, older_than}` removes one consumer's files older than `older_than`
seconds; it refuses unknown consumers and every consumer that holds the user's
files or state another feature needs (CONSUMERS, `cleanable`).

`print_modified` / `print_edited` ask `free_space_refusal()` before they print
a copy, so a print never starts on a disk with less than FREE_SPACE_FLOOR_BYTES
left. Automatic cleanup by age (`storage_auto_cleanup`) acts, so it is off until
the user opts in.

Pure rules sit at the top and touch no Moonraker object; HelixStorage is the
Moonraker-facing half, created by helix_print.py.
"""

from __future__ import annotations

import logging
import os
import shutil
import time
from typing import Any, Dict, Iterable, List, NamedTuple, Optional, Set, Tuple

import helix_file_queue as file_queue
import helix_gcode_reader as reader

STORAGE_FEATURE = "storage"
STORAGE_FEATURE_VERSION = 1
AUTO_CLEANUP_FEATURE = "storage_auto_cleanup"
AUTO_CLEANUP_FEATURE_VERSION = 1

# The free space a print started through the plugin must leave on the gcodes
# volume: room for Klipper's and Moonraker's logs, Moonraker's database and a
# timelapse render while the print runs.
FREE_SPACE_FLOOR_BYTES = 100 * 1024 * 1024

# Where history photos are kept, under the gcodes root.
PHOTOS_DIR = ".helix_photos"

# Power-loss recovery's snapshot and mesh, under Moonraker's data directory.
PLR_DIR = "helix-plr"

# Moonraker's thumbnail directories, beside the files they belong to.
THUMBS_DIR = ".thumbs"

ROOT_GCODES = "gcodes"
ROOT_TIMELAPSE = "timelapse"
ROOT_LOGS = "logs"
ROOT_DATA = "data"

# Automatic cleanup runs this long after Moonraker starts, then this often.
AUTO_CLEANUP_FIRST_DELAY_S = 600.0
AUTO_CLEANUP_INTERVAL_S = 24 * 3600.0

DAY_S = 24 * 3600.0


class Consumer(NamedTuple):
    name: str
    root: str
    # The consumer's top-level directories under its root; empty for one that
    # owns its whole root or is found by classify_gcodes_path().
    dirs: Tuple[str, ...]
    # Whether storage_cleanup may remove its files.
    cleanable: bool
    # A file younger than this is never removed, whatever `older_than` says.
    min_age_s: float
    # Automatic cleanup removes its files older than this; None = never.
    auto_age_s: Optional[float]


# Report order. `helix_temp` also takes temp_dir and symlink_dir from
# [helix_print] (consumer_dirs()).
CONSUMERS = (
    Consumer("helix_temp", ROOT_GCODES, (".helix_temp", "helix_staging"), True,
             600.0, 7 * DAY_S),
    Consumer("3mf_outputs", ROOT_GCODES, (), False, 0.0, None),
    Consumer("analysis_sidecars", ROOT_GCODES, (reader.LAYER_INDEX_DIR,), True, 0.0, None),
    Consumer("photos", ROOT_GCODES, (PHOTOS_DIR,), True, 0.0, None),
    Consumer("timelapse", ROOT_TIMELAPSE, (), True, 0.0, None),
    Consumer("logs", ROOT_LOGS, (), True, 0.0, 30 * DAY_S),
    Consumer("thumbnails", ROOT_GCODES, (), False, 0.0, None),
    Consumer("user_gcode", ROOT_GCODES, (), False, 0.0, None),
    Consumer("plr", ROOT_DATA, (PLR_DIR,), False, 0.0, None),
)

CONSUMERS_BY_NAME = {c.name: c for c in CONSUMERS}


class StorageError(Exception):
    """A request the storage endpoints refuse; the message is user-facing."""


# =============================================================================
# Pure rules
# =============================================================================


def consumer_dirs(consumer: Consumer, temp_dir: str, symlink_dir: str) -> Tuple[str, ...]:
    """A consumer's top-level directories, with [helix_print]'s own scratch
    directories added to `helix_temp`."""
    if consumer.name != "helix_temp":
        return consumer.dirs
    return tuple(dict.fromkeys(consumer.dirs + (temp_dir, symlink_dir)))


def classify_gcodes_path(rel: str, temp_dirs: Iterable[str], outputs: Set[str]) -> str:
    """The consumer a file under the gcodes root belongs to. `rel` is
    gcodes-relative with `/` separators; `outputs` holds the .3mf converter's
    own outputs. Everything no plugin feature owns is the user's."""
    parts = rel.split("/")
    top = parts[0]
    if len(parts) > 1:
        if top in temp_dirs:
            return "helix_temp"
        if top == reader.LAYER_INDEX_DIR:
            return "analysis_sidecars"
        if top == PHOTOS_DIR:
            return "photos"
    if THUMBS_DIR in parts[:-1]:
        return "thumbnails"
    if rel in outputs:
        return "3mf_outputs"
    return "user_gcode"


def is_live_log(name: str) -> bool:
    """A log a service is still writing (`klippy.log`), as opposed to a rotated
    one (`klippy.log.2026-10-20`, `moonraker.log.1`). Never removed."""
    return name.endswith(".log")


def free_space_refusal(free_bytes: Optional[int], incoming_bytes: int,
                       floor: int = FREE_SPACE_FLOOR_BYTES) -> Optional[str]:
    """Why a print must not start, or None. `free_bytes` is the volume's free
    space now (None when it cannot be read, which never refuses: a guard that
    cannot measure must not block printing); `incoming_bytes` what the plugin
    is about to write there first."""
    if free_bytes is None:
        return None
    left = free_bytes - max(0, incoming_bytes)
    if left >= floor:
        return None
    return (
        "Not enough free space on the printer: this print would leave %s free, "
        "below the %s HelixPrint keeps free. Delete old files, or clean up in "
        "Settings > System > Storage." % (format_mb(max(0, left)), format_mb(floor))
    )


def format_mb(n: int) -> str:
    return "%d MB" % (n // (1024 * 1024))


def cleanup_candidates(
    consumer: Consumer,
    files: Iterable[Tuple[str, int, float]],
    now: float,
    older_than: float,
    protected: Set[str],
) -> List[Tuple[str, int]]:
    """The files of `consumer` a cleanup removes, as (path, size): those at
    least `older_than` seconds old (and never younger than the consumer's
    `min_age_s`), not protected, and for logs not live. `files` holds
    (absolute path, size, mtime)."""
    if not consumer.cleanable:
        return []
    min_age = max(float(older_than), consumer.min_age_s)
    chosen = []  # type: List[Tuple[str, int]]
    for path, size, mtime in files:
        if os.path.abspath(path) in protected:
            continue
        if consumer.name == "logs" and is_live_log(os.path.basename(path)):
            continue
        if consumer.name == "analysis_sidecars" and \
                not path.endswith(reader.LAYER_INDEX_SUFFIX):
            # A whole-file write's .tmp is prune_layer_indexes' to judge.
            continue
        if now - mtime < min_age:
            continue
        chosen.append((path, size))
    return chosen


def parse_older_than(value: Any) -> float:
    """`older_than` in seconds: a number of zero or more."""
    if isinstance(value, bool):
        raise StorageError("older_than must be a number of seconds")
    try:
        seconds = float(value)
    except (TypeError, ValueError):
        raise StorageError("older_than must be a number of seconds")
    if seconds != seconds or seconds < 0:
        raise StorageError("older_than must be zero or more seconds")
    return seconds


def resolve_consumer(name: Any) -> Consumer:
    """The consumer a cleanup names; refuses unknown names and every consumer
    that is not cleanable."""
    consumer = CONSUMERS_BY_NAME.get(name) if isinstance(name, str) else None
    if consumer is None:
        raise StorageError("Unknown storage consumer: %r" % (name,))
    if not consumer.cleanable:
        raise StorageError("%s cannot be cleaned up from here" % consumer.name)
    return consumer


# =============================================================================
# Filesystem walks (run off the event loop)
# =============================================================================


def _walk_files(top: str, skip: Set[str]) -> Iterable[Tuple[str, int, float]]:
    """(path, size, mtime) of every file under `top`, without following
    links into directories, and leaving out the directories in `skip`
    (another root mounted inside this one)."""
    for dirpath, dirs, files in os.walk(top, followlinks=False):
        dirs[:] = [d for d in dirs if os.path.realpath(os.path.join(dirpath, d)) not in skip]
        for name in files:
            path = os.path.join(dirpath, name)
            try:
                st = os.lstat(path)
            except OSError:
                continue
            yield path, st.st_size, st.st_mtime


def _empty_usage() -> Dict[str, Dict[str, Any]]:
    return {c.name: {"bytes": 0, "files": 0} for c in CONSUMERS}


def scan_usage(
    roots: Dict[str, Optional[str]],
    temp_dirs: Tuple[str, ...],
    outputs: Set[str],
) -> Dict[str, Dict[str, Any]]:
    """Bytes and file count per consumer. `roots` maps ROOT_* to a directory,
    None for one this Moonraker does not have (its consumers report
    `available: false`)."""
    usage = _empty_usage()
    present = {k: v for k, v in roots.items() if v and os.path.isdir(v)}
    others = {os.path.realpath(v) for k, v in present.items() if k != ROOT_GCODES}
    gcodes = present.get(ROOT_GCODES)
    if gcodes is not None:
        for path, size, _mtime in _walk_files(gcodes, others):
            rel = os.path.relpath(path, gcodes).replace(os.sep, "/")
            entry = usage[classify_gcodes_path(rel, temp_dirs, outputs)]
            entry["bytes"] += size
            entry["files"] += 1
    for consumer in CONSUMERS:
        if consumer.root == ROOT_GCODES:
            continue
        root = present.get(consumer.root)
        if root is None:
            continue
        tops = [os.path.join(root, d) for d in consumer.dirs] or [root]
        for top in tops:
            for _path, size, _mtime in _walk_files(top, set()):
                usage[consumer.name]["bytes"] += size
                usage[consumer.name]["files"] += 1
    for consumer in CONSUMERS:
        root = present.get(consumer.root)
        usage[consumer.name]["available"] = root is not None
        usage[consumer.name]["cleanable"] = consumer.cleanable and root is not None
    return usage


def consumer_files(root: str, tops: Tuple[str, ...]) -> List[Tuple[str, int, float]]:
    """Every file of a consumer with directories `tops` under `root` (the whole
    root when `tops` is empty)."""
    found = []  # type: List[Tuple[str, int, float]]
    for top in [os.path.join(root, d) for d in tops] or [root]:
        if os.path.isdir(top):
            found.extend(_walk_files(top, set()))
    return found


def remove_files(paths: List[Tuple[str, int]], keep_dirs: Set[str]) -> Tuple[int, int, int]:
    """Remove `paths`, then the directories they leave empty, except
    `keep_dirs` and anything above them. (removed, bytes freed, errors)."""
    removed = freed = errors = 0
    parents = set()  # type: Set[str]
    for path, size in paths:
        try:
            os.remove(path)
        except FileNotFoundError:
            continue
        except OSError as e:
            logging.warning("HelixPrint: could not remove %s: %s", path, e)
            errors += 1
            continue
        removed += 1
        freed += size
        parents.add(os.path.dirname(path))
    keep = {os.path.abspath(d) for d in keep_dirs}
    for parent in sorted(parents, key=len, reverse=True):
        d = os.path.abspath(parent)
        while d not in keep and any(d.startswith(k + os.sep) for k in keep):
            try:
                os.rmdir(d)
            except OSError:
                break
            d = os.path.dirname(d)
    return removed, freed, errors


def disk_usage(path: Optional[str]) -> Optional[Dict[str, int]]:
    if not path:
        return None
    try:
        du = shutil.disk_usage(path)
    except OSError:
        return None
    return {"total": du.total, "used": du.used, "free": du.free}


# =============================================================================
# Moonraker-facing half
# =============================================================================


class HelixStorage:
    """The storage endpoints, the free-space guard and automatic cleanup inside
    a running Moonraker. helix_print.py creates one in its component_init."""

    def __init__(self, helix: Any) -> None:
        self.helix = helix
        self.server = helix.server
        self.eventloop = self.server.get_event_loop()
        self.clock = time.time
        self.last_auto_cleanup: Optional[Dict[str, Any]] = None
        self._timer: Any = None

    def start(self) -> None:
        self.server.register_endpoint(
            "/server/helix/storage", ["GET"], self.handle_storage)
        self.server.register_endpoint(
            "/server/helix/storage_cleanup", ["POST"], self.handle_storage_cleanup)
        self._schedule(AUTO_CLEANUP_FIRST_DELAY_S)

    def close(self) -> None:
        cancel = getattr(self._timer, "cancel", None)
        if cancel is not None:
            cancel()
        self._timer = None

    # -- roots ---------------------------------------------------------------

    def roots(self) -> Dict[str, Optional[str]]:
        helix = self.helix
        fm = helix.file_manager
        get = getattr(fm, "get_directory", None)

        def root(name: str) -> Optional[str]:
            try:
                path = get(name) if get is not None else ""
            except Exception:
                path = ""
            return path or None

        gcodes = str(helix.gc_path) if helix.gc_path is not None else None
        return {
            ROOT_GCODES: gcodes,
            ROOT_TIMELAPSE: root("timelapse"),
            ROOT_LOGS: root("logs"),
            ROOT_DATA: str(helix._data_dir()),
        }

    def _temp_dirs(self) -> Tuple[str, ...]:
        return consumer_dirs(CONSUMERS_BY_NAME["helix_temp"], self.helix.temp_dir,
                             self.helix.symlink_dir)

    def _3mf_outputs(self) -> Set[str]:
        converter = getattr(self.helix, "converter", None)
        records = getattr(converter, "records", None) or {}
        return {c.output for convs in records.values() for c in convs
                if getattr(c, "owned", False) and c.output}

    def free_bytes(self) -> Optional[int]:
        usage = disk_usage(str(self.helix.gc_path) if self.helix.gc_path else None)
        return usage["free"] if usage else None

    # -- guard ---------------------------------------------------------------

    def check_free_space(self, incoming_bytes: int) -> None:
        """Raise a 507 when writing `incoming_bytes` to the gcodes volume and
        printing from it would leave less than FREE_SPACE_FLOOR_BYTES free."""
        refusal = free_space_refusal(self.free_bytes(), incoming_bytes)
        if refusal is not None:
            logging.warning("HelixPrint: %s", refusal)
            raise self.server.error(refusal, 507)

    # -- report --------------------------------------------------------------

    async def report(self) -> Dict[str, Any]:
        roots = self.roots()
        usage = await self.eventloop.run_in_thread(
            scan_usage, roots, self._temp_dirs(), self._3mf_outputs())
        consumers = []
        for consumer in CONSUMERS:
            entry = {"name": consumer.name, "root": consumer.root}
            entry.update(usage[consumer.name])
            consumers.append(entry)
        return {
            "volume": disk_usage(roots[ROOT_GCODES]),
            "floor": FREE_SPACE_FLOOR_BYTES,
            "consumers": consumers,
            "auto_cleanup": {
                "enabled": await self.helix.feature_enabled(AUTO_CLEANUP_FEATURE),
                "last_run": self.last_auto_cleanup,
            },
        }

    async def handle_storage(self, web_request: Any) -> Dict[str, Any]:
        return await self.report()

    # -- cleanup -------------------------------------------------------------

    async def _printing_paths(self) -> Set[str]:
        """The file Klipper is printing or holding paused, as named and as
        resolved through its link."""
        apis = self.helix.klippy_apis
        if apis is None or self.helix.gc_path is None:
            return set()
        try:
            result = await apis.query_objects({"print_stats": ["filename", "state"]})
        except Exception:
            return set()
        stats = result.get("print_stats", {}) if isinstance(result, dict) else {}
        name = stats.get("filename") if isinstance(stats, dict) else None
        if not name or stats.get("state") in file_queue.IDLE_JOB_STATES:
            return set()
        path = os.path.join(str(self.helix.gc_path), name)
        return {os.path.abspath(path), os.path.realpath(path)}

    def _protected_indexes(self) -> List[str]:
        """The layer indexes power-loss recovery and a running analysis use."""
        analysis = getattr(self.helix, "analysis", None)
        protected = getattr(analysis, "_protected_indexes", None)
        if protected is None:
            plr = getattr(self.helix, "plr", None)
            protected = getattr(plr, "protected_index_paths", None)
        return list(protected()) if protected is not None else []

    async def protected_paths(self) -> Set[str]:
        """Files no cleanup removes: an active linked print's copy and link,
        whatever Klipper is printing, and the layer indexes in use."""
        gc = str(self.helix.gc_path) if self.helix.gc_path is not None else ""
        paths = set()  # type: Set[str]
        for link, info in getattr(self.helix, "active_prints", {}).items():
            paths.add(os.path.abspath(os.path.join(gc, link)))
            paths.add(os.path.abspath(os.path.join(gc, info.temp_filename)))
        paths |= await self._printing_paths()
        paths |= {os.path.abspath(p) for p in self._protected_indexes() if p}
        return paths

    async def cleanup(self, consumer: Consumer, older_than: float) -> Dict[str, Any]:
        roots = self.roots()
        root = roots.get(consumer.root)
        result = {"consumer": consumer.name, "removed": 0, "freed": 0, "errors": 0}
        if not root or not os.path.isdir(root):
            return result
        protected = await self.protected_paths()
        tops = consumer_dirs(consumer, self.helix.temp_dir, self.helix.symlink_dir)
        if consumer.name == "analysis_sidecars":
            await self.eventloop.run_in_thread(
                reader.prune_layer_indexes, root, self._protected_indexes())
        files = await self.eventloop.run_in_thread(consumer_files, root, tops)
        chosen = cleanup_candidates(consumer, files, self.clock(), older_than, protected)
        keep = {os.path.join(root, d) for d in tops} or {root}
        removed, freed, errors = await self.eventloop.run_in_thread(remove_files, chosen, keep)
        result.update({"removed": removed, "freed": freed, "errors": errors})
        logging.info("HelixPrint: storage cleanup of %s older than %ss removed %d files (%d bytes)",
                     consumer.name, older_than, removed, freed)
        return result

    async def handle_storage_cleanup(self, web_request: Any) -> Dict[str, Any]:
        try:
            consumer = resolve_consumer(web_request.get_str("consumer", ""))
            older_than = parse_older_than(_arg(web_request, "older_than", 0))
        except StorageError as e:
            raise self.server.error(str(e), 400)
        return await self.cleanup(consumer, older_than)

    # -- automatic cleanup ---------------------------------------------------

    def _schedule(self, delay: float) -> None:
        delay_callback = getattr(self.eventloop, "delay_callback", None)
        if delay_callback is not None:
            self._timer = delay_callback(delay, self.auto_cleanup)

    async def _job_idle(self) -> bool:
        apis = self.helix.klippy_apis
        if apis is None:
            return False
        try:
            result = await apis.query_objects({"print_stats": ["state"]})
        except Exception:
            return False
        stats = result.get("print_stats", {}) if isinstance(result, dict) else {}
        return isinstance(stats, dict) and stats.get("state") in file_queue.IDLE_JOB_STATES

    async def auto_cleanup(self) -> Optional[Dict[str, Any]]:
        """Remove every consumer's files older than its `auto_age_s`, when the
        user opted in and no print is running. Reschedules itself."""
        self._schedule(AUTO_CLEANUP_INTERVAL_S)
        try:
            if not await self.helix.feature_enabled(AUTO_CLEANUP_FEATURE):
                return None
            if not await self._job_idle():
                return None
            removed = freed = 0
            for consumer in CONSUMERS:
                if consumer.auto_age_s is None:
                    continue
                result = await self.cleanup(consumer, consumer.auto_age_s)
                removed += result["removed"]
                freed += result["freed"]
            self.last_auto_cleanup = {"time": self.clock(), "removed": removed, "freed": freed}
            return self.last_auto_cleanup
        except Exception:
            logging.exception("HelixPrint: automatic storage cleanup failed")
            return None


def _arg(web_request: Any, key: str, default: Any) -> Any:
    getter = getattr(web_request, "get", None)
    if getter is None:
        return default
    try:
        return getter(key, default)
    except Exception:
        return default
