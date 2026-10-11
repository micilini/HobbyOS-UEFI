#!/usr/bin/env python3
"""Collect and replay the fixed current inherited-regression contract."""
import argparse
import copy
import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import memory_evidence as E
import validation_candidates as candidates
import memory_current

ROOT = Path(__file__).resolve().parent.parent
REQUIREMENTS = ROOT / 'scripts/memory-current-requirements.json'


def load_module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def read(path):
    return json.loads(Path(path).read_text())


def family_result(attempt, transport=None):
    """Retain a failed aggregate while binding a fresh replay of completed work."""
    original = read(attempt / 'result.json')
    path = attempt / 'replayed-result.json'
    if not path.exists(): return original
    row = read(path); source = E.identity(E.manifest())
    E.require(row.get('schema') == 1 and row.get('status') == 'PASS' and
              original['status'] == 'FAIL' and original['key'] == 'interrupts' and
              row['original_sha256'] == E.sha(attempt / 'result.json') and
              row['started_sha256'] == E.sha(attempt / 'started.json') and
              original['build_input_sha256'] == row['build_input_sha256'] == source,
              'completed native replay original/source binding')
    E.require(original['commands'] and set(row['commands']) == set(original['commands']),
              'completed native replay command inventory')
    for name in original['commands']:
        E.safe(name); command = read(attempt / name)
        E.require(E.sha(attempt / name) == row['commands'][name] and command['exit_status'] == 0 and
                  command['build_input_sha256'] == source and
                  E.sha((attempt / name).with_suffix('.stdout.log')) == command['stdout_sha256'],
                  'completed native replay failed or changed collection')
    E.safe(row['verification']); proof = attempt / row['verification']
    command = read(proof / 'command.json'); output = proof / 'stdout.log'
    artifact = command['environment']['HOBBYOS_TEST_ARTIFACT']
    actual = transport.mapped(str(Path(artifact) / 'evidence.json')).parent if transport is not None else Path(artifact)
    E.require(E.sha(proof / 'command.json') == row['verification_sha256'] and
              command['exit_status'] == 0 and command['build_input_sha256'] == source and
              command['argv'] == ['python3', '-B', 'scripts/verify-interrupt-bringup.py', 'verify', 'current'] and
              actual.resolve() == (attempt / 'evidence').resolve() and
              command['started_ns'] > original['finished_ns'] and
              command['finished_ns'] > command['started_ns'] and
              E.sha(output) == command['stdout_sha256'] and output.read_text().strip() == '[IRQ][EVIDENCE] PASS',
              'completed native replay verification binding')
    return dict(original, status='PASS', reason=None)


def reverify_interrupts(campaign, attempt):
    attempt.relative_to(campaign / 'regression-attempts')
    original = read(attempt / 'result.json')
    E.require(original['status'] == 'FAIL' and original['key'] == 'interrupts' and
              original['commands'] and all(read(attempt / n)['exit_status'] == 0 for n in original['commands']),
              'only completed interrupt collection can be replayed')
    replay_family(campaign, 'interrupts', attempt, result=dict(original, status='PASS', reason=None))
    G = load_module('memory-observation-gate')
    proof = G.run(attempt, 'completed-native-replay',
                  ['python3', '-B', 'scripts/verify-interrupt-bringup.py', 'verify', 'current'],
                  {'HOBBYOS_TEST_ARTIFACT': str(attempt / 'evidence'),
                   'HOBBYOS_EXPECTED_BRANCH': 'feat/memory-hardening'})
    E.write(attempt / 'replayed-result.json', {'schema': 1, 'status': 'PASS', 'fresh_boots': 0,
        'build_input_sha256': E.identity(E.manifest()), 'harness_sha256': E.identity(E.manifest(harness=True)),
        'original_sha256': E.sha(attempt / 'result.json'), 'started_sha256': E.sha(attempt / 'started.json'),
        'commands': {n: E.sha(attempt / n) for n in original['commands']},
        'verification': proof.relative_to(attempt).as_posix(), 'verification_sha256': E.sha(proof / 'command.json')})
    replay_family(campaign, 'interrupts', attempt)
    selected = read(campaign / 'regression-selection.json')
    E.require('interrupts' not in selected, 'interrupt family already selected')
    selected['interrupts'] = attempt.relative_to(campaign).as_posix()
    temporary = campaign / 'regression-selection.pending'; E.write(temporary, selected)
    os.replace(temporary, campaign / 'regression-selection.json')


def reject_case(name, expected, operation):
    try:
        operation()
    except Exception as error:
        E.require(expected in str(error), 'wrong rejection boundary for ' + name + ': ' + str(error))
        return {'name': name, 'synthetic': True, 'expected_boundary': expected,
                'reason': str(error), 'expected_exit': 1, 'actual_exit': 1}
    raise ValueError('negative accepted: ' + name)


