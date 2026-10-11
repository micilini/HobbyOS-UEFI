"""Explicit, journalled retirement of inactive generated HobbyOS artifacts.

Plans name regular files, never recursive deletion targets. A maintenance lock
excludes new owned launches; lsof checks existing readers/writers independently.
Kept content and replacement packs are verified before any unlink.
"""
import argparse
import contextlib
import fcntl
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import time
import memory_evidence as E

CATEGORIES = {'retired-native-output', 'retired-candidate', 'run-working-copy',
              'duplicate-binary', 'redundant-bundle', 'disposable-materialization',
              'compacted-evidence'}
RESERVE = 64 * 1024 * 1024


def relative(name):
    E.safe(name)
    E.require(name.split('/')[0] in ('backups', '.qemu'), 'outside generated artifact roots')
    return name


@contextlib.contextmanager
def exclusion(root, exclusive=True):
    path = Path(root) / 'backups' / '.artifact-maintenance.lock'
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        E.require(stat.S_ISREG(os.fstat(fd).st_mode), 'maintenance lock type')
        fcntl.flock(fd, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


@contextlib.contextmanager
def parent_fd(root, name):
    """Walk from an owned root descriptor; no component may be a symlink/mount."""
    E.safe(name)
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    device = os.fstat(fd).st_dev
    chain = []
    try:
        for component in Path(name).parts[:-1]:
            child = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd); fd = child
            st = os.fstat(fd)
            E.require(st.st_dev == device, 'unexpected artifact mount')
            chain.append([st.st_dev, st.st_ino])
        yield fd, Path(name).name, chain
    finally:
        os.close(fd)


