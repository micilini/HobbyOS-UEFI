#!/usr/bin/env python3
"""Replay rejected copies of a real capture without changing accepted evidence."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import memory_evidence as E

SPEC = importlib.util.spec_from_file_location('memory_verify', Path(__file__).with_name('verify-memory-observation.py'))
V = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(V)


def replay_retained_recipes(path):
    """Replay original fixtures with their frozen historical verifier.

    These outcomes preserve the earlier tested boundaries. They are separate
    from the strengthened current verifier and never supply a boot repetition.
    """
    value = json.loads(path.read_text()); root = path.parent
    E.require(value['schema'] == 1 and value['kind'] == 'retained-synthetic-capture-deltas', 'retained recipe schema')
    for name in ('base', 'verifier'): E.safe(value[name])
    for row in value['verifier_sources']:
        E.safe(row['path']); source = root / row['path']
        E.require(source.stat().st_size == row['size'] and E.sha(source) == row['sha256'], 'retained verifier source differs')
    spec = importlib.util.spec_from_file_location('retained_memory_verifier', root / value['verifier'])
    validator = importlib.util.module_from_spec(spec); spec.loader.exec_module(validator)
    base = root / value['base']; rows = value['base_files']
    E.require(len(rows) == len({r['path'] for r in rows}) and
              {r['path'] for r in rows} == {p.relative_to(base).as_posix() for p in base.rglob('*') if p.is_file()}, 'retained base inventory')
    for row in rows:
        E.safe(row['path']); source = base / row['path']
        E.require(source.stat().st_size == row['size'] and E.sha(source) == row['sha256'], 'retained base bytes differ')
    validator.verify_run(base / 'qemu-serial.log')
    E.require(len(value['cases']) == 23 and len({c['name'] for c in value['cases']}) == 23, 'retained mutation coverage')
    original_result = root / 'original-result.json'
    E.require(E.sha(original_result) == value['original_result_sha256'], 'retained result identity differs')
    original_cases = {c['name']: c for c in json.loads(original_result.read_text())['cases']}
    for case in value['cases']:
        E.safe(case['name']); E.safe(case['file']); E.safe(case['receipt'])
        receipt = root / case['receipt']
        E.require(E.sha(receipt) == case['receipt_sha256'] and json.loads(receipt.read_text()) == original_cases[case['name']], 'retained case receipt differs')
        with tempfile.TemporaryDirectory(prefix='memory-retained-synthetic-') as temp:
            destination = Path(temp) / case['name']; shutil.copytree(base, destination)
            target = destination / case['file']; original = target.read_bytes()
            E.require(hashlib.sha256(original).hexdigest() == case['base_sha256'], 'retained mutation base differs')
            delta = case['delta']; offset = delta['offset']; remove = delta['remove']
            E.require(0 <= offset <= len(original) and 0 <= remove <= len(original) - offset, 'retained delta bounds')
            changed = original[:offset] + bytes.fromhex(delta['insert_hex']) + original[offset + remove:]
            E.require(len(changed) == case['size'] and hashlib.sha256(changed).hexdigest() == case['materialized_sha256'], 'retained materialized bytes differ')
            target.write_bytes(changed)
            shutil.copyfile(receipt, destination / 'expected-rejection.json')
            materialized = [{'path': p.relative_to(destination).as_posix(), 'size': p.stat().st_size, 'sha256': E.sha(p)}
                            for p in sorted(destination.rglob('*')) if p.is_file()]
            E.require(E.identity(materialized) == case['materialized_manifest_sha256'], 'retained complete fixture manifest differs')
            try: validator.verify_run(destination / 'qemu-serial.log')
            except (ValueError, KeyError, OSError, TypeError, IndexError) as error:
                reason = str(error).replace(str(destination), '<synthetic-run>')
                E.require(reason == case['reason'] and case['expected_exit'] == case['actual_exit'] == 1,
                          'retained fixture rejection boundary differs: ' + case['name'] + ': ' + reason)
            else: raise ValueError('retained negative accepted: ' + case['name'])
    return {'status': 'PASS', 'positive': 1, 'negative': 23, 'scope': 'retained earlier verifier boundaries; synthetic fixtures only'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path)
    p.add_argument('--output', type=Path)
    p.add_argument('--replay', type=Path, help='Replay immutable recipes in temporary directories; never modifies the base.')
    p.add_argument('--retained-recipes', type=Path, help='Replay explicitly historical fixtures with their immutable verifier source.')
    a = p.parse_args()
    if a.retained_recipes:
        E.require(not any((a.source, a.output, a.replay)), 'retained recipe replay is a separate contract')
        print(json.dumps(replay_retained_recipes(a.retained_recipes))); return
    E.require(a.source is not None, 'current capture source required')
    V.verify_run(a.source / 'qemu-serial.log')
    if bool(a.output) == bool(a.replay): raise ValueError('choose output or replay')
    if a.output: a.output.mkdir(parents=True, exist_ok=False)
    cases = []
    def replace_serial(data, old, new):
        assert old in data
        return data.replace(old, new, 1)
    def json_change(data, key, value):
        obj = json.loads(data); obj[key] = value
        return (json.dumps(obj, indent=2) + '\n').encode()
    owned = os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks')
    pat_before, pat_after = (b'pat_index=5', b'pat_index=4') if owned else (b'pat_index=4', b'pat_index=5')
    changes = [
        ('missing-begin', 'qemu-serial.log', lambda b: replace_serial(b, b'[HARNESS][BEGIN] seq=1', b'[HARNESS][BEGIX] seq=1')),
        ('missing-end', 'qemu-serial.log', lambda b: replace_serial(b, b'[HARNESS][END] seq=1 ', b'[HARNESS][ENX] seq=1 ')),
        ('wrong-checksum', 'qemu-serial.log', lambda b: re.sub(rb'(ACCEPT seq=1 crc=)([0-9a-f])',
            lambda m: m[1] + (b'1' if m[2] == b'0' else b'0'), b, count=1)),
        ('truncated-line', 'qemu-serial.log', lambda b: b.rstrip(b'\r\n')),
        ('missing-cpu', 'qemu-serial.log', lambda b: re.sub(rb'\[CPU_CONTROL\]\[CPU\]([^\n]*slot=1[^\n]*)', rb'[CPU_CONTROL][CPX]\1', b, count=1)),
        ('duplicate-slot', 'qemu-serial.log', lambda b: re.sub(rb'(\[CPU_CONTROL\]\[CPU\][^\n]*slot=)1', rb'\g<1>0', b, count=1)),
        ('wrong-apic', 'qemu-serial.log', lambda b: re.sub(rb'(\[CPU_CONTROL\]\[CPU\][^\n]*apic=)1', rb'\g<1>7', b, count=1)),
        ('stale-generation', 'qemu-serial.log', lambda b: re.sub(rb'(\[CPU_CONTROL\]\[CPU\][^\n]*generation=)([0-9]+)',
            lambda m: m[1] + str(int(m[2])-1).encode(), b, count=1)),
        ('false-uniform', 'qemu-serial.log', lambda b: re.sub(
            rb'(uniform_cr0=)([01])',
            lambda m: m[1] + (b'1' if m[2] == b'0' else b'0'), b, count=1)),
        ('wrong-translation', 'qemu-serial.log', lambda b: re.sub(rb'(\[PAGE_TABLE\]\[RESULT\][^\n]*physical=0x[0-9a-f]{15})0', rb'\g<1>1', b, count=1)),
        ('bad-hierarchical-permissions', 'qemu-serial.log', lambda b: replace_serial(b, b'effective_us=0', b'effective_us=1')),
        ('wrong-pat-index', 'qemu-serial.log', lambda b: replace_serial(b, pat_before, pat_after)),
        ('invalid-stride', 'qemu-serial.log', lambda b: replace_serial(b, b'stride=48', b'stride=00')),
        ('descriptor-overflow', 'qemu-serial.log', lambda b: re.sub(rb'(\[MEMORY_MAP\]\[DESCRIPTOR\][^\n]*end=0x)[0-9a-f]{16}', rb'\g<1>ffffffffffffffff', b, count=1)),
        ('fabricated-mtrr', 'qemu-serial.log', lambda b: re.sub(rb'(\[MTRR\]\[RANGE\][^\n]*end=0x[0-9a-f]{15})0', rb'\g<1>1', b, count=1)),
        ('missing-timing-marker', 'qemu-serial.log', lambda b: replace_serial(b, b'[CORE] Init Paging...', b'[CORE] Init Paginx...')),
        ('kvm-label-on-tcg', 'launch.json', lambda b: b.replace(b'"kvm"', b'"tcg"', 1)),
        ('wrong-vcpu-count', 'launch.json', lambda b: b.replace(b'4,sockets=1,cores=4,threads=1', b'2,sockets=1,cores=2,threads=1', 1)),
        ('wrong-image-hash', 'result.json', lambda b: json_change(b, 'candidate_image', '0' * 64)),
        ('wrong-elf-hash', 'result.json', lambda b: json_change(b, 'candidate_elf', '0' * 64)),
        ('duplicate-command', 'framed.jsonl', lambda b: b + b.splitlines(keepends=True)[0]),
        ('incomplete-collection', 'result.json', lambda b: json_change(b, 'status', 'FAIL')),
    ]
    def nonmonotonic(data):
        rows = data.splitlines(keepends=True); event = json.loads(rows[2]); event['monotonic_ns'] = 0
        rows[2] = (json.dumps(event) + '\n').encode(); return b''.join(rows)
    changes.append(('nonmonotonic-observation', 'serial-observations.jsonl', nonmonotonic))
    changes.extend([
        ('forged-source-digest', 'candidate.json', lambda b: json_change(b, 'source_sha256', '0' * 64)),
        ('missing-readback', 'page-readback.stdout.log', lambda b: None),
        ('truncated-readback', 'page-readback.stdout.log', lambda b: b.split(b'\n', 2)[0] + b'\n'),
        ('reordered-readback', 'page-readback.stdout.log', lambda b: b'\n'.join(reversed(b.split(b'\n')))),
        ('wrong-readback-value', 'page-readback.stdout.log', lambda b: re.sub(rb'(?m)^(0x[0-9a-f]+:\s*)0x([0-9a-f]+)',
             lambda m: m[1] + f'0x{int(m[2], 16) ^ 0x1000:016x}'.encode(), b, count=1)),
    ])
    expectations = {'forged-source-digest': 'source manifest digest mismatch',
                    'missing-readback': 'page-readback.stdout.log', 'truncated-readback': 'readback missing',
                    'reordered-readback': 'readback address/order/value mismatch',
                    'wrong-readback-value': 'readback address/order/value mismatch'}
    replay = json.loads(a.replay.read_text()) if a.replay else None
    expected_cases = {c['name']: c for c in replay['cases']} if replay else {}
    for name, filename, mutate in changes:
        with tempfile.TemporaryDirectory(prefix='memory-synthetic-') as tmp:
            dest = Path(tmp) / name
            shutil.copytree(a.source, dest, ignore=shutil.ignore_patterns('qemu-trace.log', 'input-layout'))
            path = dest / filename
            old = path.read_bytes(); new = mutate(old)
            if old == new: raise ValueError('mutation did not change ' + name)
            if new is None: path.unlink()
            else: path.write_bytes(new)
            error = None
            try: V.verify_run(dest / 'qemu-serial.log')
            except (ValueError, KeyError, OSError, TypeError, IndexError) as e: error = str(e)
            if not error: raise ValueError('verifier accepted ' + name)
            boundary = expectations.get(name)
            if boundary and boundary not in error: raise ValueError('wrong rejecting boundary for ' + name + ': ' + error)
            # Temporary directory names are not part of the portable rejection identity.
            error = error.replace(str(dest), '<synthetic-run>')
            case = {'name': name, 'file': filename, 'expected_exit': 1, 'actual_exit': 1,
                    'kind': 'synthetic_artifact_fixture', 'recipe': name, 'reason': error,
                    'base_sha256': hashlib.sha256(old).hexdigest(),
                    'materialized_sha256': None if new is None else hashlib.sha256(new).hexdigest()}
            if replay and case != expected_cases.get(name): raise ValueError('mutation replay differs: ' + name)
            cases.append(case)
    result = {'schema': 2, 'status': 'PASS', 'positive': 1, 'negative': len(cases), 'cases': cases,
              'base_candidate_sha256': V.digest(a.source / 'candidate.json'),
              'recipe_source_sha256': V.digest(Path(__file__))}
    if replay and result != replay: raise ValueError('recipe/base identity changed')
    if a.output: (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'status': 'PASS', 'positive': 1, 'negative': len(cases), 'storage': 'immutable_base_and_deterministic_recipes'}))


if __name__ == '__main__': main()
