"""Reads the few fields of a replay file (BattleLogFile, core/gdxsv/gdxsv.proto) the tests need,
without a protobuf dependency."""

from typing import Iterator, List, Tuple

# Field numbers from core/gdxsv/gdxsv.pb.h
ROUND_DATA = 18  # BattleLogFile.round_data (BattleLogRound)
WIN_TEAM = 1  # BattleLogRound.win_team (int32; -1 = draw)
INPUTS = 15  # BattleLogFile.inputs (repeated fixed64)
START_MSG_INDEXES = 16  # BattleLogFile.start_msg_indexes (repeated int32)


def _varint(b: bytes, i: int) -> Tuple[int, int]:
    v = shift = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7f) << shift
        shift += 7
        if c < 0x80:
            return v, i


def fields(b: bytes) -> Iterator[Tuple[int, int, object]]:
    """(field number, wire type, value) of one message; length-delimited values are bytes."""
    i = 0
    while i < len(b):
        key, i = _varint(b, i)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _varint(b, i)
        elif wt == 1:
            v, i = b[i:i + 8], i + 8
        elif wt == 2:
            n, i = _varint(b, i)
            v, i = b[i:i + n], i + n
        elif wt == 5:
            v, i = b[i:i + 4], i + 4
        else:
            raise ValueError(f"wire type {wt} at {i}")
        yield num, wt, v


def win_teams(path: str) -> List[int]:
    """The winner the players saw in each round (empty for formats without round_data)."""
    with open(path, "rb") as f:
        data = f.read()
    out = []
    for num, wt, v in fields(data):
        if num == ROUND_DATA and wt == 2:
            win = 0
            for n, w, x in fields(v):
                if n == WIN_TEAM and w == 0:
                    win = x - (1 << 64) if x >= 1 << 63 else x
            out.append(win)
    return out


def _packed(v) -> List[int]:
    vals, i = [], 0
    while i < len(v):
        x, i = _varint(v, i)
        vals.append(x)
    return vals


def start_indexes(path: str) -> List[int]:
    """Index of the first input of each round."""
    with open(path, "rb") as f:
        data = f.read()
    out = []
    for num, wt, v in fields(data):
        if num == START_MSG_INDEXES:
            out += _packed(v) if wt == 2 else [v]
    return out


def _put_varint(v: int) -> bytes:
    out = bytearray()
    while True:
        c = v & 0x7f
        v >>= 7
        if not v:
            out.append(c)
            return bytes(out)
        out.append(c | 0x80)


def truncate_inputs(src: str, dst: str, keep: int) -> None:
    """Copy of a replay file with only its first `keep` inputs."""
    with open(src, "rb") as f:
        data = f.read()
    out = bytearray()
    for num, wt, v in fields(data):
        if num == INPUTS and wt == 2:  # packed
            v = v[:8 * keep]
        elif num == INPUTS and wt == 1:
            keep -= 1
            if keep < 0:
                continue
        out += _put_varint(num << 3 | wt)
        if wt == 0:
            out += _put_varint(v)
        elif wt == 2:
            out += _put_varint(len(v)) + v
        else:
            out += v
    with open(dst, "wb") as f:
        f.write(out)
