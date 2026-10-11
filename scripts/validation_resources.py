"""Resource admission for the existing gates; receipts are never completion flags.

The semantic callback is mandatory. This module does not grant acceptance, repair
receipts, or retire originals. Failed reuse requires a new evidence destination.
"""
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import memory_evidence as E

MIB = 1024 ** 2
GIB = 1024 ** 3
RESERVE = 64 * MIB
NATIVE_FLOOR = 4 * GIB


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')


def classify(path, verify, *, source, harness=None, schema=1,
             source_key='build_input_sha256', harness_key='harness_sha256', header=None):
    """Verify source closure/raw evidence using the stage's native predicate.

    Presence, status, and hashes alone cannot produce REUSABLE. Stale or invalid
    files remain in place; the caller must use a distinct successor destination.
    """
    path = Path(path)
    result = {'path': str(path), 'state': 'MISSING', 'reason': 'receipt absent'}
    if not path.exists():
        return result
    if path.is_symlink() or not path.is_file():
        return dict(result, state='INVALID', reason='receipt is not a regular file')
    result['sha256'] = E.sha(path)
    try:
        row = json.loads(path.read_text())
        identity = header(row) if header else row
        E.require(isinstance(identity, dict) and identity.get('schema') == schema and
                  identity.get('status') == 'PASS', 'receipt schema/status is incomplete')
        E.require(isinstance(identity.get(source_key), str), 'receipt source identity absent')
        if identity[source_key] != source:
            return dict(result, state='STALE', reason='linked input identity differs')
        if harness is not None and identity.get(harness_key) != harness:
            return dict(result, state='STALE', reason='required verifier identity differs')
        verified = verify(row)
        E.require(verified is not None, 'semantic validator supplied no result')
    except FileNotFoundError as error:
        return dict(result, state='PARTIAL', reason='raw dependency missing: ' + str(error))
    except (ValueError, KeyError, TypeError, OSError, AssertionError, subprocess.SubprocessError) as error:
        return dict(result, state='INVALID', reason=str(error))
    return dict(result, state='REUSABLE', reason='source, raw dependencies and native predicate verified')


def require_successor(state):
    E.require(state['state'] in ('MISSING', 'REUSABLE'),
              'preserved nonreusable stage requires a new successor destination: ' +
              json.dumps(state, sort_keys=True))
    return state['state'] == 'REUSABLE'


def filesystem(path):
    requested = Path(path).absolute()
    existing = requested
    while not existing.exists():
        E.require(existing != existing.parent, 'no existing resource filesystem')
        existing = existing.parent
    resolved = existing.resolve()
    stats = os.statvfs(resolved)
    usage = shutil.disk_usage(resolved)
    mount = None
    # Mount identity is descriptive. Available bytes/inodes come from the actual
    # target filesystem, including user-reserved space reported by statvfs.
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        left, right = line.split(' - ', 1)
        parts = left.split(); tail = right.split()
        point = Path(parts[4].replace('\\040', ' ').replace('\\134', '\\'))
        if resolved.is_relative_to(point) and (mount is None or len(str(point)) > len(mount['mount'])):
            mount = {'mount': str(point), 'device': tail[1], 'type': tail[0],
                     'options': parts[5] + ',' + tail[2]}
    return {'requested': str(requested), 'resolved': str(resolved), 'device_id': os.stat(resolved).st_dev,
            'free_bytes': min(usage.free, stats.f_bavail * stats.f_frsize),
            'free_inodes': stats.f_favail, 'read_only': bool(stats.f_flag & os.ST_RDONLY),
            'mount': mount, 'quota': 'NOT_INDEPENDENTLY_QUERIED'}


def configure_scratch(path):
    """One explicit filesystem for replay and all inherited compiler temporaries."""
    path = Path(path).absolute()
    E.require(not any(p.is_symlink() for p in (path, *path.parents)), 'scratch path contains symlink')
    path.mkdir(parents=True, exist_ok=True)
    E.require(not filesystem(path)['read_only'], 'read-only scratch path')
    os.environ['HOBBYOS_REVIEW_SCRATCH'] = str(path)
    os.environ['TMPDIR'] = str(path)
    tempfile.tempdir = str(path)
    return str(path)


