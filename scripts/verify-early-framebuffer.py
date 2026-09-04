#!/usr/bin/env python3
"""Pixel-only validation for the early framebuffer bind and clear."""

from __future__ import annotations

import argparse
from pathlib import Path


def read_token(stream) -> bytes:
    while True:
        byte = stream.read(1)
        if not byte:
            raise ValueError("unexpected end of PPM")
        if byte == b"#":
            stream.readline()
            continue
        if not byte.isspace():
            data = bytearray(byte)
            while True:
                byte = stream.read(1)
                if not byte or byte.isspace():
                    return bytes(data)
                data.extend(byte)


def load_ppm(path: Path) -> tuple[int, int, bytes]:
    with path.open("rb") as stream:
        magic = read_token(stream)
        if magic != b"P6":
            raise ValueError(f"{path}: expected binary P6 PPM")
        width = int(read_token(stream))
        height = int(read_token(stream))
        maximum = int(read_token(stream))
        if width <= 0 or height <= 0 or maximum != 255:
            raise ValueError(f"{path}: invalid dimensions or max value")
        pixels = stream.read()
    expected = width * height * 3
    if len(pixels) != expected:
        raise ValueError(f"{path}: pixel length {len(pixels)} != {expected}")
    return width, height, pixels


def changed_pixels(left: bytes, right: bytes) -> int:
    return sum(
        left[index : index + 3] != right[index : index + 3]
        for index in range(0, len(left), 3)
    )


def validate(before_path: Path, early_path: Path, final_path: Path) -> None:
    before_w, before_h, before = load_ppm(before_path)
    early_w, early_h, early = load_ppm(early_path)
    final_w, final_h, final = load_ppm(final_path)
    if (before_w, before_h) != (early_w, early_h) or (
        early_w,
        early_h,
    ) != (final_w, final_h):
        raise ValueError("framebuffer dimensions changed between captures")

    total = early_w * early_h
    changed = changed_pixels(before, early)
    if changed < max(32, total // 1000):
        raise ValueError("early frame is pixel-identical or nearly identical to UEFI")

    upper_rows = max(1, early_h // 4)
    upper = early[: upper_rows * early_w * 3]
    background = b"\x00\x00\x00"
    background_pixels = sum(
        upper[index : index + 3] == background
        for index in range(0, len(upper), 3)
    )
    upper_total = early_w * upper_rows
    if background_pixels * 100 < upper_total * 70:
        raise ValueError("early upper region is not predominantly cleared")

    sample_colors = {
        final[index : index + 3]
        for index in range(0, len(final), 3 * max(1, total // 20000))
    }
    if len(sample_colors) < 2:
        raise ValueError("final shell capture is uniform")
    if changed_pixels(early, final) < max(32, total // 1000):
        raise ValueError("final shell capture did not visibly advance")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("early", type=Path)
    parser.add_argument("final", type=Path)
    args = parser.parse_args()
    try:
        validate(args.before, args.early, args.final)
    except (OSError, ValueError) as error:
        print(f"[GRAPHICS][EARLY_CLEAR] FAIL reason={error}")
        return 1
    print("[GRAPHICS][EARLY_CLEAR] PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
