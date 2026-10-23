# SPDX-License-Identifier: GPL-3.0-or-later
"""
Camera photos in print history: the HelixPrint plugin's `photos` feature.

For each job history adds, the default webcam's snapshot is saved twice: once
the print finishes its second layer (print_stats.info.current_layer moving
from 1 or 2 past 2) and once when the job ends completed, cancelled or in
error. The JPEG bytes are stored unchanged under PHOTOS_DIR in the gcodes
root, listed in a record in PHOTOS_NAMESPACE keyed by history job id, and
linked from the job's history `auxiliary_data` where that Moonraker has it.
The screen's own fallback writes the same files and record when the plugin
cannot.

Storage is bounded: a record whose history job is gone (deleted, or its id
reused by a later job) is pruned with its files, and beyond MAX_JOBS jobs or
MAX_TOTAL_BYTES of photos the oldest jobs go first. No photo is taken while
the volume has less than MIN_FREE_BYTES free.

Everything above HelixPhotos is pure and touches no Moonraker object.
Python 3.8, stdlib only.
"""

from __future__ import annotations

import asyncio
import logging
import os
import shutil
import time
import urllib.request
from typing import Any, Callable, Dict, List, Optional, Tuple

PHOTOS_FEATURE = "photos"
PHOTOS_FEATURE_VERSION = 1

# One record per history job: key = job id as history spells it ("00002A").
PHOTOS_NAMESPACE = "helix_photos"
RECORD_VERSION = 1

# Under the gcodes root, so a screen reads and writes the files through
# Moonraker's file API. Moonraker lists no hidden directory.
PHOTOS_DIR = ".helix_photos"

KIND_LAYER = "layer2"
KIND_END = "end"

# History statuses that end a job with a part worth a photo.
END_STATUSES = ("completed", "cancelled", "error")

# The marker kind of a job the plugin resumed after a power loss: the
# interrupted job already has its layer-2 photo.
PLR_RESUME_KIND = "plr_resume"

# Bounds on what one photo, and all of them together, may take.
MAX_PHOTO_BYTES = 8 * 1024 * 1024
MAX_TOTAL_BYTES = 128 * 1024 * 1024
MAX_JOBS = 500
MIN_FREE_BYTES = 256 * 1024 * 1024
# A file with no record is a screen's upload whose record has not landed yet,
# until it is this old.
ORPHAN_GRACE_S = 3600.0

FETCH_TIMEOUT_S = 10.0

# Our entry's name in a history job's auxiliary_data (HelixPrint.set_history_aux)
AUX_NAME = "photos"

# Where a relative snapshot URL is served on the printer's own host
LOCAL_WEB_HOST = "http://127.0.0.1"

# The webcam a HelixScreen's built-in screen server registers: a screen, not
# a view of the printer.
SCREEN_WEBCAM_NAME = "HelixScreen Screen"


# ============================================================================
# Pure rules
# ============================================================================


def photo_name(job_id: str, start_time: Any, kind: str) -> str:
    """The file name of one photo. The start time keeps a job id history
    reuses from overwriting the earlier job's photos."""
    try:
        start = int(float(start_time))
    except (TypeError, ValueError):
        start = 0
    return f"{job_id}_{start}_{kind}.jpg"


def photo_path(job_id: str, start_time: Any, kind: str) -> str:
    """The photo's path in the gcodes root."""
    return f"{PHOTOS_DIR}/{photo_name(job_id, start_time, kind)}"


def layer_two_done(previous: Any, current: Any) -> bool:
    """Whether the move from layer `previous` to `current` finishes layer 2.
    A job first seen past layer 2 (a resumed part, a restart mid-print) never
    qualifies: its layer-2 photo would show a later layer."""
    if not isinstance(previous, int) or not isinstance(current, int):
        return False
    if isinstance(previous, bool) or isinstance(current, bool):
        return False
    return 1 <= previous <= 2 < current


def is_jpeg(data: Any) -> bool:
    """A JPEG starts with an SOI marker and a segment marker; an HTML error
    page or an empty body does not."""
    return isinstance(data, (bytes, bytearray)) and len(data) > 3 and data[:3] == b"\xff\xd8\xff"


def is_mjpeg_service(service: Any) -> bool:
    return isinstance(service, str) and ("mjpeg" in service or "ustreamer" in service)


def is_usable_snapshot_url(url: Any) -> bool:
    """The app's rule (include/webcam_selection.h): an HTML page is not an
    image endpoint; anything else might be."""
    if not isinstance(url, str) or not url:
        return False
    path = url.lower().split("?", 1)[0].split("#", 1)[0]
    return not path.endswith(".html")