def fingerprint(root, name):
    with parent_fd(root, name) as (parent, leaf, chain):
        fd = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=parent)
        try:
            st = os.fstat(fd)
            E.require(stat.S_ISREG(st.st_mode), 'artifact is not a regular file')
            h = hashlib.sha256()
            while data := os.read(fd, 1024 * 1024): h.update(data)
            after = os.fstat(fd)
            E.require((st.st_size, st.st_mtime_ns, st.st_ctime_ns) ==
                      (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'artifact changed while hashing')
            return {'path': name, 'size': st.st_size, 'allocated': st.st_blocks * 512,
                    'device': st.st_dev, 'inode': st.st_ino, 'links': st.st_nlink,
                    'mode': stat.S_IMODE(st.st_mode), 'mtime_ns': st.st_mtime_ns,
                    'ctime_ns': st.st_ctime_ns, 'sha256': h.hexdigest(), 'parents': chain}
        finally:
            os.close(fd)


def open_reference_command(paths, mountinfo=None):
    """Exclude unrelated private namespace mounts, never an artifact device.

    lsof cannot stat these Snap mounts as the workspace user. Kernel mount
    identities bound the exclusion; all other diagnostics still stop deletion.
    """
    devices = {(os.major(p.stat().st_dev), os.minor(p.stat().st_dev)) for p in paths}
    command = ['lsof']; excluded = []
    if mountinfo is None: mountinfo = Path('/proc/self/mountinfo').read_text()
    for line in mountinfo.splitlines():
        left, right = line.split(' - ', 1); fields = left.split(); kind = right.split()[0]
        point = Path(fields[4].replace('\\040', ' ').replace('\\134', '\\'))
        if not (point == Path('/run/snapd/ns') and kind == 'tmpfs' or
                point.parent == Path('/run/snapd/ns') and kind == 'nsfs'):
            continue
        device = tuple(int(n) for n in fields[2].split(':'))
        E.require(device not in devices and not any(p.resolve().is_relative_to(point) for p in paths),
                  'open-reference exclusion overlaps artifact filesystem')
        command += ['-e', str(point)]
        excluded.append({'mount':str(point), 'device':fields[2], 'type':kind})
    return command + ['-nP', '-Fpn', '--', *map(str, paths)], excluded


def check_users(paths):
    if not paths: return
    command, excluded = open_reference_command(paths)
    p = subprocess.run(command, capture_output=True, text=True)
    E.require(p.returncode == 1 and not p.stdout and not p.stderr,
              'in-use artifact or unavailable open-reference inspection: ' + (p.stdout + p.stderr)[:2000])
    return {'argv':command, 'excluded_unrelated_mounts':excluded, 'exit_status':p.returncode,
            'stdout':p.stdout, 'stderr':p.stderr}


def kept_manifest(root, entries):
    rows = []
    for row in entries:
        E.require(row['consumers'], 'kept artifact needs named consumers')
        rows.append(fingerprint(root, row['path']) | {'consumers': row['consumers']})
    E.require(len({r['path'] for r in rows}) == len(rows), 'duplicate kept path')
    return {'schema': 1, 'root': str(Path(root).resolve()), 'files': rows}


def verify_kept(root, kept, hashes=True):
    for row in kept['files']:
        if hashes:
            current = fingerprint(root, row['path'])
            E.require(all(current[k] == row[k] for k in ('size', 'mode', 'sha256', 'device', 'inode')),
                      'retained input mismatch: ' + row['path'])
        else:
            with parent_fd(root, row['path']) as (fd, leaf, chain):
                st = os.stat(leaf, dir_fd=fd, follow_symlinks=False)
                E.require(stat.S_ISREG(st.st_mode) and (st.st_dev, st.st_ino, st.st_size,
                          stat.S_IMODE(st.st_mode), st.st_mtime_ns) ==
                          (row['device'], row['inode'], row['size'], row['mode'], row['mtime_ns']),
                          'retained input metadata changed: ' + row['path'])


def plan(root, entries, kept, directories=()):
    root = Path(root).resolve()
    verify_kept(root, kept)
    protected = {r['path'] for r in kept['files']}
    inodes = {(r['device'], r['inode']) for r in kept['files']}
    rows = []
    for entry in entries:
        name = relative(entry['path'])
        E.require(name not in protected, 'retained artifact cannot be retired')
        E.require(entry['category'] in CATEGORIES and entry['reason'] and not entry['consumers'],
                  'unclassified or still-consumed artifact')
        row = fingerprint(root, name)
        E.require(row['links'] == 1 and (row['device'], row['inode']) not in inodes,
                  'unsafe hardlink or protected inode alias')
        E.require(not entry.get('backing_consumers'), 'required disk backing dependency')
        if entry['category'] == 'compacted-evidence':
            replacement = entry.get('replacement')
            E.require(replacement and replacement['decoded_sha256'] == row['sha256'],
                      'missing/corrupt durable replacement')
            verify_pack(root, replacement)
        rows.append(row | {k: entry[k] for k in ('category', 'reason', 'consumers')} |
                    ({'replacement': entry['replacement']} if 'replacement' in entry else {}))
    E.require(len({r['path'] for r in rows}) == len(rows), 'duplicate retirement destination')
    directory_rows = []
    for name in directories:
        relative(name)
        with parent_fd(root, name + '/placeholder') as (fd, leaf, chain):
            directory_rows.append({'path': name, 'parents': chain, 'children': sorted(os.listdir(fd))})
    return {'schema': 1, 'root': str(root), 'kept_sha256': E.identity(kept), 'entries': rows,
            'directories': directory_rows, 'estimated_allocated_release': sum(r['allocated'] for r in rows),
            'logical_bytes': sum(r['size'] for r in rows), 'created_ns': time.time_ns()}


def append_journal(path, row):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o644)
    try:
        data = E.canonical(row)
        E.require(os.write(fd, data) == len(data), 'short retirement journal write')
        os.fsync(fd)
    finally:
        os.close(fd)


def verify_journal(value, history, *, complete=False):
    """An absent file is evidence of retirement only after its exact intent."""
    identity = E.identity(value); entries = {r['path']: r for r in value['entries']}
    states = {}
    for row in history:
        name = row['path']; state = row['state']
        E.require(row['plan_id'] == identity and name in entries, 'journal belongs to another plan/path')
        E.require(states.get(name) not in ('REMOVED', 'ABSENT_AFTER_INTENT'), 'journal repeats completed retirement')
        if state == 'INTENT':
            E.require(row['sha256'] == entries[name]['sha256'], 'journal intent identity differs')
        else:
            E.require(state in ('REMOVED', 'ABSENT_AFTER_INTENT') and states.get(name) == 'INTENT',
                      'journal outcome has no matching intent')
            if state == 'REMOVED':
                E.require(row['allocated_estimate'] == entries[name]['allocated'], 'journal block estimate differs')
        states[name] = state
    completed = {name for name, state in states.items() if state in ('REMOVED', 'ABSENT_AFTER_INTENT')}
    if complete: E.require(completed == set(entries), 'journal terminal retirement missing')
    return completed, set(states)


