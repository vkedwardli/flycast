"""run.py --recheck: each case runs its checks on a copy of a saved out/<case> directory and
launches nothing. emu.launch, servers.Lobby and servers.FakeHttp then read back what the
run being rechecked saved; a record that run did not save is listed in the case's result."""

import json
import os
import threading
from typing import Optional

ACTIVE = False
CASE = threading.local()  # the case this thread runs: dir, records counted, records missing


def begin(case_dir: str) -> None:
    CASE.dir, CASE.counts, CASE.missing = case_dir, {}, []


def missing() -> list:
    return list(getattr(CASE, "missing", []))


def record_path(kind: str) -> Optional[str]:
    """<case dir>/<kind>_<n>.json for the n-th record of this kind in the case."""
    d = getattr(CASE, "dir", None)
    if d is None:
        return None
    n = CASE.counts.get(kind, 0)
    CASE.counts[kind] = n + 1
    return os.path.join(d, f"{kind}_{n}.json")


def load(path: Optional[str], default):
    """The saved record; in a recheck, a missing one is noted and `default` returned."""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, TypeError, ValueError):
        if ACTIVE and hasattr(CASE, "missing"):
            CASE.missing.append(os.path.relpath(path, CASE.dir) if path else "?")
        return default


def save(path: Optional[str], value) -> None:
    if path and not ACTIVE:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(value, f, indent=1)


class Done:
    """Stands in for the finished subprocess.Popen of a recorded instance."""
    pid = 0

    def __init__(self, rc: Optional[int]):
        self.returncode = rc

    def poll(self) -> Optional[int]:
        return self.returncode

    def wait(self, timeout=None) -> Optional[int]:
        return self.returncode