def normalize_webcam(entry: Any) -> Optional[Dict[str, Any]]:
    """One webcam as {name, enabled, service, snapshot_url, stream_url}, from
    either server.webcams.list's spelling or the `webcams` database
    namespace's (urlSnapshot, urlStream)."""
    if not isinstance(entry, dict):
        return None
    snapshot = entry.get("snapshot_url", entry.get("urlSnapshot", ""))
    stream = entry.get("stream_url", entry.get("urlStream", ""))
    return {
        "name": str(entry.get("name", "")),
        "enabled": entry.get("enabled", True) is not False,
        "service": str(entry.get("service", "")),
        "snapshot_url": snapshot if isinstance(snapshot, str) else "",
        "stream_url": stream if isinstance(stream, str) else "",
    }


def is_screen_webcam(cam: Dict[str, Any]) -> bool:
    name = cam.get("name", "")
    return name == SCREEN_WEBCAM_NAME or name.startswith(SCREEN_WEBCAM_NAME + " (")


def pick_snapshot_url(webcams: List[Any]) -> Optional[str]:
    """The snapshot URL of the camera a screen shows by default: the first
    enabled MJPEG camera, else the first enabled camera, in Moonraker's order,
    each only with a usable snapshot URL. Mirrors webcam::photo_pick in the
    app, so the plugin and the screen photograph the same camera."""
    cams = [c for c in (normalize_webcam(w) for w in webcams or []) if c is not None]
    cams = [c for c in cams if c["enabled"] and not is_screen_webcam(c)
            and is_usable_snapshot_url(c["snapshot_url"])]
    for cam in cams:
        if is_mjpeg_service(cam["service"]) and cam["stream_url"]:
            return cam["snapshot_url"]
    return cams[0]["snapshot_url"] if cams else None


def absolute_url(url: str, host: str = LOCAL_WEB_HOST) -> str:
    """A relative snapshot URL ("/webcam/?action=snapshot") is served by the
    web frontend on this host."""
    if "://" in url:
        return url
    return f"{host}/{url.lstrip('/')}"


def new_record(job_id: str, filename: str, start_time: Any) -> Dict[str, Any]:
    return {
        "version": RECORD_VERSION,
        "job_id": job_id,
        "filename": filename,
        "start_time": start_time,
        "photos": [],
    }


def valid_record(record: Any) -> bool:
    return (
        isinstance(record, dict)
        and record.get("version") == RECORD_VERSION
        and isinstance(record.get("job_id"), str)
        and isinstance(record.get("photos"), list)
    )


def with_photo(record: Dict[str, Any], kind: str, path: str, size: int,
               taken_at: float, source: str) -> Dict[str, Any]:
    """`record` with the photo of `kind` set, replacing an earlier one."""
    photos = [p for p in record.get("photos", [])
              if isinstance(p, dict) and p.get("kind") != kind]
    photos.append({"kind": kind, "path": path, "size": int(size),
                   "taken_at": taken_at, "source": source})
    photos.sort(key=lambda p: 0 if p.get("kind") == KIND_LAYER else 1)
    out = dict(record)
    out["photos"] = photos
    return out


def record_paths(record: Any) -> List[str]:
    """Every photo path a record lists that lies inside PHOTOS_DIR."""
    if not isinstance(record, dict) or not isinstance(record.get("photos"), list):
        return []
    out = []
    for photo in record["photos"]:
        path = photo.get("path") if isinstance(photo, dict) else None
        if isinstance(path, str) and _inside_photos_dir(path):
            out.append(path)
    return out


def _inside_photos_dir(path: str) -> bool:
    prefix = PHOTOS_DIR + "/"
    if not path.startswith(prefix):
        return False
    name = path[len(prefix):]
    return bool(name) and "/" not in name and name not in (".", "..")


def aux_paths(record: Dict[str, Any]) -> List[str]:
    """The value of our photos entry in a job's auxiliary_data: the paths."""
    return [p["path"] for p in record.get("photos", []) if isinstance(p, dict)]


def history_matches(record: Dict[str, Any], job: Any) -> bool:
    """Whether history's `job` is the job `record` was taken for: history
    reuses ids, so the start time must agree too."""
    if not isinstance(job, dict):
        return False
    try:
        return abs(float(job.get("start_time")) - float(record.get("start_time"))) < 1.0
    except (TypeError, ValueError):
        return False