def apply(root, value, kept, journal, *, users=check_users, batch_size=128, interrupt=None):
    root = Path(root).resolve(); journal = Path(journal)
    E.require(value['root'] == str(root) == kept['root'] and value['kept_sha256'] == E.identity(kept),
              'retention root/kept binding')
    E.require(journal.resolve().is_relative_to(root / 'backups') and not journal.is_symlink(),
              'journal must be kept in the artifact root')
    E.require(journal.relative_to(root).as_posix() not in {r['path'] for r in value['entries']},
              'journal cannot delete itself')
    journal.parent.mkdir(parents=True, exist_ok=True)
    plan_id = E.identity(value); history = []
    if journal.exists():
        history = [json.loads(line) for line in journal.read_text().splitlines()]
    completed, intended = verify_journal(value, history)
    initial = shutil.disk_usage(root).free; removed = []; recovered_intents = []; open_checks = []
    with exclusion(root):
        verify_kept(root, kept)
        protected = {r['path'] for r in kept['files']}
        protected_inodes = {(r['device'], r['inode']) for r in kept['files']}
        E.require(all(r['path'] not in protected and (r['device'], r['inode']) not in protected_inodes
                      and r['links'] == 1 and r['category'] in CATEGORIES and not r['consumers']
                      for r in value['entries']), 'plan attempts to retire retained/unclassified input')
        for directory in value['directories']:
            with parent_fd(root, directory['path'] + '/placeholder') as (fd, leaf, chain):
                removed_children = {Path(n).name for n in completed if str(Path(n).parent) == directory['path']}
                E.require(chain == directory['parents'] and set(os.listdir(fd)) ==
                          set(directory['children']) - removed_children, 'unexpected directory child/replacement')
        for start in range(0, len(value['entries']), batch_size):
            E.require(shutil.disk_usage(root).free >= RESERVE, 'retirement journal reserve exhausted')
            batch = value['entries'][start:start + batch_size]
            inspection = users([root / r['path'] for r in batch if r['path'] not in completed and (root / r['path']).exists()])
            if inspection is not None: open_checks.append(inspection)
            for row in batch:
                name = relative(row['path'])
                if name in completed:
                    E.require(not (root / name).exists(), 'retired path reappeared')
                    continue
                if not (root / name).exists() and name in intended:
                    append_journal(journal, {'plan_id': plan_id, 'path': name, 'state': 'ABSENT_AFTER_INTENT'})
                    recovered_intents.append(name); continue
                actual = fingerprint(root, name)
                E.require(all(actual[k] == row[k] for k in actual), 'artifact changed since plan: ' + name)
                if 'replacement' in row: verify_pack(root, row['replacement'])
                append_journal(journal, {'plan_id': plan_id, 'path': name, 'state': 'INTENT',
                                       'sha256': row['sha256'], 'time_ns': time.time_ns()})
                if interrupt: interrupt('before', row)
                with parent_fd(root, name) as (fd, leaf, chain):
                    st = os.stat(leaf, dir_fd=fd, follow_symlinks=False)
                    E.require(chain == row['parents'] and stat.S_ISREG(st.st_mode) and
                              (st.st_dev, st.st_ino, st.st_size, st.st_nlink, st.st_mtime_ns, st.st_ctime_ns) ==
                              (row['device'], row['inode'], row['size'], 1, row['mtime_ns'], row['ctime_ns']),
                              'artifact changed immediately before unlink')
                    os.unlink(leaf, dir_fd=fd); os.fsync(fd)
                if interrupt: interrupt('after', row)
                append_journal(journal, {'plan_id': plan_id, 'path': name, 'state': 'REMOVED',
                                       'allocated_estimate': row['allocated'], 'time_ns': time.time_ns()})
                removed.append(row)
            verify_kept(root, kept, hashes=False)
        verify_kept(root, kept)
    final = shutil.disk_usage(root).free
    return {'schema': 1, 'status': 'APPLIED', 'plan_id': plan_id, 'removed_files': len(removed),
            'removed_logical_bytes': sum(r['size'] for r in removed),
            'released_allocated_estimate': sum(r['allocated'] for r in removed),
            'observed_available_before': initial, 'observed_available_after': final,
            'observed_free_delta': final - initial, 'recovered_interrupted_intents': recovered_intents,
            'already_completed': len(completed), 'kept_recheck': 'PASS', 'open_reference_checks':open_checks}


