# SPDX-License-Identifier: GPL-3.0-or-later
"""
AD5X IFS vendor push: Adventurer5M.json, pushed when it changes.

The app's AD5X IFS backend on native ZMOD downloads Adventurer5M.json from
the config root every 5 s idle and 30 s printing, to catch colour and
material edits the printer's own dialog makes without a G-code echo. This
module watches the file where it lives, at the cost of a stat, and
broadcasts its text as the vendor `ad5x` payload, {"file", "content"}, only
when the text changed. The app applies the content exactly as a poll that
saw a change.

Design: docs/devel/plans/2026-10-20-helixprint-plugin-expansion-design.md,
Phase 11. Python 3.8, stdlib only.
"""

from __future__ import annotations

import logging
import os
import stat
from typing import Any, Dict, Optional, Tuple

from helix_vendor_push import VendorPush, Watcher

AD5X_VENDOR = "ad5x"
AD5X_FEATURE = "vendor_ad5x"
AD5X_FEATURE_VERSION = 1

ADVENTURER_JSON = "Adventurer5M.json"
ADVENTURER_ROOT = "config"

# A stat per interval; the file is read only when its stat changes.
CHECK_INTERVAL_S = 1.0

# Ignored rather than pushed: a real Adventurer5M.json is a few hundred bytes,
# and a file this size is not one.
MAX_CONTENT_BYTES = 256 * 1024

NO_FILE_REASON = "no_adventurer_json"


def file_signature(path: str) -> Optional[Tuple[int, int]]:
    """(mtime_ns, size) of a regular file, or None when there is none."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    if not stat.S_ISREG(st.st_mode):
        return None
    return st.st_mtime_ns, st.st_size


def ad5x_payload(content: str) -> Dict[str, Any]:
    return {"file": ADVENTURER_JSON, "content": content}


def names_adventurer_json(payload: Any) -> bool:
    """Whether a file_manager:filelist_changed payload is about the file."""
    if not isinstance(payload, dict):
        return False
    item = payload.get("item")
    if not isinstance(item, dict):
        return False
    return item.get("root") == ADVENTURER_ROOT and item.get("path") == ADVENTURER_JSON


class HelixVendorAd5x:
    """Pushes Adventurer5M.json on change. Created and started by
    helix_print.py."""

    def __init__(self, helix: Any) -> None:
        self.helix = helix
        self.server = helix.server
        self.push = VendorPush(self.server, AD5X_VENDOR)
        self.watcher = Watcher("vendor_ad5x", CHECK_INTERVAL_S, self.check)
        self._signature: Optional[Tuple[int, int]] = None
        config_dir = ""
        file_manager = helix.file_manager
        if file_manager is not None:
            config_dir = file_manager.get_directory(ADVENTURER_ROOT) or ""
        self.path = os.path.join(config_dir, ADVENTURER_JSON) if config_dir else ""

    def availability(self) -> Tuple[bool, Optional[str]]:
        """Available while the file exists: a printer without it is not an
        AD5X on ZMOD."""
        if self.path and file_signature(self.path) is not None:
            return True, None
        return False, NO_FILE_REASON

    def start(self) -> None:
        self.server.register_event_handler(
            "file_manager:filelist_changed", self.on_filelist_changed
        )
        self.helix.eventloop.register_callback(self.watcher.run)

    def close(self) -> None:
        self.watcher.close()

    async def on_filelist_changed(self, payload: Any, *args: Any) -> None:
        if names_adventurer_json(payload):
            await self.check()

    async def check(self) -> None:
        """Push the file's text when its stat moved and the text changed."""
        if not self.path:
            return
        signature = file_signature(self.path)
        if signature is None or signature == self._signature:
            self._signature = signature
            return
        self._signature = signature
        if signature[1] > MAX_CONTENT_BYTES:
            return
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()
        except OSError as e:
            logging.debug("HelixPrint: %s unreadable: %s", ADVENTURER_JSON, e)
            self._signature = None
            return
        self.push.emit_if_changed(ad5x_payload(content))