def assertion_records(root, assertions, *, validated_matrix=None):
    """Use the native context matrix and strict record parser on fresh captures."""
    V = load_module('verify-assert-record-evidence')
    A = load_module('verify-assert-list-evidence')
    # Portable callers separately replay every native runtime record, with
    # omitted executable identity checked against the transport index.
    matrix = A.verify_all(assertions) if validated_matrix is None else validated_matrix
    host = V.load_json(root / 'host/result.json')
    V.require(host.get('status') == 'PASS' and host.get('profiles') == ['normal', 'ubsan', 'asan']
              and host.get('cases') == 9 and host.get('records') == 4 and host.get('record_capacity') == 96,
              'HOST_COVERAGE_INVALID')
    for profile in host['profiles']:
        directory = root / 'host' / profile
        V.require((directory / 'run-exit-code.txt').read_text().strip() == '0', 'HOST_EXIT_INVALID')
        V.require(re.search(r'^\[ASSERT_RECORD_HOST\]\[SUITE_END\] status=PASS cases=9 assertions=[1-9][0-9]* failures=0 records=4 capacity=96$',
                            (directory / 'run.log').read_text(), re.M), 'HOST_SUMMARY_INVALID')
    E.require(read(root / 'static/result.json')['status'] == 'PASS', 'assertion source invariants')
    campaign = read(assertions / 'campaign.json')
    records = []
    cases = []
    for row in campaign['planned_runs']:
        raw = assertions / 'runtime' / row['name'] / 'serial-transcript.log'
        transcript = raw.read_text()
        context = row['scenario'] == 'warn' or row['scenario'] not in A.NONFATAL
        complete = row['scenario'] in A.NONFATAL
        scenario = A.SCENARIOS[row['scenario']]
        V.validate_transcript(transcript, scenario, context, complete)
        records.append({'run': row['name'], 'serial_sha256': E.sha(raw), 'scenario': scenario,
                        'context': context, 'complete': complete})
        if not cases:
            execute = f'[ASSERT_TEST][EXECUTE] scenario={scenario}'
            mutations = [
                ('split', transcript.replace(execute, '[ASSERT_TEST][EXECUTE] scenario=[BOOT][PROGRESS]\n' + str(scenario), 1), 'EXECUTE_SCENARIO_INVALID'),
                ('interleaved', transcript.replace(execute, execute + ' [BOOT][PROGRESS] stage=x', 1), 'EXECUTE_FIELD_INVALID'),
                ('missing-field', transcript.replace(execute, '[ASSERT_TEST][EXECUTE] unrelated=1', 1), 'EXECUTE_FIELD_SET_INVALID'),
            ]
            for name, value, boundary in mutations:
                case = reject_case(name, boundary, lambda text=value: V.validate_transcript(text, scenario, context, complete))
                case.update(base_sha256=E.sha(raw), materialized_sha256=E.identity(value), recipe=name, base_run=row['name'])
                cases.append(case)
    E.require(records and len(cases) == 3, 'fresh assertion mutation coverage')
    return {'status': 'PASS', 'contract': 'current-assertion-record', 'matrix': matrix,
            'records': records, 'mutations': cases, 'historical': 'HISTORICAL_REPLAY_UNAVAILABLE'}


def statistics_records(root):
    V = load_module('verify-taskman-stats-evidence')
    result = read(root / 'result.json')
    E.require(result['historical_split_record_status'] == 'HISTORICAL_REPLAY_UNAVAILABLE', 'historical status falsely promoted')
    E.require(result['field_count'] == 52 and result['record_capacity'] == 1953 and
              result['qemu_profiles'] == list(V.QEMU_PROFILES), 'current statistics coverage')
    candidate = E.sha(root / 'candidate/kernel.elf')
    E.require(result['candidate_sha256'] == candidate, 'statistics candidate identity')
    errors = V.verify_sources(root, result['implementation_base_commit'], False) + V.verify_host(root)
    E.require(not errors, 'current statistics: ' + '; '.join(errors))
    return statistics_runtime_records(root, candidate)


def statistics_runtime_records(root, candidate, *, transport_replay=None):
    """Shared raw runtime and fresh-mutation replay, after source/host validation."""
    V = load_module('verify-taskman-stats-evidence')
    result = read(root / 'result.json')
    E.require(result['historical_split_record_status'] == 'HISTORICAL_REPLAY_UNAVAILABLE' and
              result['field_count'] == 52 and result['record_capacity'] == 1953 and
              result['qemu_profiles'] == list(V.QEMU_PROFILES) and result['candidate_sha256'] == candidate,
              'current statistics contract/candidate')
    for name in V.QEMU_PROFILES:
        errors = V.verify_qemu_profile(root, name, candidate, transport_replay=transport_replay)
        E.require(not errors, 'current statistics runtime: ' + '; '.join(errors))
    cases, records = [], []
    for profile in V.QEMU_PROFILES:
        raw = root / 'qemu' / profile / 'qemu/qemu-serial.log'
        lines = [line for line in raw.read_text().splitlines() if line.startswith('[TASKMANTEST][STATS] ')]
        E.require(lines, 'fresh statistics record missing')
        for line in lines: V.parse_stats_record(line)
        records.append({'profile': profile, 'records': len(lines), 'serial_sha256': E.sha(raw)})
        if not cases:
            line = lines[0]
            parts = line.split(' ')
            variants = [('split', line.replace('sessions=', 'sessions=[BOOT][PROGRESS]\n', 1)),
                        ('truncated', line[:-12]), ('missing-field', ' '.join(parts[:-1])),
                        ('malformed', line + ' unexpected=1')]
            for name, value in variants:
                try:
                    V.parse_stats_record(value)
                except V.EvidenceError as error:
                    cases.append({'name': name, 'synthetic': True, 'recipe': name, 'base_profile': profile,
                                  'base_sha256': E.identity(line), 'materialized_sha256': E.identity(value),
                                  'reason': str(error), 'expected_exit': 1, 'actual_exit': 1})
                else: raise ValueError('fresh malformed statistics accepted: ' + name)
    E.require(len(cases) == 4, 'fresh statistics mutation coverage')
    return {'status': 'PASS', 'contract': 'current-taskman-statistics', 'candidate': candidate,
            'records': records, 'mutations': cases, 'historical': 'HISTORICAL_REPLAY_UNAVAILABLE'}