def verify_pack(root, row, output=None):
    relative(row['path']); path = Path(root) / row['path']
    E.require(fingerprint(root, row['path'])['sha256'] == row['sha256'] and
              0 <= row['size'] <= 2**34 and row['mode'] in (0o644, 0o755), 'invalid compact reference')
    h = hashlib.sha256(); size = 0
    stream = output.open('xb') if output is not None else None
    try:
        with gzip.open(path, 'rb') as src:
            while data := src.read(1024 * 1024):
                size += len(data); E.require(size <= row['size'], 'compact expansion exceeds bound')
                h.update(data)
                if stream: stream.write(data)
        E.require(size == row['size'] and h.hexdigest() == row['decoded_sha256'], 'compact bytes changed')
        if stream:
            stream.flush(); os.fsync(stream.fileno()); os.fchmod(stream.fileno(), row['mode'])
    finally:
        if stream: stream.close()


def verify_report(base, report_path='report.json'):
    """Replay cleanup accounting; deleted payloads are not claimed available."""
    E.safe(report_path)
    base = Path(base); report = json.loads((base / report_path).read_text())
    E.require(report['schema'] == 1 and report['status'] == 'APPLIED', 'retention report schema/status')
    for row in report['proof_files']:
        E.safe(row['path']); path = base / row['path']
        E.require(path.is_file() and not path.is_symlink() and path.stat().st_size == row['size'] and
                  E.sha(path) == row['sha256'], 'retention report dependency changed')
    totals = dict(removed_files=0, removed_logical_bytes=0, released_allocated_estimate=0,
                  observed_free_delta_sum=0)
    for batch in report['batches']:
        for key in ('plan', 'result', 'journal'): E.safe(batch[key])
        value = json.loads((base / batch['plan']).read_text())
        result = json.loads((base / batch['result']).read_text())
        history = [json.loads(line) for line in (base / batch['journal']).read_text().splitlines()]
        verify_journal(value, history, complete=True)
        E.require(result == batch['outcome'] and result['plan_id'] == E.identity(value) and
                  result['status'] == 'APPLIED' and result['kept_recheck'] == 'PASS' and
                  result['already_completed'] == 0 and result['recovered_interrupted_intents'] == [] and
                  result['removed_files'] == len(value['entries']) and
                  result['removed_logical_bytes'] == sum(r['size'] for r in value['entries']) and
                  result['released_allocated_estimate'] == sum(r['allocated'] for r in value['entries']) and
                  result['observed_free_delta'] == result['observed_available_after'] - result['observed_available_before'],
                  'retention accounting differs from completed journal')
        for key in ('removed_files', 'removed_logical_bytes', 'released_allocated_estimate'): totals[key] += result[key]
        totals['observed_free_delta_sum'] += result['observed_free_delta']
    E.require(all(report[key] == value for key, value in totals.items()), 'retention aggregate differs')
    return dict(status='PASS', **totals, deleted_bytes_replayable=False)


def pack_required(root, name, store, validator):
    """Seal one necessary input; callers retire expanded bytes only through apply."""
    original = fingerprint(root, name); store = Path(store); store.mkdir(parents=True, exist_ok=True)
    E.require(shutil.disk_usage(store).free >= original['size'] + RESERVE,
              'insufficient durable replacement overlap')
    target = store / (original['sha256'] + '.gz')
    E.require(not target.exists(), 'replacement already exists')
    temporary = target.with_suffix('.pending')
    with temporary.open('xb') as raw:
        with gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as encoded, (Path(root) / name).open('rb') as src:
            shutil.copyfileobj(src, encoded, 1024 * 1024)
        raw.flush(); os.fsync(raw.fileno())
    os.rename(temporary, target)
    row = {'path': target.relative_to(root).as_posix(), 'sha256': E.sha(target),
           'decoded_sha256': original['sha256'], 'size': original['size'], 'mode': original['mode']}
    with tempfile.TemporaryDirectory(prefix='verify-compact-', dir=store) as temp:
        copy = Path(temp) / Path(name).name; verify_pack(root, row, copy)
        validator(copy)
    E.write(target.with_suffix('.json'), row)
    return row


