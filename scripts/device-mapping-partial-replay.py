#!/usr/bin/env python3
"""Replay a blocked development handoff without claiming mapping acceptance."""
import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import memory_evidence as E
import review_transport as R

ROOT = Path(__file__).resolve().parent.parent
BASE = 'ff6eaf609611b3d4a401e739f1f45dd7451346f3'
TREE = 'df9eba7d8c01ab2514474c3fd6e9f91eb3284906'
sys.dont_write_bytecode = True


def module(name):
    spec = importlib.util.spec_from_file_location('device_partial_' + name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = value
    spec.loader.exec_module(value)
    return value


def read(path):
    return json.loads(path.read_text())


def baseline(package):
    saved = read(package / 'preservation/source-manifest.json')
    E.require(saved['source_clean'] and saved['head'] == BASE and
              saved['head_tree'] == saved['export_tree'] == TREE, 'approved baseline identity')
    root = package / 'baseline-source'
    for row in saved['files']:
        E.safe(row['path'])
        path = root / row['path']
        data = path.read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        E.require(len(data) == row['size'] and E.sha(path) == row['sha256'] and
                  blob == row['current_blob'] and path.stat().st_mode & 0o777 == int(row['mode'][3:], 8),
                  'baseline reconstructed bytes/mode/blob')
    E.require(module('package-memory-review').source_tree(saved['files']) == TREE and
              {r['path'] for r in saved['files']} == {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()},
              'baseline reconstructed tree/member set')
    current = read(package / 'source-manifest.json')
    E.require(current['head'] == BASE and not current['source_clean'] and not current['git_worktree_clean'],
              'blocked work falsely committed/clean')
    for row in read(package / 'scope-report.json')['protected_files']:
        E.require(E.sha(ROOT / row['path']) == E.sha(root / row['path']) == row['sha256'],
                  'protected source changed: ' + row['path'])


def candidate(directory, inputs):
    value, receipt = read(directory / 'candidate.json'), read(directory / 'build-receipt.json')
    identity = E.verify_candidate(value, receipt, inputs)
    for row in receipt['commands']:
        for field in ('receipt', 'output'):
            E.safe(row[field])
            E.require(E.sha(directory / row[field]) == row[field + '_sha256'], 'candidate raw build binding')
        E.require(read(directory / row['receipt'])['exit_status'] == 0, 'candidate nonzero build')
    return value, identity


def hub_fields(launch, result, serial, topology, framed):
    E.require(result['profile'] == 'q35-tcg-smp4-hub-keyboard' and result['device_profile'] == 'hub-keyboard',
              'hub named profile')
    argv = launch['argv']
    devices = [argv[i + 1] for i, value in enumerate(argv) if value == '-device']
    E.require(devices == ['nec-usb-xhci,id=xhci,msi=on,msix=off',
                         'usb-hub,id=testhub,bus=xhci.0,port=1',
                         'usb-kbd,bus=xhci.0,port=1.1'], 'hub exclusive keyboard topology')
    E.require('Port 1, Speed 12 Mb/s, Product QEMU USB Hub, ID: testhub' in topology and
              'Port 1.1, Speed 12 Mb/s, Product QEMU USB Keyboard' in topology,
              'hub independent monitor topology')
    for marker in ('[USB_HUB] TT needed: hub_slot=1 tt_port=1',
                   '[HP] Port Enabled! Configuring device...', '[XHCI] Disabling slot 1...',
                   '[XHCI] CMD FAILED. CC=5'):
        E.require(marker in serial, 'hub raw enumeration failure: ' + marker)
    E.require(serial.index('[USB_HUB] TT needed: hub_slot=1 tt_port=1') <
              serial.index('[XHCI] Disabling slot 1...'), 'hub failure chronology')
    E.require(result['status'] == 'FAIL' and result['error'] == 'command-1: exit 1', 'hub failure hidden')
    E.require(len(framed) == 1 and framed[0]['classification'] == 'GUEST_INPUT_MISMATCH' and
              framed[0]['payload'] == 'mem' and framed[0]['handler_status'] is None and
              framed[0]['begin_count'] == framed[0]['end_count'] == 0 and
              framed[0]['guest_input_length'] == 0, 'hub input failure boundary')
    E.require(framed[0]['input_key_release_timeout_seconds'] == 5, 'hub deadline changed')
    return {'classification': 'GUEST_INPUT_MISMATCH', 'handler_executed': False,
            'topology': 'xHCI -> hub port 1 -> sole keyboard port 1.1',
            'cause_boundary': 'Concurrent hub/root reconfiguration observed; protocol repair not attempted'}


def run_proof(directory, inputs, role):
    value, identity = candidate(directory, inputs)
    if role == 'hub-approved':
        E.require(identity == 'b167cfe76eb1c35b6921eb7f5ac23cff3957d53e8742145f65b27f09cef26509',
                  'hub control is not the approved predecessor candidate')
    launch, result, boot = [read(directory / name) for name in ('launch.json', 'result.json', 'boot-receipt.json')]
    E.require(launch['source_sha256'] == value['source_sha256'] and
              launch['build_receipt_sha256'] == value['build_receipt_sha256'], 'run source/build identity')
    for key in ('candidate_id', 'launch_id', 'vm_id', 'boot_id'):
        E.require(launch[key] == result[key] == boot[key] and launch[key], 'run independent identity: ' + key)
    E.require(launch['candidate_id'] == identity and result['cleanup'] and
              result['started_ns'] < launch['monotonic_ns'] <= boot['observed_ns'] < result['finished_ns'],
              'run identity/chronology/cleanup')
    E.require(E.sha(directory / 'vm-identity.jsonl') == boot['qmp_sha256'], 'VM receipt hash')
    qmp = [json.loads(line) for line in (directory / 'vm-identity.jsonl').read_text().splitlines()]
    sent = [r['message'] for r in qmp if r['direction'] == 'send' and r['message'].get('execute') == 'query-uuid']
    E.require(len(sent) == 1, 'VM identity query count')
    answer = [r['message'] for r in qmp if r['direction'] == 'receive' and r['message'].get('id') == sent[0]['id']]
    E.require(len(answer) == 1 and answer[0]['return']['UUID'] == launch['vm_id'], 'VM raw identity mismatch')
    argv = launch['argv']
    def arg(key):
        E.require(argv.count(key) == 1, 'launch argument count: ' + key)
        return argv[argv.index(key) + 1]
    accel, smp = arg('-accel').split(',')[0], int(arg('-smp').split(',')[0])
    E.require(arg('-machine') == 'q35' and accel in ('tcg', 'kvm') and smp in (1, 4) and
              arg('-cpu') == ('max' if accel == 'tcg' else 'host') and arg('-m') == '2G' and
              arg('-uuid') == launch['vm_id'], 'partial launch profile')
    for kind, name in [('elf', 'kernel.elf'), ('image', 'hobbyos.img')]:
        E.require(launch[kind]['sha256'] == result['candidate_' + kind] == value['artifacts'][name]['sha256'],
                  'run binary identity')
    raw = (directory / 'qemu-serial.log').read_bytes()
    lines, ends, offset = [], [], 0
    for line in raw.splitlines(keepends=True):
        offset += len(line); ends.append(offset); lines.append(line.rstrip(b'\r\n').decode('ascii'))
    topology = {}
    for line in lines:
        match = re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)', line)
        if match:
            slot = int(match[1]); E.require(slot not in topology, 'duplicate online slot')
            topology[slot] = (int(match[2]), match[3])
    E.require(set(topology) == set(range(smp)) and len({v[0] for v in topology.values()}) == smp, 'partial online set')
    policy = module('verify-cpu-control-state')
    ready = policy.readiness(lines, topology)
    modes = [line.removeprefix('[LAPIC] System using ') for line in lines
             if line in ('[LAPIC] System using XAPIC', '[LAPIC] System using X2APIC')]
    E.require(len(modes) == 1, 'actual APIC mode record missing/duplicate')
    if role == 'early-context-failure':
        commands = [json.loads(l) for l in (directory / 'commands.jsonl').read_text().splitlines()]
        E.require(result['status'] == 'FAIL' and result['error'] == 'command-1: exit 1' and
                  any(r['name'] == 'command-1' and r['exit_status'] == 1 for r in commands) and
                  not (directory / 'framed.jsonl').exists() and
                  (directory / 'command-1.stdout.log').read_bytes() == b'' and
                  'foundation-qemu.py: timed out' in (directory / 'command-1.stderr.log').read_text() and
                  'PCI: MCFG Table found. Parsing entries...' in lines and
                  not any('[XHCI]' in line for line in lines), 'initial unframed timeout evidence')
        return {'profile': result['profile'], 'candidate_id': identity, 'vm_id': launch['vm_id'],
                'boot_id': launch['boot_id'], 'role': role, 'status': 'RETAINED_FAILED_ATTEMPT',
                'boundary': 'No framed record was produced; missing command completion remains failure'}
    framed = [json.loads(line) for line in (directory / 'framed.jsonl').read_text().splitlines()]
    E.require([r['sequence'] for r in framed] == list(range(1, len(framed) + 1)) and
              all(r['vm_id'] == launch['vm_id'] and r['candidate_sha256'] == result['candidate_elf'] for r in framed),
              'framed identity/sequence')
    result_proof = {'profile': result['profile'], 'candidate_id': identity, 'vm_id': launch['vm_id'],
                    'boot_id': launch['boot_id'], 'role': role, 'online_cpus': smp,
                    'actual_apic_mode': modes[0], 'controls': [policy.policy_cpu(row) for row in ready]}
    if role in ('hub-current', 'hub-approved'):
        commands = [json.loads(l) for l in (directory / 'commands.jsonl').read_text().splitlines()]
        E.require(any(r['name'] == 'command-1' and r['exit_status'] == 1 for r in commands), 'hub nonzero command missing')
        result_proof['hub'] = hub_fields(launch, result, raw.decode('ascii'),
                                         (directory / 'usb-topology.stdout.log').read_text(), framed)
    else:
        E.require(result['status'] == 'COLLECTED', 'partial successful collection missing')
        observation = module('verify-memory-observation')
        foundation = observation.FOUNDATION
        proof = []
        tlb = module('verify-tlb-shootdown')
        blocks = tlb.trace_blocks(directory) if any(r['payload'] == 'tlbtest' for r in framed) else []
        for row in framed:
            profile = {'name': result['profile'], 'smp': str(smp), 'vm_id': launch['vm_id'], 'input_profile': 'framed',
                       'command_input_observation': 'qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb'}
            errors = foundation.validate_transaction(row, lines, ends, profile, result['candidate_elf'], directory)
            E.require(not errors and row['handler_status'] == 0, 'partial command framing: ' + '; '.join(errors))
            segment = foundation.transaction_segment(lines, row)
            item = {'command': row['payload'], 'handler_status': 0}
            if row['payload'] in ('mem', 'cpu', 'irq check'):
                E.require(not foundation.evidence_errors(row, segment, smp), 'partial ordinary command invariant')
            elif row['payload'] == 'cpustate':
                item['policy'] = policy.policy_observation(segment, topology)
            elif row['payload'] == 'tlbtest':
                E.require(len(blocks) == 1, 'partial TLB trace count')
                segment = tlb.bind_trace(segment, blocks[0])
                try:
                    item['tlb'] = tlb.verify_tlb(segment, topology, policy.number(ready[0], 'cr3'),
                        '[LAPIC] System using X2APIC' in lines)
                except ValueError as error:
                    E.require(str(error) == 'TLB missing/stale global request generation',
                              'unexpected partial TLB replay failure: ' + str(error))
                    item['tlb'] = {'status': 'REJECTED', 'reason': str(error),
                        'required_work': 'Account independently for mapping-maintenance requests before the first fixture generation',
                        'acceptance': 'NOT_ESTABLISHED'}
            proof.append(item)
        result_proof.update(status='PARTIAL_COMMAND_PROOF', commands=proof,
                            boundary='No complete mapping/observation matrix or independent table readback claimed')
    return result_proof


