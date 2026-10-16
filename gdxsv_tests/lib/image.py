"""Screen grabs of an emulator window and compares with the small images in golden/ (Pillow)."""

import os
import threading
from typing import Optional, Tuple

from . import emu

GOLDEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "golden")
# One grabbing case at a time (run.py --jobs): each raises its windows over the others'.
SCREEN_LOCK = threading.Lock()


def grab(pid: int, path: str) -> Optional[str]:
    """Save pid's window client area to path; None if it has no visible window."""
    from PIL import ImageGrab
    box = emu.window_rect(pid)
    if box is None:
        return None
    ImageGrab.grab(bbox=box, all_screens=True).save(path)
    return path


def white_mask(path: str, box: Tuple[int, int, int, int], size: Tuple[int, int]):
    """A 1-bit image of the near-white pixels (text) in box of the image at path,
    the image first scaled to size (the size the box is given in)."""
    from PIL import Image, ImageChops
    im = Image.open(path).convert("RGB")
    if im.size != size:
        im = im.resize(size)
    r, g, b = im.crop(box).split()
    low = ImageChops.darker(ImageChops.darker(r, g), b)
    return low.point(lambda v: 255 if v > 200 else 0).convert("1")


def recall(golden_name: str, mask) -> float:
    """The share of golden/<golden_name>'s white pixels that are white in mask."""
    from PIL import Image
    g = Image.open(os.path.join(GOLDEN, golden_name)).convert("1")
    assert g.size == mask.size, (g.size, mask.size)
    gw = [i for i, v in enumerate(g.convert("L").tobytes()) if v]
    m = mask.convert("L").tobytes()
    return sum(1 for i in gw if m[i]) / len(gw) if gw else 0.0