def image_info(path):
    return json.loads(subprocess.check_output(['qemu-img', 'info', '--output=json', str(path)]))


def seal_firmware_disk(image, directory, candidate):
    """OVMF may add NvVars; retain it and verify every boot payload before retirement."""
    image_digest = E.sha(image)
    expected = {'kernel.elf': '::/kernel.elf', 'BOOTX64.EFI': '::/EFI/BOOT/BOOTX64.EFI',
                'zap-light16.psf': '::/EFI/fonts/zap-light16.psf',
                'logo.bmp': '::/EFI/images/logo.bmp', 'startup.nsh': '::/startup.nsh'}
    listing = subprocess.check_output(['mdir', '-a', '-b', '-/', '-i', str(image), '::'], text=True).splitlines()
    allowed = set(expected.values()) | {'::/EFI/', '::/EFI/BOOT/', '::/EFI/fonts/', '::/EFI/images/', '::/NvVars'}
    E.require(len(listing) == len(set(listing)) and set(listing) <= allowed and
              set(expected.values()) <= set(listing), 'unclassified guest disk contents')
    output = directory / 'working-disk-state'; output.mkdir(exist_ok=True)
    payload = json.loads((candidate / 'build-receipt.json').read_text())['payload']['files']
    files = []
    with tempfile.TemporaryDirectory(prefix='verify-run-payload-') as temp:
        for name, guest in expected.items():
            target = Path(temp) / name
            subprocess.run(['mcopy', '-i', str(image), guest, str(target)], check=True)
            E.require({'size': target.stat().st_size, 'sha256': E.sha(target)} == payload[name],
                      'working boot payload changed: ' + name)
        if '::/NvVars' in listing:
            staged = Path(temp) / 'NvVars.bin'
            subprocess.run(['mcopy', '-i', str(image), '::/NvVars', str(staged)], check=True)
            target = output / staged.name
            if target.exists():
                E.require(not target.is_symlink() and target.is_file() and
                          target.stat().st_size == staged.stat().st_size and E.sha(target) == E.sha(staged),
                          'staged firmware state changed')
            else:
                with target.open('xb') as dest, staged.open('rb') as src:
                    shutil.copyfileobj(src, dest); dest.flush(); os.fsync(dest.fileno())
            files.append({'path': target.name, 'size': target.stat().st_size, 'sha256': E.sha(target)})
    E.require(E.sha(image) == image_digest, 'working image changed during sealing')
    record = {'schema': 1, 'status': 'SEALED', 'working_image_sha256': image_digest, 'listing': listing,
              'verified_payload': payload, 'firmware_state': files,
              'retirement': 'Disposable disk retired; exact boot payload and added OVMF state remain available.'}
    if (output / 'state.json').exists():
        E.require(json.loads((output / 'state.json').read_text()) == record, 'sealed firmware receipt changed')
    else:
        E.write(output / 'state.json', record)
    return output


