"""Game images and savestates; none of them are in the repository.

GDXSV_ROM_DIR holds gdx-disc1/gdx-disc1.gdi, gdx-disc2/gdx-disc2.gdi and the NAOMI set
(naomi.zip, gundmxgd.zip, gundmxgd/gdl-0006.chd). GDXSV_DISC1, GDXSV_DISC2, GDXSV_NAOMI
override single files. GDXSV_STATE_DIR keeps the boot savestates (<rom>_99.state),
downloaded when missing (default gdxsv_tests/out/state).
"""

import os
from typing import Optional

from . import emu

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)

ROMS = {
    "dc1": ("GDXSV_DISC1", os.path.join("gdx-disc1", "gdx-disc1.gdi")),
    "dc2": ("GDXSV_DISC2", os.path.join("gdx-disc2", "gdx-disc2.gdi")),
    "naomi": ("GDXSV_NAOMI", "gundmxgd.zip"),
}


def rom(name: str) -> Optional[str]:
    """Path of a game image, None if it is not on this machine."""
    var, rel = ROMS[name]
    path = os.environ.get(var)
    if not path and os.environ.get("GDXSV_ROM_DIR"):
        path = os.path.join(os.environ["GDXSV_ROM_DIR"], rel)
    return os.path.abspath(path) if path and os.path.isfile(path) else None


def state_dir(rom_path: str) -> str:
    """A directory with only the boot savestate of rom_path (emu.launch copies all *.state)."""
    root = os.environ.get("GDXSV_STATE_DIR") or os.path.join(TESTS, "out", "state")
    emu.ensure_state(root, rom_path)
    name = os.path.splitext(os.path.basename(rom_path))[0] + "_99.state"
    sub = os.path.join(root, "only", name[:-len(".state")])
    if not os.path.isfile(os.path.join(sub, name)):
        os.makedirs(sub, exist_ok=True)
        with open(os.path.join(root, name), "rb") as src, open(os.path.join(sub, name), "wb") as dst:
            dst.write(src.read())
    return sub