def semantic(package, host_tests):
    os.chdir(ROOT)
    baseline(package)
    selection = read(package / 'partial-selection.json')
    E.require(selection['current_acceptance'] == 'BLOCKED' and
              selection['blocker'] == 'PREEXISTING_HUB_ENUMERATION_FAILURE', 'blocked delivery status')
    result = []
    identities = set()
    for row in selection['runs']:
        E.safe(row['path']); E.safe(row['inputs'])
        proof = run_proof(package / row['path'], package / row['inputs'], row['role'])
        key = (proof['vm_id'], proof['boot_id'])
        E.require(key not in identities, 'duplicate partial boot'); identities.add(key); result.append(proof)
    E.require(len(result) == 6 and {r['role'] for r in result} ==
              {'hub-current', 'hub-approved', 'early-context-failure', 'integration'}, 'partial boot inventory')
    for row in selection['builds']:
        value, _ = candidate(package / row['path'], ROOT)
        E.require(value['source_sha256'] == E.identity(E.manifest()), 'current build inputs changed')
    E.require({r['profile'] for r in selection['builds']} == {'debug', 'production', 'selftest'}, 'partial build profile inventory')
    order = module('device-mapping-order').fixtures((ROOT / 'kernel/src/core/kernel_init.c').read_text())
    negatives = []
    hub = next(r for r in selection['runs'] if r['role'] == 'hub-current')
    directory = package / hub['path']
    original = (read(directory / 'launch.json'), read(directory / 'result.json'),
                (directory / 'qemu-serial.log').read_text(), (directory / 'usb-topology.stdout.log').read_text(),
                [json.loads(l) for l in (directory / 'framed.jsonl').read_text().splitlines()])
    for name in ('direct-keyboard', 'missing-monitor-child', 'hidden-failure', 'fake-handler', 'extended-deadline'):
        args = copy.deepcopy(original)
        if name == 'direct-keyboard': args[0]['argv'][args[0]['argv'].index('usb-kbd,bus=xhci.0,port=1.1')] = 'usb-kbd,bus=xhci.0,port=2'
        elif name == 'missing-monitor-child': args = (*args[:3], args[3].replace('Port 1.1,', 'Port 2,'), args[4])
        elif name == 'hidden-failure': args[1]['status'] = 'COLLECTED'
        elif name == 'fake-handler': args[4][0]['handler_status'] = 0
        else: args[4][0]['input_key_release_timeout_seconds'] = 10
        try: hub_fields(*args)
        except ValueError as error: negatives.append({'synthetic': True, 'name': name, 'reason': str(error), 'status': 'REJECTED'})
        else: raise ValueError('hub semantic mutation accepted: ' + name)
    with tempfile.TemporaryDirectory(prefix='device-partial-tests-') as temporary:
        scratch = Path(temporary)
        transport = module('review-transport-fixtures').run(scratch / 'transport')
        if host_tests:
            with contextlib.redirect_stdout(io.StringIO()):
                host = module('device-mapping-host').run(scratch / 'host', inherited=True)
        else: host = {'status': 'NOT_EXECUTED'}
    return {'current_acceptance': 'BLOCKED', 'full_portable_acceptance': 'NOT_AVAILABLE',
            'partial_evidence_replay': 'PASS', 'independent_review': 'PENDING',
            'hardware': 'DEFERRED_TO_FINAL_CLOSURE', 'runs': result, 'host': host,
            'worker_order_fixtures': order, 'hub_fixtures': negatives,
            'transport_negative_cases': len(transport['cases']), 'binary_limitation': R.LIMITATION}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--materialized', action='store_true')
    parser.add_argument('--host-tests', action='store_true')
    args = parser.parse_args(); package = args.package.resolve()
    if args.materialized:
        print(json.dumps(semantic(package, args.host_tests), sort_keys=True)); return
    pkg = module('package-memory-review'); pkg.validate_tree(package)
    R.selection_audit(R.index(package))
    with tempfile.TemporaryDirectory(prefix='device-partial-reconstruction-') as temporary:
        restored = Path(temporary) / 'restored'; R.materialize(package, restored)
        argv = [sys.executable, '-B', str(restored / 'source/scripts/device-mapping-partial-replay.py'),
                '--package', str(restored), '--materialized']
        if args.host_tests: argv.append('--host-tests')
        process = subprocess.run(argv, text=True, capture_output=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        E.require(process.returncode == 0, 'partial semantic replay failed:\n' + process.stdout + process.stderr)
        result = json.loads(process.stdout.splitlines()[-1])
    pkg.validate_tree(package)
    result['exact_reconstruction'] = 'PASS'
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