def modal_current(campaign):
    """Replay the maintained ownership and outer-resource checkpoint validators."""
    selected = read(campaign / 'regression-selection.json')
    attempt = campaign / selected['formatting']
    E.require(read(attempt / 'result.json')['status'] == 'PASS', 'modal source campaign incomplete')
    root = attempt / 'evidence'
    V = load_module('verify-libc-format-evidence')
    errors = V.verify_evidence(root)
    E.require(not errors, 'current modal checkpoint: ' + '; '.join(errors))
    result = read(root / 'result.json')
    consumers = read(root / result['consumer_result'])
    expected = [f'q35-kvm-smp{smp}-run{repeat}' for smp, count in ((4, 2), (8, 3))
                for repeat in range(1, count + 1)]
    E.require(consumers['modal_heap_profiles'] == expected, 'current modal repetition contract')
    return {'status': 'PASS', 'contract': 'current-modal-ownership-and-resource-checkpoint',
            'source_family': selected['formatting'], 'family_receipt_sha256': E.sha(attempt / 'result.json'),
            'consumer_result_sha256': E.sha(root / result['consumer_result']), 'profiles': expected,
            'invariant_authority': 'docs/test-reports/TMV1-CL-13-FIX14-modal-ownership-vs-global-heap.md',
            'legacy_classification': 'NOT_REPLAYED_DEPENDENCY_UNAVAILABLE',
            'legacy_mode': 'scripts/test-cl13-modal-heap-focused.sh all',
            'legacy_capture': 'artifacts/build/cl13fix12-soak8-kvm-failed.log'}


def inventory():
    value = read(REQUIREMENTS)
    keys = [r['key'] for r in value['requirements']]
    E.require(len(keys) == len(set(keys)) and len(keys) == 17, 'fixed requirement inventory invalid')
    return value


def requirement_binding(key, started, result):
    """Keep a captured inventory-description correction explicit and bounded."""
    captured = result['requirement']
    current = next(r for r in inventory()['requirements'] if r['key'] == key)
    E.require(started['requirement'] == captured, 'regression captured requirement changed')
    if captured == current: return {'status': 'UNCHANGED'}
    earlier = copy.deepcopy(current)
    earlier['profiles'] = [p.replace('modal-q35-kvm-smp4:2', 'modal-q35-kvm-smp4:3') for p in earlier['profiles']]
    E.require(key == 'formatting' and captured == earlier, 'unapproved regression requirement revision')
    source = 'scripts/verify-libc-format-evidence.py'
    digest = '5d5010938475c17ecff1bfdec514b4c4a9d91eb7475227aea02e733860f588b3'
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        contract = device_current.contract()
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
            prior = read(ROOT / 'scripts/device-mapping-scope.json')['promoted_sources'][source]
            E.require(prior['baseline_sha256'] == digest, 'formatting historical baseline changed')
            digest = prior['candidate_sha256']
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
            prior = read(ROOT / 'scripts/direct-map-scope.json')['promoted_sources'][source]
            E.require(prior['baseline_sha256'] == digest, 'formatting RAM predecessor changed')
            digest = prior['candidate_sha256']
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
            prior=read(ROOT/'scripts/low-address-scope.json')['promoted_sources'][source]
            E.require(prior['baseline_sha256']==digest,'formatting no-low predecessor changed')
            digest=prior['candidate_sha256']
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            anchor=read(ROOT/'scripts/kernel-image-scope.json')['promoted_sources'][source]
            E.require(anchor['baseline_sha256']==digest,'formatting image predecessor changed')
            digest=anchor['candidate_sha256']
            anchor=contract['promoted_sources'][source]
            E.require(anchor['baseline_sha256']==digest,
                'formatting stack successor predecessor changed')
            digest=anchor['candidate_sha256']
        else:
            anchor = contract['promoted_sources'][source]
            E.require(anchor['baseline_sha256'] == digest, 'formatting successor baseline changed')
            digest = anchor['candidate_sha256']
    E.require(E.sha(ROOT / source) == digest, 'maintained formatting contract changed')
    return {'status': 'CORRECTED_DESCRIPTION', 'captured_requirement': captured,
            'current_requirement': current, 'invariant_source': source, 'invariant_sha256': digest,
            'reason': 'The unchanged native all-mode requires two SMP4 and three SMP8 modal consumer runs; the initial inventory description misstated the SMP4 count.'}


def ledger_policy(ledger, candidate_source):
    """Policy-only entry point; raw verification is mandatory in verify_ledger."""
    fixed = inventory()
    E.require(ledger.get('schema') == 1 and ledger.get('inventory_sha256') == E.sha(REQUIREMENTS), 'regression inventory identity')
    rows = ledger.get('rows', [])
    keys = [r.get('key') for r in rows]
    E.require(len(keys) == len(set(keys)), 'duplicate regression requirement')
    E.require(set(keys) == {r['key'] for r in fixed['requirements']}, 'incomplete regression inventory')
    by_key = {r['key']: r for r in rows}
    for requirement in fixed['requirements']:
        row = by_key[requirement['key']]
        E.require(row['requirement'] == requirement, 'wrong regression mode/profile/evidence contract')
        E.require(row['status'] == 'PASS' and row['exit_status'] == 0, 'nonzero/unavailable regression result')
        E.require(row['build_input_sha256'] == candidate_source, 'stale regression candidate inputs')
        E.require(row.get('raw') and row.get('commands') and row.get('replay'), 'missing raw regression proof')
    E.require(ledger.get('historical_unavailable') == fixed['historical_unavailable'], 'historical authority mapping changed')


