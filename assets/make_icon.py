#!/usr/bin/env python3
"""Generates a simple 128x128 TreeCapacitator icon as icon.png.

Pure-stdlib PNG writer (no external dependencies).
"""
import struct
import zlib
from pathlib import Path


def png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def make_icon(path: Path, size: int = 128) -> None:
    # RGBA background: deep woodland green.
    bg = (0x0E, 0x31, 0x1E)
    # Trunk brown.
    trunk = (0x6B, 0x3F, 0x1D)
    # Leaf greens.
    leaf_dark = (0x22, 0x7A, 0x3E)
    leaf_mid = (0x35, 0xA8, 0x52)
    leaf_light = (0x5C, 0xCE, 0x78)
    # Capacitor spark (cyan).
    spark = (0x4F, 0xE3, 0xE0)

    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0
        for x in range(size):
            # Branches / trunk
            in_trunk = abs(x - size // 2) < size // 12 and y > size // 3
            # Canopy ellipse
            cy = y - size * 0.28
            cx = x - size // 2
            canopy = (cx / (size * 0.30)) ** 2 + (cy / (size * 0.30)) ** 2 <= 1

            color = bg
            if in_trunk:
                color = trunk
            elif canopy:
                # Per-pixel deterministic zigzag for leaf texture.
                hashv = ((x * 7 + y * 13 + x * y) % 3)
                if hashv == 0:
                    color = leaf_dark
                elif hashv == 1:
                    color = leaf_mid
                else:
                    color = leaf_light
                # Capacitor cells: small cyan sparks in the canopy.
                if x % 24 in (0, 1) and y % 24 in (0, 1):
                    color = spark
            raw.extend(color)
            raw.append(255)

    defs = b"\x08\x06\x00\x00\x00\x00\x00"  # 8-bit RGBA
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", idat)
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)
    print(f"Wrote {path} ({len(png)} bytes)")


if __name__ == "__main__":
    make_icon(Path(__file__).resolve().parent / "icon.png")