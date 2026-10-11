"""Canonical input identities shared by the memory collector and offline replay.

These checks establish provenance consistency, not physical-run authentication.
Build inputs use a conservative source closure (including inactive profile sources)
so a missing dependency declaration cannot hide a changed linked input.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat

ROOT = Path(__file__).resolve().parent.parent
BUILD_TOOLS = {'makefile', 'scripts/kernel-check.sh', 'scripts/check-deps.sh',
               'scripts/check-stack-usage.py', 'scripts/verify-production-test-policy.sh'}
GENERATED_SUFFIXES = {'.o', '.d', '.su', '.pyc'}


def require(value, reason):
    if not value:
        raise ValueError(reason)


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode('ascii')


def identity(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def safe(name):
    p = PurePosixPath(name)
    require(bool(name) and not p.is_absolute() and '..' not in p.parts and
            '\\' not in name and ':' not in name and str(p) == name, 'unsafe evidence path: ' + name)
    return p


def write(path, value):
    """Exclusive atomic publication; completed receipts cannot be overwritten."""
    path = Path(path)
    require(not path.exists(), 'receipt already exists: ' + str(path))
    temporary = path.with_name(path.name + '.pending')
    with temporary.open('xb') as stream:
        stream.write(canonical(value)); stream.flush(); os.fsync(stream.fileno())
    os.link(temporary, path)
    temporary.unlink()


def row(root, name):
    safe(name)
    p = root / name
    require(not p.is_symlink() and stat.S_ISREG(p.stat().st_mode), 'not a regular input: ' + name)
    return {'path': name, 'size': p.stat().st_size, 'sha256': sha(p),
            'git_mode': '100755' if p.stat().st_mode & 0o111 else '100644'}


def input_paths(root=ROOT):
    names = set(BUILD_TOOLS)
    policy = (root / 'scripts/verify-production-test-policy.sh').read_text()
    if 'scripts/device-mapping-order.py' in policy:
        names.update(('scripts/device-mapping-order.py', 'scripts/tlb_current.py',
                      'scripts/cpu_current.py', 'scripts/memory_evidence.py'))
    for prefix in ('kernel', 'bootloader', 'shared'):
        for p in (root / prefix).rglob('*'):
            if p.is_file() and p.suffix not in GENERATED_SUFFIXES:
                names.add(p.relative_to(root).as_posix())
    return sorted(names)


def manifest(root=ROOT, harness=False):
    names = (sorted(p.relative_to(root).as_posix() for p in (root / 'scripts').rglob('*')
                    if p.is_file() and p.suffix not in GENERATED_SUFFIXES)
             if harness else input_paths(root))
    return {'schema': 2, 'kind': 'harness-sources' if harness else 'build-input-closure',
            'serialization': 'sorted-compact-ascii-json-with-final-newline',
            'files': [row(root, name) for name in names]}


def verify_manifest(value, expected_digest, root=ROOT, harness=False):
    require(value.get('schema') == 2 and value.get('serialization') ==
            'sorted-compact-ascii-json-with-final-newline', 'input manifest schema/serialization')
    require(identity(value) == expected_digest, 'source manifest digest mismatch')
    rows = value['files']
    names = [r['path'] for r in rows]
    for name in names: safe(name)
    require(names == sorted(set(names)), 'input manifest duplicate or unsorted paths')
    expected = manifest(root, harness)
    require(value['kind'] == expected['kind'], 'input manifest kind')
    require(names == [r['path'] for r in expected['files']], 'missing or unlisted build-input paths')
    require(rows == expected['files'], 'source export differs from tested build inputs')


def verify_candidate(candidate, receipt, root=ROOT):
    require(candidate.get('schema') == 2 and receipt.get('schema') == 2, 'frozen candidate receipt schema')
    verify_manifest(candidate['build_inputs'], candidate['source_sha256'], root)
    require(candidate['source_sha256'] == receipt['build_input_sha256'], 'candidate/build source binding')
    require(identity(candidate['harness_inputs']) == candidate['harness_sha256'], 'harness manifest digest')
    require(candidate['artifacts'] == receipt['artifacts'] and candidate['profile'] == receipt['profile'],
            'candidate/build artifact or option mismatch')
    require(identity(receipt) == candidate['build_receipt_sha256'], 'build receipt digest mismatch')
    require(receipt['status'] == 'PASS' and receipt['inputs_before'] == receipt['build_input_sha256'],
            'build inputs changed during compilation')
    require(receipt['payload']['status'] == 'PASS' and
            receipt['payload']['image_sha256'] == candidate['artifacts']['hobbyos.img']['sha256'], 'image payload binding')
    payload = receipt['payload']['files']
    require(set(payload) == {'kernel.elf', 'BOOTX64.EFI', 'zap-light16.psf', 'logo.bmp', 'startup.nsh'},
            'image payload coverage')
    for name in ('kernel.elf', 'BOOTX64.EFI'):
        require(payload[name] == candidate['artifacts'][name], 'compiled image payload mismatch')
    for name, source in [('zap-light16.psf', 'bootloader/fonts/zap-light16.psf'),
                         ('logo.bmp', 'bootloader/images/logo.bmp'), ('startup.nsh', 'bootloader/startup.nsh')]:
        require(payload[name] == {'size': (root / source).stat().st_size, 'sha256': sha(root / source)},
                'image asset differs from source export')
    return identity(candidate)