def verify_ledger(root, source, *, ledger=None):
    if ledger is None: ledger = read(root / 'regression-ledger.json')
    ledger_policy(ledger, source)
    E.require(ledger['replay_harness_sha256'] == E.identity(E.manifest(harness=True)), 'regression replay verifier version changed')
    for row in ledger['rows']:
        attempt = root / row['attempt']
        E.safe(row['attempt'])
        listed = [item['path'] for item in row['raw']]
        E.require(len(listed) == len(set(listed)), 'duplicate raw regression output')
        actual = {p.relative_to(root).as_posix() for p in attempt.rglob('*') if p.is_file()}
        E.require(set(listed) == actual, 'missing or unlisted raw regression output')
        for item in row['raw']:
            E.safe(item['path']); path = root / item['path']
            E.require(path.resolve().is_relative_to(attempt.resolve()) and path.is_file() and not path.is_symlink(), 'missing raw regression output')
            E.require(path.stat().st_size == item['size'] and E.sha(path) == item['sha256'], 'changed raw regression output')
        result = family_result(attempt)
        E.require(row['requirement_binding'] == requirement_binding(row['key'], read(attempt / 'started.json'), result),
                  'regression requirement authority receipt differs')
        E.require(row['commands'] == [str(Path(row['attempt']) / p) for p in result['commands']], 'regression command inventory differs')
        expected_commands = family_commands(row['key'], attempt / 'evidence', root)
        E.require(len(expected_commands) == len(row['commands']), 'regression command count differs')
        for item, (argv, environment) in zip(row['commands'], expected_commands):
            receipt = read(root / item)
            E.require(receipt['exit_status'] == 0 and receipt['build_input_sha256'] == source, 'failed/stale regression command receipt')
            E.require(receipt['argv'] == argv and receipt['environment'] == environment and
                      receipt['started_ns'] < receipt['finished_ns'], 'regression command/options/chronology differs')
            E.require(E.sha((root / item).with_suffix('.stdout.log')) == receipt['stdout_sha256'], 'regression command raw output differs')
        E.require(row['replay'] == {'family': row['key'], 'attempt': row['attempt']}, 'regression replay contract differs')
        replay_family(root, row['key'], attempt)
    return {'status': 'PASS', 'requirements': len(ledger['rows']), 'historical': ledger['historical_unavailable']}


NATIVE = {
    'build-profile': ('BUILD_PROFILE_EVIDENCE_DIR', 'verify-build-profile.py'),
    'architecture': ('ARCH_EVIDENCE_DIR', 'verify-arch-contract.py'),
    'assertion-context': ('ASSERT_EVIDENCE_DIR', 'verify-assert-list-evidence.py'),
    'assertion-record': ('ASSERT_RECORD_EVIDENCE_DIR', None),
    'taskman-statistics': ('STATS_EVIDENCE_DIR', None),
    'section-layout': ('SECTION_EVIDENCE_DIR', None),
    'formatting': ('FORMAT_EVIDENCE_DIR', 'verify-libc-format-evidence.py'),
    'panic': ('PANIC_EVIDENCE_DIR', 'verify-panic-evidence.py'),
    'foundation': ('FOUNDATION_EVIDENCE_DIR', 'verify-foundation-regression.py'),
    'interrupts': ('HOBBYOS_TEST_ARTIFACT', 'verify-interrupt-bringup.py'),
    'clockevent': ('HOBBYOS_TIMER_ARTIFACT', 'verify-timer-clockevent.py'),
    'lock-progress': ('LOCK_PROGRESS_EVIDENCE_DIR', 'verify-lock-progress.py'),
    'allocator': ('ALLOC_HARDENING_EVIDENCE_DIR', 'verify-alloc-hardening.py'),
    'modal-heap': ('MODAL_HEAP_EVIDENCE_DIR', None),
    'taskman-visual': ('TASKMAN_VISUAL_EVIDENCE_DIR', 'verify-taskman-visual-evidence.py'),
}
STACK_RUNTIME_FAMILIES={'build-profile','architecture','assertion-context','taskman-statistics',
    'formatting','panic','foundation','interrupts','clockevent','lock-progress','allocator','taskman-visual'}


