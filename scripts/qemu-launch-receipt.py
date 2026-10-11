#!/usr/bin/env python3
"""Record the exact argument vector and immutable launch inputs before QEMU."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import memory_evidence as E


def identity(path):
    p = Path(path)
    h = hashlib.sha256()
    with p.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return {'path': str(p.resolve()), 'size': p.stat().st_size, 'sha256': h.hexdigest()}


def main():
    output, image, kernel, *argv = sys.argv[1:]
    firmware = []
    for i, arg in enumerate(argv[:-1]):
        if arg == '-bios': firmware.append(identity(argv[i + 1]))
        elif arg == '-drive' and 'if=pflash,' in argv[i + 1]:
            firmware.append(identity(argv[i + 1].split('file=', 1)[1].split(',')[0]))
    source = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    receipt = {'schema': 1, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'argv': argv, 'cwd': os.getcwd(), 'source_head': source,
        'image': identity(image), 'elf': identity(kernel), 'firmware': firmware,
        'qemu_version': subprocess.check_output([argv[0], '--version'], text=True),
        'host_schedulable_cpus': len(os.sched_getaffinity(0)),
        'fault_trace': os.environ.get('QEMU_FAULT_TRACE', '0') == '1'}
    if os.environ.get('QEMU_CANDIDATE_RECEIPT'):
        candidate = json.loads(Path(os.environ['QEMU_CANDIDATE_RECEIPT']).read_text())
        build = json.loads(Path(os.environ['QEMU_CANDIDATE_RECEIPT']).with_name('build-receipt.json').read_text())
        receipt.update(schema=2, candidate_id=E.verify_candidate(candidate, build),
            source_sha256=candidate['source_sha256'], build_receipt_sha256=candidate['build_receipt_sha256'],
            launch_id=os.environ['QEMU_LAUNCH_ID'], vm_id=os.environ['QEMU_VM_ID'],
            boot_id=os.environ['QEMU_BOOT_ID'], monotonic_ns=time.monotonic_ns(),
            host_boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip())
    path = Path(output)
    with path.open('x') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