def prune_plan(
    records: List[Tuple[str, Any, Optional[bool]]],
    sizes: Dict[str, int],
    max_jobs: int = MAX_JOBS,
    max_bytes: int = MAX_TOTAL_BYTES,
) -> List[str]:
    """The record keys to delete, with their files.

    `records` holds (key, record, alive): alive is whether history still has
    that job (False: gone; None: cannot be told, so kept). `sizes` maps a
    photo path to its size on disk. A malformed record or a dead one goes;
    then the oldest by start time until at most `max_jobs` remain and their
    photos fit in `max_bytes`.
    """
    drop: List[str] = []
    live: List[Tuple[float, str, int]] = []
    for key, record, alive in records:
        if not valid_record(record) or alive is False:
            drop.append(key)
            continue
        try:
            start = float(record.get("start_time") or 0.0)
        except (TypeError, ValueError):
            start = 0.0
        size = sum(sizes.get(p, 0) for p in record_paths(record))
        live.append((start, key, size))
    live.sort()
    total = sum(s for _, _, s in live)
    while live and (len(live) > max_jobs or total > max_bytes):
        _, key, size = live.pop(0)
        total -= size
        drop.append(key)
    return drop


def orphan_files(names: List[Tuple[str, float]], referenced: List[str], now: float,
                 grace: float = ORPHAN_GRACE_S) -> List[str]:
    """Files in PHOTOS_DIR, as (name, mtime), that no record lists and that
    are older than `grace`. Temp files of an interrupted write count too."""
    keep = {p[len(PHOTOS_DIR) + 1:] for p in referenced}
    return [n for n, mtime in names if n not in keep and now - mtime > grace]


def fetch_snapshot(url: str, timeout: float = FETCH_TIMEOUT_S,
                   max_bytes: int = MAX_PHOTO_BYTES) -> bytes:
    """GET `url`, refusing a body over `max_bytes`. Blocking: run it in a
    thread."""
    request = urllib.request.Request(url, headers={"User-Agent": "HelixPrint"})
    with urllib.request.urlopen(request, timeout=timeout) as response:  # nosec B310
        data = response.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise ValueError(f"snapshot over {max_bytes} bytes")
    return data


# ============================================================================
# Moonraker component
# ============================================================================