def replay_family(campaign, key, attempt, result=None):
    result = family_result(attempt) if result is None else result
    requirement_binding(key, read(attempt / 'started.json'), result)
    E.require(result['status'] == 'PASS' and result['key'] == key, 'uncompleted regression family')
    E.require(result['build_input_sha256'] == E.identity(E.manifest()), 'regression source changed')
    directory = attempt / 'evidence'
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', HOBBYOS_EXPECTED_BRANCH='feat/memory-hardening')
    commands = []
    if key in ('build-profile', 'architecture', 'assertion-context', 'formatting', 'lock-progress', 'allocator'):
        commands = [['python3', 'scripts/' + NATIVE[key][1], 'all', str(directory)]]
    elif key == 'panic':
        commands = [['python3', 'scripts/verify-panic-evidence.py', 'tree', str(directory / 'runtime')]]
    elif key == 'assertion-record':
        selected = read(campaign / 'regression-selection.json')
        actual = assertion_records(directory, campaign / selected['assertion-context'] / 'evidence')
        E.require(read(directory / 'current-result.json') == actual, 'current assertion replay differs')
    elif key == 'taskman-statistics':
        E.require(read(directory / 'current-result.json') == statistics_records(directory), 'current statistics replay differs')
    elif key == 'section-layout':
        commands = [['python3', 'scripts/memory-current-layout.py', 'verify', '--evidence', str(directory)]]
    elif key == 'taskman-visual':
        commands = [['python3', 'scripts/verify-taskman-visual-evidence.py', 'current', '--evidence', str(directory),
                     '--candidate', str(candidates.resolve(campaign / 'candidates/selftest')), '--replay']]
    elif key in ('interrupts', 'clockevent'):
        env[NATIVE[key][0]] = str(directory)
        commands = [['python3', 'scripts/' + NATIVE[key][1], 'verify', 'current' if key == 'interrupts' else 'matrix']]
    elif key == 'foundation':
        commands = [['python3', 'scripts/verify-foundation-regression.py', 'all', str(directory)],
                    ['python3', 'scripts/verify-foundation-regression.py', 'production', str(directory / 'production-contract/production')],
                    ['python3', 'scripts/verify-foundation-regression.py', 'diagnostic-negative', str(directory / 'negative-contract/diagnostic-negative')]]
    elif key == 'modal-heap':
        E.require(read(directory / 'current-result.json') == modal_current(campaign), 'current modal replay differs')
    elif key == 'production-policy':
        policy = (attempt / 'command-1.stdout.log').read_text()
        E.require('[BOOT][PRODUCTION_TEST_POLICY] PASS' in policy and ' FAIL' not in policy, 'production policy raw failure')
        production_policy(directory, campaign, replay=True)
    elif key == 'canonical-builds':
        canonical_builds(campaign)
    else: raise ValueError('unknown regression replay family')
    for argv in commands:
        subprocess.run(argv, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
        import stack_native_audit
        actual=stack_native_audit.verify_family(directory,required=key in STACK_RUNTIME_FAMILIES)
        E.require(result.get('stack_services')==actual,'native stack audit terminal launch inventory differs')


def verify_ledger_revision(previous, current):
    """Only the replay harness may change when reusing completed raw results."""
    old = dict(previous); new = dict(current)
    old_harness = old.pop('replay_harness_sha256', None)
    new_harness = new.pop('replay_harness_sha256', None)
    E.require(old == new, 'ledger revision changed existing evidence or requirements')
    E.require(all(isinstance(value, str) and len(value) == 64 and
                  all(c in '0123456789abcdef' for c in value) for value in (old_harness, new_harness)) and
              old_harness != new_harness, 'ledger revision requires distinct valid harness identities')


def finalize(campaign):
    selected = read(campaign / 'regression-selection.json')
    required = inventory()
    E.require(set(selected) == {r['key'] for r in required['requirements']}, 'incomplete selected regression families')
    rows = []
    source = E.identity(E.manifest())
    for requirement in required['requirements']:
        key = requirement['key']; relative = selected[key]; E.safe(relative)
        attempt = campaign / relative
        replay_family(campaign, key, attempt)
        result = family_result(attempt)
        commands = []
        for name in result['commands']:
            record = read(attempt / name)
            E.require(record['exit_status'] == 0, 'failed command cannot close family')
            commands.append((attempt / name).relative_to(campaign).as_posix())
        raw = []
        for p in sorted(attempt.rglob('*')):
            if not p.is_file(): continue
            E.require(not p.is_symlink(), 'regression raw symlink')
            raw.append({'path': p.relative_to(campaign).as_posix(), 'size': p.stat().st_size, 'sha256': E.sha(p)})
        rows.append({'key': key, 'requirement': requirement, 'attempt': relative,
                     'requirement_binding': requirement_binding(key, read(attempt / 'started.json'), result),
                     'status': 'PASS', 'exit_status': 0, 'build_input_sha256': result['build_input_sha256'],
                     'raw': raw, 'commands': commands, 'replay': {'family': key, 'attempt': relative}})
    ledger = {'schema': 1, 'inventory_sha256': E.sha(REQUIREMENTS), 'rows': rows,
              'replay_harness_sha256': E.identity(E.manifest(harness=True)),
              'historical_unavailable': required['historical_unavailable']}
    ledger_policy(ledger, source)
    verified = verify_ledger(campaign, source, ledger=ledger)
    path = campaign / 'regression-ledger.json'
    if path.exists():
        previous_bytes = path.read_bytes()
        previous = json.loads(previous_bytes)
        if previous != ledger:
            E.require(not (campaign / 'acceptance.json').exists() and
                      not (campaign / 'control-acceptance.json').exists(),
                      'accepted campaign requires a new verification campaign')
            verify_ledger_revision(previous, ledger)
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
            revision = campaign / 'attempts' / (stamp + '-ledger-reverification')
            revision.mkdir(parents=True, exist_ok=False)
            (revision / 'previous-ledger.json').write_bytes(previous_bytes)
            E.write(revision / 'replayed-ledger.json', ledger)
            E.write(revision / 'replay.json', {
                'schema': 1, 'kind': 'native-ledger-reverification', 'status': verified['status'],
                'previous_sha256': E.sha(revision / 'previous-ledger.json'),
                'replayed_sha256': E.sha(revision / 'replayed-ledger.json'),
                'previous_harness_sha256': previous['replay_harness_sha256'],
                'harness_sha256': ledger['replay_harness_sha256'],
                'build_input_sha256': source, 'existing_raw_rows': 'UNCHANGED'})
            temporary = path.with_suffix('.pending')
            E.require(not temporary.exists() and path.read_bytes() == previous_bytes,
                      'ledger publication changed during replay')
            temporary.write_bytes(E.canonical(ledger)); os.replace(temporary, path)
    else:
        E.write(path, ledger)
    return verified


def family_commands(key, directory, campaign):
    requirement = next(r for r in inventory()['requirements'] if r['key'] == key)
    env = {'JOBS': '4', 'PYTHONDONTWRITEBYTECODE': '1', 'HOBBYOS_EXPECTED_BRANCH': 'feat/memory-hardening'}
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('cpu-control-state', 'tlb-coherency', 'physical-translation', 'owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        env['HOBBYOS_VALIDATION_CONTRACT'] = os.environ['HOBBYOS_VALIDATION_CONTRACT']
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
        env['HOBBYOS_STACK_NATIVE_AUDIT']=str(directory/'service-audits')
    if key in NATIVE: env[NATIVE[key][0]] = str(directory)
    if key == 'formatting' and (campaign / 'formatting-reuse.json').is_file():
        reuse = read(campaign / 'formatting-reuse.json')
        E.safe(reuse['attempt'])
        E.require(reuse['schema'] == 1 and reuse['build_input_sha256'] == E.identity(E.manifest()) and
                  reuse['attempt'].startswith('regression-attempts/'), 'formatting reuse source/attempt')
        return [(['python3', '-B', 'scripts/memory-current-regression.py', 'resume-formatting',
                  '--evidence', str(directory), '--campaign', str(campaign)], env)]
    candidate = candidates.resolve(campaign / 'candidates/selftest')
    if key == 'canonical-builds':
        return [(['python3', 'scripts/memory-current-regression.py', 'canonical', '--evidence', str(campaign)], env)]
    if key == 'assertion-record':
        selected = read(campaign / 'regression-selection.json')
        env['MEMORY_ASSERTION_EVIDENCE'] = str(campaign / selected['assertion-context'] / 'evidence')
    if key == 'taskman-visual': env['MEMORY_CURRENT_CANDIDATE'] = str(candidate)
    if key == 'modal-heap': env['MEMORY_CURRENT_CAMPAIGN'] = str(campaign)
    if key in ('interrupts', 'clockevent'):
        # Unix socket paths have a small fixed limit independent of the evidence
        # path length. Each immutable attempt gets its own compact runtime.
        env['HOBBYOS_TEST_RUNTIME' if key == 'interrupts' else 'HOBBYOS_TIMER_RUNTIME'] = '/tmp/memory-' + directory.parent.name
    if key == 'foundation':
        return [([requirement['family'], 'existing-all', str(candidate), 'memory-diagnostic-current'], env),
                ([requirement['family'], 'production'], dict(env, FOUNDATION_EVIDENCE_DIR=str(directory / 'production-contract'))),
                ([requirement['family'], 'diagnostic-negative'], dict(env, FOUNDATION_EVIDENCE_DIR=str(directory / 'negative-contract')))]
    if key == 'section-layout':
        env['SECTION_CANDIDATE_MANIFEST'] = str(campaign / 'layout-roles.json')
    if key == 'production-policy':
        return [(['make', 'production-image', 'JOBS=4'], env), ([requirement['family']], env),
                (['python3', 'scripts/memory-current-regression.py', 'policy', '--evidence', str(directory), '--campaign', str(campaign)], env)]
    E.require(key in NATIVE, 'family requires explicit adapter: ' + key)
    return [([requirement['family'], requirement['mode']], env)]


def canonical_builds(campaign):
    G = load_module('memory-observation-gate')
    E.require(read(campaign / 'build.json')['status'] == 'PASS', 'canonical build collection incomplete')
    for profile in ('debug', 'selftest', 'production'): G.verify_local_candidate(campaign / 'candidates' / profile)
    found = set()
    for base in (campaign / 'attempts', campaign / 'reused-build-commands'):
        for path in base.glob('*/command.json'):
            name = path.parent.name.split('Z-', 1)[-1]
            if name not in ('deps-check', 'debug-stack', 'production-stack', 'selftest-stack'): continue
            record = read(path)
            E.require(record['exit_status'] == 0, 'canonical prerequisite or stack check failed')
            if name.endswith('-stack'): E.require('stack-check: PASS' in path.with_name('stdout.log').read_text(), 'stack output missing')
            found.add(name)
    E.require(found == {'deps-check', 'debug-stack', 'production-stack', 'selftest-stack'}, 'canonical prerequisite/stack inventory incomplete')
    return {'status': 'PASS', 'build_input_sha256': E.identity(E.manifest()), 'checks': sorted(found)}


def resume_formatting(directory, campaign):
    """Replay captured native work after an exact source-only predicate correction."""
    import validation_resources
    reuse = read(campaign / 'formatting-reuse.json')
    E.safe(reuse['attempt'])
    origin = campaign / reuse['attempt']
    E.require(reuse['schema'] == 1 and reuse['build_input_sha256'] == E.identity(E.manifest()) and
              E.sha(origin / 'result.json') == reuse['failed_receipt_sha256'] and
              read(origin / 'result.json')['status'] == 'FAIL' and
              read(origin / 'started.json')['build_input_sha256'] == reuse['build_input_sha256'],
              'formatting reuse failed-attempt identity')
    output = (origin / 'command-0.stdout.log').read_text()
    expected_rejection = 'FORMAT_EVIDENCE_REJECTED: boot progress no longer emits one complete line\n'
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        expected_rejection = 'FORMAT_EVIDENCE_REJECTED: constant-only paging diagnostics were needlessly migrated\n'
    E.require(output.endswith(expected_rejection),
              'formatting reuse is not the named source-only rejection')
    V = load_module('verify-libc-format-evidence')
    errors = V.verify_evidence(origin / 'evidence')
    E.require(not errors, 'formatting reuse raw/native predicate: ' + '; '.join(errors))
    E.require(not directory.exists(), 'formatting successor output exists')
    rows = []
    for path in sorted((origin / 'evidence').rglob('*')):
        E.require(not path.is_symlink(), 'formatting reuse symlink')
        if path.is_dir(): continue
        E.require(path.is_file(), 'formatting reuse nonregular input')
        relative = path.relative_to(origin / 'evidence')
        destination = directory / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        validation_resources.sparse_copy(path, destination)
        rows.append({'path': relative.as_posix(), 'size': path.stat().st_size,
                     'mode': path.stat().st_mode & 0o777, 'sha256': E.sha(path)})
    V.run_fixtures(directory / 'oracle-fixtures')
    errors = V.verify_evidence(directory)
    E.require(not errors, 'formatting successor native predicate: ' + '; '.join(errors))
    E.write(directory / 'reuse.json', {'schema': 1, 'status': 'PASS', 'source': reuse,
            'raw_files': rows, 'raw_file_count': len(rows), 'fresh_boots': 0,
            'predicate': 'Native source, hosted C, exact runtime and modal/input predicates; regenerated negatives.',
            'harness_sha256': E.identity(E.manifest(harness=True))})
    print(json.dumps({'status': 'PASS', 'reused_raw_files': len(rows), 'fresh_boots': 0}))


def production_policy(directory, campaign, replay=False):
    G = load_module('memory-observation-gate')
    reference = read(campaign / 'candidates/production/candidate.json')
    receipt = read(campaign / 'candidates/production/build-receipt.json')
    E.verify_candidate(reference, receipt)
    if not replay:
        directory.mkdir(parents=True, exist_ok=False)
        for name in ('kernel.elf', 'BOOTX64.EFI', 'hobbyos.img'): shutil.copyfile(ROOT / name, directory / name)
    for name in ('kernel.elf', 'BOOTX64.EFI'):
        E.require(E.sha(directory / name) == reference['artifacts'][name]['sha256'], 'production policy candidate payload changed')
    absence = G.absent(directory)
    payloads = {}
    with tempfile.TemporaryDirectory(prefix='memory-policy-payload-') as temp:
        for name, guest in [('kernel.elf', '::/kernel.elf'), ('BOOTX64.EFI', '::/EFI/BOOT/BOOTX64.EFI'),
                            ('zap-light16.psf', '::/EFI/fonts/zap-light16.psf'), ('logo.bmp', '::/EFI/images/logo.bmp'),
                            ('startup.nsh', '::/startup.nsh')]:
            target = Path(temp) / name
            subprocess.run(['mcopy', '-i', str(directory / 'hobbyos.img'), guest, str(target)], check=True)
            payloads[name] = {'size': target.stat().st_size, 'sha256': E.sha(target)}
    E.require(payloads == receipt['payload']['files'], 'production policy FAT payload differs')
    result = {'status': 'PASS', 'build_input_sha256': E.identity(E.manifest()), 'absence': absence,
              'image_sha256': E.sha(directory / 'hobbyos.img'), 'payloads': payloads}
    if replay: E.require(read(directory / 'policy-proof.json') == result, 'production policy replay differs')
    else: E.write(directory / 'policy-proof.json', result)
    return result


def layout_roles(campaign):
    selected = read(campaign / 'regression-selection.json')
    paths = {k: campaign / v / 'evidence' for k, v in selected.items()}
    definitions = [
        ('assertions-debug', 'assertion-context', 'builds/instrumented/kernel.elf', {'SELFTEST': 1, 'DEBUG_ASSERT': 1, 'ASSERT_TEST': 1}),
        ('formatting-test', 'formatting', 'candidate/kernel.elf', {'SELFTEST': 1, 'DEBUG_ASSERT': 1, 'FORMAT_TEST': 1}),
        ('panic-instrumented', 'panic', 'builds/instrumented-final/kernel.elf', {'SELFTEST': 1, 'DEBUG_ASSERT': 1, 'KERNEL_EXTRA_CFLAGS': '-DHOBBYOS_PANIC_TEST'}),
        ('foundation-framed', 'foundation', 'candidate/kernel.elf', {'SELFTEST': 1, 'DEBUG_ASSERT': 1}),
        ('architecture-instrumented', 'architecture', 'candidates/instrumented/kernel.elf', {'SELFTEST': 1, 'DEBUG_ASSERT': 1, 'ARCH_TEST': 1}),
        ('architecture-negative', 'architecture', 'candidates/negative/kernel.elf', {'SELFTEST': 1, 'DEBUG_ASSERT': 1, 'ARCH_TEST': 1, 'ARCH_NEGATIVE_GP': 1}),
    ]
    rows = []
    for role, key, name, flags in definitions:
        elf = paths[key] / name
        receipt = campaign / 'layout-role-receipts' / (role + '.json'); receipt.parent.mkdir(exist_ok=True)
        E.require(elf.is_file(), 'current layout role binary missing: ' + role)
        attempt = campaign / selected[key]
        E.require(read(attempt / 'result.json')['status'] == 'PASS', 'layout role family not accepted')
        E.write(receipt, {'role': role, 'elf_sha256': E.sha(elf), 'family_receipt': selected[key] + '/result.json',
                         'family_receipt_sha256': E.sha(attempt / 'result.json'), 'configuration': flags,
                         'build_input_sha256': E.identity(E.manifest())})
        rows.append({'role': role, 'elf': str(elf), 'receipt': str(receipt), 'configuration': flags})
    candidate = candidates.resolve(campaign / 'candidates/production')
    rows.append({'role': 'production-debug-off', 'elf': str(candidate / 'kernel.elf'),
                 'receipt': str(candidate / 'candidate.json'), 'configuration': {'DEBUG_ASSERT': 0, 'SELFTEST': 0}})
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        candidate=candidates.resolve(campaign/'candidates/framebuffer-reference')
        load_module('memory-observation-gate').verify_local_candidate(candidate)
        rows.append({'role':'framebuffer-reference','elf':str(candidate/'kernel.elf'),
            'receipt':str(candidate/'candidate.json'),'configuration':{
                'SELFTEST':1,'DEBUG_ASSERT':1,'SELFTEST_AUTORUN':0,'FRAMEBUFFER_REFERENCE':1}})
    E.write(campaign / 'layout-roles.json', {'schema': 1, 'candidates': rows})


def collect_all(campaign):
    order = ['canonical-builds', 'build-profile', 'architecture', 'assertion-context', 'assertion-record',
             'taskman-statistics', 'formatting', 'panic', 'foundation', 'interrupts', 'clockevent',
             'lock-progress', 'allocator', 'modal-heap', 'taskman-visual', 'section-layout', 'production-policy']
    for key in order:
        selection = read(campaign / 'regression-selection.json') if (campaign / 'regression-selection.json').is_file() else {}
        if key in selection:
            replay_family(campaign, key, campaign / selection[key])
            continue
        if key == 'section-layout' and not (campaign / 'layout-roles.json').exists(): layout_roles(campaign)
        collect_family(campaign, key)
    return finalize(campaign)


def collect_family(campaign, key):
    """Each invocation owns a new immutable attempt, including failed invocations."""
    import validation_resources
    validation_resources.before_step(campaign, 'native')
    requirement = next(r for r in inventory()['requirements'] if r['key'] == key)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    attempt = campaign / 'regression-attempts' / (stamp + '-' + key)
    attempt.mkdir(parents=True, exist_ok=False)
    directory = attempt / 'evidence'
    before = E.manifest(); source = E.identity(before)
    harness = E.manifest(harness=True)
    E.write(attempt / 'started.json', {'schema': 1, 'requirement': requirement, 'build_inputs': before,
            'build_input_sha256': source, 'harness': harness, 'harness_sha256': E.identity(harness),
            'started_utc': stamp, 'started_ns': time.monotonic_ns()})
    commands = []
    status = 'FAIL'; reason = None;stack_services=None
    try:
        for index, (argv, environment) in enumerate(family_commands(key, directory, campaign)):
            record = {'schema': 1, 'argv': argv, 'environment': environment, 'build_input_sha256': source,
                      'started_ns': time.monotonic_ns(), 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
            with (attempt / f'command-{index}.stdout.log').open('xb') as out:
                proc = subprocess.run(argv, cwd=ROOT, env=dict(os.environ, **environment), stdout=out, stderr=subprocess.STDOUT)
                out.flush(); os.fsync(out.fileno())
            record.update(exit_status=proc.returncode, finished_ns=time.monotonic_ns(),
                          stdout_sha256=E.sha(attempt / f'command-{index}.stdout.log'))
            E.write(attempt / f'command-{index}.json', record); commands.append(f'command-{index}.json')
            E.require(proc.returncode == 0, f'{key}: command returned {proc.returncode}: {argv}; see {attempt}')
        E.require(E.identity(E.manifest()) == source, 'build inputs changed during inherited regression')
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            import stack_native_audit
            stack_services=stack_native_audit.verify_family(directory,required=key in STACK_RUNTIME_FAMILIES)
        replay_family(campaign, key, attempt, result={'status':'PASS', 'key':key,
                      'requirement':requirement, 'commands':commands, 'build_input_sha256':source,
                      'stack_services':stack_services})
        status = 'PASS'
    except Exception as error:
        reason = str(error)
        raise
    finally:
        E.write(attempt / 'result.json', {'status': status, 'reason': reason, 'key': key,
                'requirement': requirement, 'commands': commands, 'build_input_sha256': source,
                'stack_services':stack_services,
                'finished_ns': time.monotonic_ns(), 'finished_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()})
    # The selection is a resumable pointer; immutable attempts remain the authority.
    selection_path = campaign / 'regression-selection.json'
    selection = read(selection_path) if selection_path.is_file() else {}
    selection[key] = attempt.relative_to(campaign).as_posix()
    temporary = selection_path.with_suffix('.pending')
    temporary.write_bytes(E.canonical(selection)); os.replace(temporary, selection_path)
    print(json.dumps({'status': status, 'family': key, 'attempt': str(attempt)}), flush=True)
    return attempt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    records = sub.add_parser('records')
    records.add_argument('family', choices=('assertion-record', 'taskman-statistics'))
    records.add_argument('--evidence', type=Path, required=True)
    records.add_argument('--assertions', type=Path)
    records.add_argument('--replay', action='store_true')
    verify = sub.add_parser('verify')
    verify.add_argument('--evidence', type=Path, required=True)
    verify.add_argument('--source', required=True)
    collect = sub.add_parser('family')
    collect.add_argument('key', choices=[r['key'] for r in inventory()['requirements']])
    collect.add_argument('--evidence', type=Path, required=True)
    final = sub.add_parser('finalize')
    final.add_argument('--evidence', type=Path, required=True)
    for mode in ('all', 'canonical', 'layout-roles'):
        item = sub.add_parser(mode); item.add_argument('--evidence', type=Path, required=True)
    modal = sub.add_parser('modal')
    modal.add_argument('--evidence', type=Path, required=True)
    modal.add_argument('--campaign', type=Path, required=True)
    policy = sub.add_parser('policy')
    policy.add_argument('--evidence', type=Path, required=True)
    policy.add_argument('--campaign', type=Path, required=True)
    resume = sub.add_parser('resume-formatting')
    resume.add_argument('--evidence', type=Path, required=True)
    resume.add_argument('--campaign', type=Path, required=True)
    replay = sub.add_parser('reverify-interrupts')
    replay.add_argument('--evidence', type=Path, required=True)
    replay.add_argument('--attempt', type=Path, required=True)
    args = parser.parse_args()
    if args.mode == 'reverify-interrupts':
        reverify_interrupts(args.evidence.resolve(), args.attempt.resolve()); return
    if args.mode == 'resume-formatting':
        resume_formatting(args.evidence.resolve(), args.campaign.resolve()); return
    if args.mode == 'family':
        collect_family(args.evidence.resolve(), args.key)
        return
    if args.mode == 'finalize':
        print(json.dumps(finalize(args.evidence.resolve()), sort_keys=True)); return
    if args.mode == 'modal':
        result = modal_current(args.campaign.resolve())
        args.evidence.mkdir(parents=True, exist_ok=False)
        E.write(args.evidence / 'current-scope.json', memory_current.scope('modal-heap'))
        E.write(args.evidence / 'current-result.json', result)
        print(json.dumps(result, sort_keys=True)); return
    if args.mode == 'policy':
        print(json.dumps(production_policy(args.evidence.resolve(), args.campaign.resolve()), sort_keys=True)); return
    if args.mode in ('all', 'canonical', 'layout-roles'):
        function = {'all': collect_all, 'canonical': canonical_builds, 'layout-roles': layout_roles}[args.mode]
        print(json.dumps(function(args.evidence.resolve()), sort_keys=True)); return
    if args.mode == 'records':
        result = (assertion_records(args.evidence, args.assertions) if args.family == 'assertion-record'
                  else statistics_records(args.evidence))
        path = args.evidence / 'current-result.json'
        if args.replay: E.require(read(path) == result, 'current record replay differs')
        else: E.write(path, result)
    else:
        result = verify_ledger(args.evidence, args.source)
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__': main()
