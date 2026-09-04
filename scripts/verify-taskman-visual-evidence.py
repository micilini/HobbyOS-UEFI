#!/usr/bin/env python3
"""Verify active TASKMAN visual evidence and image payload continuity."""

from __future__ import annotations

import filecmp
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = ROOT / "artifacts/build/taskman-visual-final"
IMAGE = ROOT / "hobbyos.img"
KERNEL_HASH = "ca630b9f282016ddcfd832c3b130011b41232ed6b7f0c667663f6850e628db8c"
IMAGE_SIZE = 67_108_864
PAYLOADS = (
    (
        "kernel.elf", "::/kernel.elf", ROOT / "kernel.elf",
        "ca630b9f282016ddcfd832c3b130011b41232ed6b7f0c667663f6850e628db8c",
    ),
    (
        "BOOTX64.EFI", "::/EFI/BOOT/BOOTX64.EFI", ROOT / "BOOTX64.EFI",
        "3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66",
    ),
    (
        "zap-light16.psf", "::/EFI/fonts/zap-light16.psf",
        ROOT / "bootloader/fonts/zap-light16.psf",
        "5296332bcf01bc21a2177cc17c01944ba688420f188ca61d0d42acffda2c942b",
    ),
    (
        "logo.bmp", "::/EFI/images/logo.bmp",
        ROOT / "bootloader/images/logo.bmp",
        "a89522f57573bc4fb3e7ff9ab25fa2a8e2f7c3b2ec157350389133df5480707a",
    ),
    (
        "startup.nsh", "::/startup.nsh", ROOT / "bootloader/startup.nsh",
        "3b6565db1dc01469b24c077a77f3c28a5ce596556d0585bf017069252cff5e35",
    ),
)


class VerificationError(RuntimeError):
    pass


class ImageContainerError(VerificationError):
    pass


class ImagePayloadError(VerificationError):
    pass


class KernelContinuityError(VerificationError):
    pass


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def recorded_hashes() -> dict[str, str]:
    values: dict[str, str] = {}
    path = ARTIFACT / "final-hashes.txt"
    for line in path.read_text(encoding="utf-8").splitlines():
        value, name = line.split(maxsplit=1)
        values[Path(name).name] = value
    return values


def verify_active_evidence() -> list[str]:
    checks: list[str] = []
    naming = (ARTIFACT / "naming.log").read_text(encoding="utf-8")
    require("[TASKMAN][ACTIVE_NAMING] PASS" in naming, "naming gate missing")
    checks.append("active naming")

    smoke = (ARTIFACT / "smoke.log").read_text(
        encoding="utf-8", errors="replace"
    )
    for marker in (
        "[TASKMAN][FINAL_SMOKE] PASS",
        "[TASKMAN][WIDE_SMOKE] PASS full_frames=3",
        "[TASKMAN][COMPACT_SMOKE] PASS full_frames=3",
    ):
        require(marker in smoke, f"smoke marker missing: {marker}")

    serial = (ARTIFACT / "smoke-serial.log").read_text(
        encoding="utf-8", errors="replace"
    )
    for marker in (
        "[INPUTTEST][CHECK] PASS",
        "[MODALTEST][CHECK] PASS",
        "[TASKDIAG][CHECK] PASS",
        "[TASKMANTEST][CHECK] PASS",
    ):
        require(marker in serial, f"guest check missing: {marker}")
    for field in (
        "fallback_frames=0", "stable_frame_full_clears=0", "scroll_delta=0",
        "clipped=0", "workspace_live=0", "stale_cells=0",
    ):
        require(field in serial, f"smoke invariant missing: {field}")
    for fault in (
        "PANIC", "#PF", "#GP", "FATAL", "STRUCTURAL_FAULT", "FINISH_FAULT",
    ):
        require(fault not in smoke and fault not in serial,
                f"fault marker present: {fault}")
    checks.append("focused smoke")
    return checks


def verify_kernel(hashes: dict[str, str]) -> str:
    path = ROOT / "kernel.elf"
    if not path.is_file():
        raise KernelContinuityError("missing kernel.elf")
    value = digest(path)
    if value != KERNEL_HASH or hashes.get("kernel.elf") != KERNEL_HASH:
        raise KernelContinuityError("kernel hash mismatch")
    return value


def verify_container(hashes: dict[str, str]) -> dict[str, object]:
    if not IMAGE.is_file():
        raise ImageContainerError("missing hobbyos.img")
    if IMAGE.stat().st_size != IMAGE_SIZE:
        raise ImageContainerError("unexpected image size")
    with IMAGE.open("rb") as stream:
        boot = stream.read(512)
    if len(boot) != 512 or boot[510:512] != b"\x55\xaa":
        raise ImageContainerError("invalid boot signature")
    if boot[82:90] != b"FAT32   ":
        raise ImageContainerError("filesystem marker is not FAT32")
    bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
    total_short = struct.unpack_from("<H", boot, 19)[0]
    total_long = struct.unpack_from("<I", boot, 32)[0]
    total_sectors = total_short or total_long
    if bytes_per_sector != 512 or total_sectors != 131_072:
        raise ImageContainerError("unexpected FAT geometry")
    raw_hash = digest(IMAGE)
    if hashes.get("hobbyos.img") != raw_hash:
        raise ImageContainerError("recorded raw image hash mismatch")
    serial = struct.unpack_from("<I", boot, 67)[0]
    return {
        "raw_image_sha256": raw_hash,
        "volume_serial": f"0x{serial:08x}",
        "filesystem": "FAT32",
        "size": IMAGE_SIZE,
        "bytes_per_sector": bytes_per_sector,
        "total_sectors": total_sectors,
    }