def admit(evidence, *, stages, durable, temporary=0, scratch=0, packaging=0,
          scratch_path=None, working_path=None, minimum=0, required_inodes=4096,
          purpose='complete-campaign'):
    """Sum accumulating output and one live work/replay footprint by filesystem.

    Estimates are not reservations. Every expensive stage/boot checks again.
    Only a semantically reusable stage removes its declared durable allowance.
    """
    E.require(set(durable) == set(stages), 'resource estimate/stage inventory differs')
    E.require(all(isinstance(n, int) and n >= 0 for n in
                  [*durable.values(), temporary, scratch, packaging, minimum, required_inodes]),
              'invalid resource estimate')
    pending = {name: size for name, size in durable.items() if stages[name]['state'] != 'REUSABLE'}
    evidence = Path(evidence)
    scratch_path = Path(scratch_path or os.environ.get('HOBBYOS_REVIEW_SCRATCH') or
                        os.environ.get('TMPDIR') or tempfile.gettempdir())
    working_path = Path(working_path or E.ROOT / '.qemu')
    paths = {'evidence': filesystem(evidence), 'working': filesystem(working_path),
             'scratch': filesystem(scratch_path), 'source_build': filesystem(E.ROOT),
             'compiler_scratch': filesystem(os.environ.get('TMPDIR') or tempfile.gettempdir())}
    needs = {}
    def add(role, amount, inodes):
        row = paths[role]; key = str(row['device_id'])
        target = needs.setdefault(key, {'roles': [], 'required_bytes': 0, 'required_inodes': 0,
                                       'filesystem': row})
        target['roles'].append(role); target['required_bytes'] += amount
        target['required_inodes'] += inodes
    add('evidence', max(minimum, sum(pending.values()) + packaging + RESERVE), required_inodes)
    add('working', temporary, 256 if temporary else 0)
    add('scratch', scratch, 4096 if scratch else 0)
    compiler_peak = 0
    if scratch and paths['compiler_scratch']['device_id'] != paths['scratch']['device_id']:
        compiler_peak = 512 * MIB
        add('compiler_scratch', compiler_peak, 4096)
    # Builds use both the source tree and the working-image path. If these are
    # different devices, each must fit the declared active build footprint.
    if paths['source_build']['device_id'] != paths['working']['device_id']:
        add('source_build', temporary, 256 if temporary else 0)
    failures = []
    for key, row in needs.items():
        fs = row['filesystem']
        if fs['read_only'] or fs['free_bytes'] < row['required_bytes'] or fs['free_inodes'] < row['required_inodes']:
            failures.append(key)
        row['shortage_bytes'] = max(0, row['required_bytes'] - fs['free_bytes'])
    record = {'schema': 2, 'purpose': purpose, 'status': 'INSUFFICIENT' if failures else 'AVAILABLE',
              'build_input_sha256': E.identity(E.manifest()), 'harness_sha256': E.identity(E.manifest(harness=True)),
              'stages': stages, 'pending_durable_bytes': pending, 'durable_growth_bytes': sum(pending.values()),
              'temporary_peak_bytes': temporary, 'scratch_peak_bytes': scratch, 'packaging_bytes': packaging,
              'separate_compiler_scratch_bytes': compiler_peak,
              'emergency_receipt_bytes': RESERVE, 'admission_floor_bytes': minimum,
              'paths': paths, 'filesystems': needs, 'failed_filesystems': failures,
              'policy': 'No capacity reservation; dependency-bound retention; exact native replay required for reuse.'}
    directory = evidence / 'resource-attempts' / stamp()
    directory.mkdir(parents=True, exist_ok=False)
    E.write(directory / 'storage.json', record)
    E.require(not failures, 'BLOCKED_STORAGE: ' + str(directory / 'storage.json'))
    return record


def before_step(evidence, kind):
    """Existing floor plus bounded per-operation output and failure headroom."""
    E.require(kind in ('runtime', 'native', 'build', 'fixtures'), 'unknown expensive stage')
    floor = NATIVE_FLOOR if kind == 'native' else 512 * MIB
    return admit(evidence, stages={kind: {'state': 'MISSING', 'reason': 'new operation'}},
                 durable={kind: 256 * MIB}, temporary=128 * MIB, minimum=floor,
                 purpose='before-' + kind)


def sparse_copy(source, destination):
    """Independent byte-verified candidate copy; never writable hardlinks."""
    source, destination = Path(source), Path(destination)
    E.require(source.is_file() and not source.is_symlink() and not destination.exists(),
              'candidate copy source/destination ownership')
    before = (source.stat().st_size, source.stat().st_mode & 0o7777, E.sha(source))
    subprocess.run(['cp','--sparse=always','--preserve=mode,timestamps','--',str(source),str(destination)],check=True)
    E.require(destination.is_file() and not destination.is_symlink() and
              (source.stat().st_dev,source.stat().st_ino)!=(destination.stat().st_dev,destination.stat().st_ino) and
              (destination.stat().st_size,destination.stat().st_mode & 0o7777,E.sha(destination))==before,
              'candidate sparse copy changed bytes/mode or shared a writable inode')
    return str(destination)
