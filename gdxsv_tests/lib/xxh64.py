"""XXH64 in pure Python (the texture pack's chunk and file hashes); standard library only."""

import struct

M = (1 << 64) - 1
P1 = 11400714785074694791
P2 = 14029467366897019727
P3 = 1609587929392839161
P4 = 9650029242287828579
P5 = 2870177450012600261


def _rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & M


def _round(acc: int, v: int) -> int:
    return (_rotl((acc + v * P2) & M, 31) * P1) & M


def _merge(h: int, v: int) -> int:
    return ((h ^ _round(0, v)) * P1 + P4) & M


def xxh64(data: bytes, seed: int = 0) -> int:
    n = len(data)
    i = 0
    if n >= 32:
        v = [(seed + P1 + P2) & M, (seed + P2) & M, seed & M, (seed - P1) & M]
        while i + 32 <= n:
            for k in range(4):
                v[k] = _round(v[k], struct.unpack_from("<Q", data, i + 8 * k)[0])
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M
        for x in v:
            h = _merge(h, x)
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        h = ((_rotl(h ^ _round(0, struct.unpack_from("<Q", data, i)[0]), 27) * P1) + P4) & M
        i += 8
    if i + 4 <= n:
        h = ((_rotl(h ^ ((struct.unpack_from("<I", data, i)[0] * P1) & M), 23) * P2) + P3) & M
        i += 4
    while i < n:
        h = (_rotl(h ^ ((data[i] * P5) & M), 11) * P1) & M
        i += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


def hexdigest(data: bytes) -> str:
    """The texture updater's format: 16 lowercase hex digits."""
    return f"{xxh64(data):016x}"