def verify_payloads() -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="taskman-image-payload-") as directory:
        output_dir = Path(directory)
        for name, image_path, source, expected in PAYLOADS:
            if not source.is_file() or digest(source) != expected:
                raise ImagePayloadError(f"source payload mismatch: {source}")
            extracted = output_dir / name
            try:
                subprocess.run(
                    ["mcopy", "-i", str(IMAGE), image_path, str(extracted)],
                    cwd=ROOT, check=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True,
                )
            except (OSError, subprocess.CalledProcessError) as error:
                detail = getattr(error, "stderr", None) or str(error)
                raise ImagePayloadError(
                    f"cannot extract {image_path}: {detail.strip()}"
                ) from error
            extracted_hash = digest(extracted)
            if extracted_hash != expected or not filecmp.cmp(
                source, extracted, shallow=False
            ):
                raise ImagePayloadError(f"image payload mismatch: {image_path}")
            results.append({
                "name": name,
                "image_path": image_path,
                "source": str(source.relative_to(ROOT)),
                "size": source.stat().st_size,
                "sha256": expected,
                "identical": True,
            })
    return results


def write_outputs(checks: list[str], kernel_hash: str,
                  container: dict[str, object],
                  payloads: list[dict[str, object]]) -> None:
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    manifest = [
        f"raw_image_sha256={container['raw_image_sha256']}",
        f"volume_serial={container['volume_serial']}",
        f"filesystem={container['filesystem']}",
        f"size={container['size']}",
        f"bytes_per_sector={container['bytes_per_sector']}",
        f"total_sectors={container['total_sectors']}",
        f"kernel_sha256={kernel_hash}",
    ]
    manifest.extend(
        f"payload.{item['name']}.sha256={item['sha256']}" for item in payloads
    )
    (ARTIFACT / "payload-manifest.txt").write_text(
        "\n".join(manifest) + "\n", encoding="utf-8"
    )
    markers = [
        "[TASKMAN][IMAGE_CONTAINER] PASS",
        f"[TASKMAN][IMAGE_PAYLOAD] PASS files={len(payloads)}",
        f"[TASKMAN][BINARY_CONTINUITY] PASS kernel=1 payload={len(payloads)}",
        "[TASKMAN][VISUAL_EVIDENCE] PASS",
    ]
    (ARTIFACT / "verification.log").write_text(
        "\n".join(markers) + "\n", encoding="utf-8"
    )
    payload = {
        "result": "PASS",
        "checks": checks,
        "kernel_sha256": kernel_hash,
        "container": container,
        "payloads": payloads,
        "human_visual_review": "AWAITING_HUMAN_REVIEW",
    }
    (ARTIFACT / "verification.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )
    for marker in markers:
        print(marker)
    print(f"raw_image_sha256={container['raw_image_sha256']}")
    print(f"volume_serial={container['volume_serial']}")


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] != "all":
        print("usage: verify-taskman-visual-evidence.py all", file=sys.stderr)
        return 2
    try:
        checks = verify_active_evidence()
        hashes = recorded_hashes()
        kernel_hash = verify_kernel(hashes)
        container = verify_container(hashes)
        payloads = verify_payloads()
        checks.extend(("kernel continuity", "image container", "image payload"))
        checklist = ROOT / "docs/test-reports/TASKMAN_VISUAL_HUMAN_CHECKLIST.md"
        text = checklist.read_text(encoding="utf-8")
        require("STATUS: AWAITING_HUMAN_REVIEW" in text,
                "human gate not pending")
        require("[x]" not in text.lower(), "human item marked automatically")
        checks.append("pending human review")
        write_outputs(checks, kernel_hash, container, payloads)
    except ImageContainerError as error:
        print(f"REJECTED_TASKMAN_IMAGE_CONTAINER: {error}", file=sys.stderr)
        return 1
    except ImagePayloadError as error:
        print(f"REJECTED_TASKMAN_IMAGE_PAYLOAD: {error}", file=sys.stderr)
        return 1
    except KernelContinuityError as error:
        print(f"REJECTED_TASKMAN_KERNEL_CONTINUITY: {error}", file=sys.stderr)
        return 1
    except (OSError, ValueError, VerificationError) as error:
        print(f"REJECTED_TASKMAN_VISUAL_EVIDENCE: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
