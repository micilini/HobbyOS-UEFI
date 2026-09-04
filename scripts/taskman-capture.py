#!/usr/bin/env python3
"""Validate active TASKMAN visual captures without image libraries."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

FOCUSED_CAPTURE_NAMES = ("wide.ppm", "compact.ppm")


class CaptureError(ValueError):
    """The capture bytes or recorded scenario do not satisfy the contract."""


def token(stream) -> bytes:
    value = bytearray()
    while True:
        byte = stream.read(1)
        if not byte:
            raise CaptureError("truncated PPM header")
        if byte == b"#":
            stream.readline()
            continue
        if not byte.isspace():
            value.extend(byte)
            break
    while True:
        byte = stream.read(1)
        if not byte or byte.isspace():
            break
        value.extend(byte)
    return bytes(value)


def metadata(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise CaptureError(f"missing metadata: {path}")
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    return fields


def number(fields: dict[str, str], name: str, path: Path) -> int:
    try:
        return int(fields[name])
    except (KeyError, ValueError) as error:
        raise CaptureError(f"invalid {name} for {path}") from error


def validate(path: Path) -> dict[str, object]:
    expected_mode = "COMPACT" if path.name == "compact.ppm" else "WIDE"
    expected_cols = 76 if expected_mode == "COMPACT" else 118
    fields = metadata(Path(f"{path}.meta.txt"))
    required = {
        "base", "commit", "smp", "accel", "requested_cols",
        "requested_rows", "actual_mode", "refresh", "fixture", "page",
        "selected_pid", "full_frames", "fallback_frames", "captured",
        "pages", "cleanup", "sha256",
    }
    missing = sorted(required - fields.keys())
    if missing:
        raise CaptureError(f"metadata fields missing for {path}: {missing}")
    if fields["actual_mode"] != expected_mode:
        raise CaptureError(
            f"{path.name} mode={fields['actual_mode']} expected={expected_mode}"
        )
    if number(fields, "requested_cols", path) != expected_cols:
        raise CaptureError(f"wrong requested width for {path}")
    if number(fields, "requested_rows", path) != 40:
        raise CaptureError(f"wrong requested height for {path}")
    if number(fields, "full_frames", path) < 3:
        raise CaptureError(f"insufficient full frames for {path}")
    if number(fields, "fallback_frames", path) != 0:
        raise CaptureError(f"fallback frame recorded for {path}")
    if number(fields, "captured", path) <= 0 or number(fields, "pages", path) < 1:
        raise CaptureError(f"empty TASKMAN model for {path}")
    if fields["cleanup"] != "PASS":
        raise CaptureError(f"cleanup did not pass for {path}")

    with path.open("rb") as stream:
        magic = token(stream)
        width = int(token(stream))
        height = int(token(stream))
        maximum = int(token(stream))
        pixels = stream.read()
    if magic != b"P6" or width <= 0 or height <= 0 or maximum != 255:
        raise CaptureError(f"invalid PPM header: {path}")
    if len(pixels) != width * height * 3:
        raise CaptureError(f"incomplete raster: {path}")
    first = pixels[:3]
    if not any(
        pixels[index:index + 3] != first
        for index in range(3, len(pixels), 3)
    ):
        raise CaptureError(f"uniform capture: {path}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if fields["sha256"] != digest:
        raise CaptureError(f"metadata hash mismatch for {path}")
    return {
        "path": str(path), "mode": expected_mode,
        "width": width, "height": height, "sha256": digest,
    }


def main() -> int:
    directory = Path(
        sys.argv[1]
        if len(sys.argv) > 1
        else "artifacts/build/taskman-visual-final/screens"
    )
    try:
        results = [validate(directory / name) for name in FOCUSED_CAPTURE_NAMES]
    except (OSError, CaptureError) as error:
        print(f"REJECTED_TASKMAN_VISUAL_CAPTURE: {error}", file=sys.stderr)
        return 1
    output = directory.parent / "captures-validation.json"
    output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"[TASKMAN][CAPTURES] PASS count={len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