class HelixPhotos:
    """The Moonraker-facing half, created by helix_print.py."""

    def __init__(self, helix: Any, write_bytes: Callable[[str, bytes], None]) -> None:
        self.helix = helix
        self.server = helix.server
        self.write_bytes = write_bytes
        self.fetch: Callable[[str], bytes] = fetch_snapshot
        self.clock = time.time
        # The job history has open: job_id, filename, start_time
        self.job: Optional[Dict[str, Any]] = None
        self.layer: Optional[int] = None
        self.layer_photo_due = False
        self._record_lock: Optional[asyncio.Lock] = None
        self._tasks: List[Any] = []

    def start(self) -> None:
        self.server.register_event_handler("history:history_changed", self.on_history_changed)
        self.server.register_event_handler("server:status_update", self.on_status_update)
        self.server.register_event_handler("server:klippy_ready", self.on_klippy_ready)

    @property
    def database(self) -> Any:
        return self.helix.database

    def photos_dir(self) -> Optional[str]:
        gc_path = self.helix.gc_path
        return None if gc_path is None else os.path.join(str(gc_path), PHOTOS_DIR)

    def _lock(self) -> asyncio.Lock:
        # Made inside the running loop: Python 3.8 binds a Lock to the loop
        # current when it is created.
        if self._record_lock is None:
            self._record_lock = asyncio.Lock()
        return self._record_lock

    async def load(self) -> None:
        """Pick up the job history has open, for a restart mid-print. Its
        layer-2 photo is not taken: the layer it would show is unknown."""
        history = self.helix.history
        job = getattr(history, "current_job", None) if history is not None else None
        job_id = getattr(history, "current_job_id", None) if history is not None else None
        if job_id is None or getattr(job, "status", "") != "in_progress":
            return
        key = f"{job_id:06X}" if isinstance(job_id, int) else str(job_id)
        self.job = {"job_id": key, "filename": getattr(job, "filename", "") or "",
                    "start_time": getattr(job, "start_time", None)}
        self.layer = None
        self.layer_photo_due = False

    # -- events --------------------------------------------------------------

    async def on_klippy_ready(self, *args: Any) -> None:
        apis = self.helix.klippy_apis
        if apis is None:
            return
        try:
            status = await apis.subscribe_objects({"print_stats": ["info", "state"]})
        except Exception as e:
            logging.info("HelixPrint: photos could not subscribe print_stats: %s", e)
            return
        if isinstance(status, dict):
            self._layer_from(status)

    def _layer_from(self, status: Dict[str, Any]) -> Optional[int]:
        stats = status.get("print_stats")
        info = stats.get("info") if isinstance(stats, dict) else None
        if not isinstance(info, dict) or "current_layer" not in info:
            return None
        layer = info.get("current_layer")
        return layer if isinstance(layer, int) and not isinstance(layer, bool) else None

    async def on_status_update(self, status: Dict[str, Any], *args: Any) -> None:
        if not isinstance(status, dict):
            return
        layer = self._layer_from(status)
        if layer is None:
            return
        previous, self.layer = self.layer, layer
        if self.job is None or not self.layer_photo_due:
            return
        if layer_two_done(previous, layer):
            self.layer_photo_due = False
            self._spawn(self.capture(dict(self.job), KIND_LAYER))

    async def on_history_changed(self, payload: Dict[str, Any], *args: Any) -> None:
        job = payload.get("job") if isinstance(payload, dict) else None
        if not isinstance(job, dict) or not job.get("job_id"):
            return
        ref = {"job_id": str(job["job_id"]), "filename": job.get("filename", "") or "",
               "start_time": job.get("start_time")}
        action = payload.get("action")
        if action == "added":
            # helix_print binds its marker before it first awaits, and its
            # handler runs before this one.
            kind = self.helix.helix_initiated(ref["job_id"], ref["filename"], ref["start_time"])
            self.job = ref
            self.layer = None
            self.layer_photo_due = kind != PLR_RESUME_KIND
        elif action == "finished":
            if self.job is not None and self.job["job_id"] == ref["job_id"]:
                self.job = None
            self.layer_photo_due = False
            if job.get("status") in END_STATUSES:
                await self.capture(ref, KIND_END)
            await self.prune()

    def _spawn(self, coro: Any) -> None:
        task = asyncio.ensure_future(coro)
        self._tasks.append(task)
        task.add_done_callback(lambda t: self._tasks.remove(t) if t in self._tasks else None)

    async def close(self) -> None:
        for task in list(self._tasks):
            task.cancel()

    # -- capture -------------------------------------------------------------

    async def _webcams(self) -> List[Any]:
        webcam = self.server.lookup_component("webcam", None)
        getter = getattr(webcam, "get_webcams", None)
        if getter is not None:
            try:
                cams = getter()
                return [c.as_dict() if hasattr(c, "as_dict") else vars(c)
                        for c in (cams.values() if isinstance(cams, dict) else cams)]
            except Exception as e:
                logging.info("HelixPrint: photos could not list webcams: %s", e)
                return []
        # A Moonraker without the webcam component's API keeps them here.
        if self.database is None:
            return []
        try:
            stored = await self.database.get_item("webcams", None, {})
        except Exception:
            return []
        return list(stored.values()) if isinstance(stored, dict) else []

    def _free_bytes(self, directory: str) -> Optional[int]:
        try:
            return shutil.disk_usage(directory).free
        except OSError:
            return None

    async def _run_in_thread(self, fn: Callable[..., Any], *args: Any) -> Any:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, fn, *args)

    async def capture(self, job: Dict[str, Any], kind: str) -> Optional[str]:
        """Save one photo for `job`, and list it in the job's record and
        auxiliary_data. Returns the photo's path, or None when nothing was
        saved: the feature is off, there is no webcam, or the fetch failed."""
        if not await self.helix.feature_enabled(PHOTOS_FEATURE):
            return None
        directory = self.photos_dir()
        if directory is None:
            return None
        url = pick_snapshot_url(await self._webcams())
        if url is None:
            logging.debug("HelixPrint: no webcam, no %s photo for job %s", kind, job["job_id"])
            return None
        free = self._free_bytes(os.path.dirname(directory))
        if free is not None and free < MIN_FREE_BYTES:
            logging.info("HelixPrint: %d bytes free, no %s photo for job %s",
                         free, kind, job["job_id"])
            return None
        try:
            data = await self._run_in_thread(self.fetch, absolute_url(url))
        except Exception as e:
            logging.info("HelixPrint: %s photo for job %s not taken: %s", kind, job["job_id"], e)
            return None
        if not is_jpeg(data):
            logging.info("HelixPrint: snapshot for job %s is not a JPEG", job["job_id"])
            return None
        path = photo_path(job["job_id"], job.get("start_time"), kind)
        try:
            os.makedirs(directory, exist_ok=True)
            await self._run_in_thread(
                self.write_bytes, os.path.join(directory, os.path.basename(path)), bytes(data))
        except Exception as e:
            logging.warning("HelixPrint: could not save photo %s: %s", path, e)
            return None
        record = await self._add_to_record(job, kind, path, len(data))
        if record is not None:
            await self._link_in_history(job, record)
        logging.info("HelixPrint: %s photo for job %s saved as %s", kind, job["job_id"], path)
        return path

    async def _add_to_record(self, job: Dict[str, Any], kind: str, path: str,
                             size: int) -> Optional[Dict[str, Any]]:
        if self.database is None:
            return None
        async with self._lock():
            try:
                record = await self.database.get_item(PHOTOS_NAMESPACE, [job["job_id"]], None)
            except Exception:
                record = None
            if not valid_record(record) or not history_matches(record, job):
                if valid_record(record):
                    await self._delete_files(record_paths(record))
                record = new_record(job["job_id"], job.get("filename", ""), job.get("start_time"))
            record = with_photo(record, kind, path, size, self.clock(), "plugin")
            try:
                await self.database.insert_item(PHOTOS_NAMESPACE, [job["job_id"]], record)
            except Exception as e:
                logging.warning("HelixPrint: could not store photo record: %s", e)
                return None
            return record

    async def _link_in_history(self, job: Dict[str, Any], record: Dict[str, Any]) -> None:
        """List the photos in the job's auxiliary_data, where this Moonraker's
        history can rewrite a job (get_job/save_job, v0.9 and newer)."""
        history = self.helix.history
        patch = getattr(self.helix, "patch_history_job", None)
        if history is None or patch is None:
            return

        def update(row: Dict[str, Any]) -> bool:
            if not history_matches(record, row):
                return False
            row["auxiliary_data"] = self.helix.set_history_aux(
                row.get("auxiliary_data"), AUX_NAME, aux_paths(record), "Camera photos")
            return True

        await patch(job["job_id"], update)

    # -- pruning -------------------------------------------------------------

    async def _history_alive(self, key: str, record: Any) -> Optional[bool]:
        """Whether history still has the job `record` was taken for. None
        when this Moonraker's history cannot be asked."""
        if not valid_record(record):
            return False
        history = self.helix.history
        getter = getattr(history, "get_job", None) if history is not None else None
        if getter is None:
            return None
        try:
            job = await getter(key)
        except Exception:
            return None
        return history_matches(record, job)

    def _sizes(self, directory: str) -> Dict[str, int]:
        sizes: Dict[str, int] = {}
        try:
            entries = list(os.scandir(directory))
        except OSError:
            return sizes
        for entry in entries:
            try:
                sizes[f"{PHOTOS_DIR}/{entry.name}"] = entry.stat().st_size
            except OSError:
                pass
        return sizes

    async def _delete_files(self, paths: List[str]) -> None:
        gc_path = self.helix.gc_path
        if gc_path is None:
            return
        for path in paths:
            if not _inside_photos_dir(path):
                continue
            try:
                os.unlink(os.path.join(str(gc_path), path))
            except FileNotFoundError:
                pass
            except OSError as e:
                logging.info("HelixPrint: could not delete photo %s: %s", path, e)

    async def prune(self) -> List[str]:
        """Drop records, with their photos, whose history job is gone or that
        fall outside the storage bounds; then photos no record lists. Returns
        the record keys dropped."""
        directory = self.photos_dir()
        if self.database is None or directory is None:
            return []
        async with self._lock():
            items = await self.helix._namespace_items(PHOTOS_NAMESPACE)
            records = [(key, record, await self._history_alive(key, record))
                       for key, record in items]
            drop = prune_plan(records, self._sizes(directory))
            by_key = dict(items)
            for key in drop:
                await self._delete_files(record_paths(by_key.get(key)))
                try:
                    await self.database.delete_item(PHOTOS_NAMESPACE, [key])
                except Exception as e:
                    logging.info("HelixPrint: could not drop photo record %s: %s", key, e)
            kept = [p for key, record in items if key not in drop for p in record_paths(record)]
            try:
                names = [(e.name, e.stat().st_mtime) for e in os.scandir(directory)]
            except OSError:
                names = []
            orphans = orphan_files(names, kept, self.clock())
            await self._delete_files([f"{PHOTOS_DIR}/{n}" for n in orphans])
        if drop:
            logging.info("HelixPrint: pruned photos of %d jobs", len(drop))
        return drop