def retire_run(directory):
    """Drop only sealed memory-qemu working bytes; keep every accepted/failed raw run."""
    directory = Path(directory).resolve()
    E.require(directory.is_relative_to(E.ROOT / 'backups'), 'run outside artifacts')
    result = json.loads((directory / 'result.json').read_text())
    E.require(result['cleanup'] is True and result['status'] in ('COLLECTED', 'FAIL'),
              'run writer/collector has not stopped')
    session = Path(result['runtime_session'])
    E.require(session.resolve() == session and session.is_relative_to(E.ROOT / 'backups'),
              'runtime session alias/outside artifacts')
    descriptor = json.loads((session / 'session.json').read_text())
    E.require(descriptor['kind'] == 'isolated-runtime' and Path(descriptor['parent_evidence']) == directory,
              'working copy owner differs')
    destination = directory / 'artifact-retirement'
    if (destination / 'result.json').exists(): return json.loads((destination / 'result.json').read_text())
    candidate = Path(result['canonical_candidate'])
    E.require(candidate.resolve() == candidate and candidate.is_relative_to(E.ROOT / 'backups'),
              'canonical run candidate outside artifacts')
    frozen = json.loads((candidate / 'candidate.json').read_text())
    E.require(E.identity(frozen) == result['candidate_id'], 'canonical run candidate identity differs')
    for name, fact in frozen['artifacts'].items():
        E.require((candidate / name).stat().st_size == fact['size'] and E.sha(candidate / name) == fact['sha256'],
                  'canonical run binary missing or changed')
    runtime = session / 'r'; selected = []; kept_names = {'result.json', 'candidate.json', 'launch.json'}
    for name in ('qemu-serial.log', 'qemu-debugcon.log', 'qemu-trace.log', 'launch.env'):
        original, canonical = runtime / name, directory / name
        if original.exists():
            E.require(canonical.is_file() and E.sha(original) == E.sha(canonical), 'unsealed runtime stream: ' + name)
            kept_names.add(name); selected.append(original)
    image = runtime / 'working.img'
    firmware = None
    if image.exists():
        info = image_info(image)
        E.require(info['format'] == 'raw' and not info.get('backing-filename'), 'working image has unsupported backing state')
        if E.sha(image) != result['candidate_image']:
            firmware = seal_firmware_disk(image, directory, candidate)
        selected.append(image)
    observations = runtime / 'serial-receipts.bin'
    if observations.exists():
        import base64
        import struct
        digest = hashlib.sha256(); total = 0
        with (directory / 'serial-observations.jsonl').open() as src:
            for line in src:
                row = json.loads(line); data = base64.b64decode(row['bytes_base64'], validate=True)
                E.require(row['offset'] == total and row['length'] == len(data) and 0 < len(data) <= 64,
                          'timestamp stream cannot reconstruct original')
                digest.update(struct.pack('<QI', row['monotonic_ns'], len(data))); digest.update(data); total += len(data)
        E.require(digest.hexdigest() == E.sha(observations), 'timestamp stream reconstruction differs')
        kept_names.add('serial-observations.jsonl'); selected.append(observations)
    entries = [{'path': p.relative_to(E.ROOT).as_posix(), 'category': 'run-working-copy', 'consumers': [],
                'reason': 'Stopped memory-qemu run; canonical raw streams and immutable candidate identity verified.'}
               for p in selected]
    destination.mkdir(parents=True, exist_ok=True)
    kept = kept_manifest(E.ROOT, [{'path': (directory / n).relative_to(E.ROOT).as_posix(),
                                 'consumers': ['same native runtime predicates and original timestamp replay']}
                                for n in sorted(kept_names)] +
                                [{'path': (candidate / n).relative_to(E.ROOT).as_posix(),
                                  'consumers': ['canonical independent per-run image source']}
                                 for n in sorted(frozen['artifacts'])] +
                                ([{'path': p.relative_to(E.ROOT).as_posix(), 'consumers': ['sealed per-boot OVMF state']}
                                  for p in firmware.iterdir() if p.is_file()] if firmware else []))
    value = plan(E.ROOT, entries, kept)
    E.write(destination / 'kept.json', kept); E.write(destination / 'plan.json', value)
    result = apply(E.ROOT, value, kept, destination / 'journal.jsonl')
    E.write(destination / 'result.json', result)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=('plan', 'apply'))
    p.add_argument('--selection', type=Path); p.add_argument('--kept', type=Path, required=True)
    p.add_argument('--plan', type=Path, required=True); p.add_argument('--journal', type=Path)
    p.add_argument('--result', type=Path); a = p.parse_args()
    kept = json.loads(a.kept.read_text())
    if a.mode == 'plan': E.write(a.plan, plan(E.ROOT, json.loads(a.selection.read_text()), kept))
    else:
        E.require(a.journal and a.result, 'apply needs journal/result')
        result = apply(E.ROOT, json.loads(a.plan.read_text()), kept, a.journal)
        E.write(a.result, result); print(json.dumps(result, sort_keys=True))


if __name__ == '__main__': main()
