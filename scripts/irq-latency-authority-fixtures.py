#!/usr/bin/env python3
"""Semantic mutations of a captured host snapshot; no host settings change."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import irq_latency_authority as V


def run(snapshot, launch):
    original = json.loads(snapshot.read_text())
    launched = json.loads(launch.read_text())
    positive = V.verify_snapshot(original, launched)
    cases = []

    def check(name, mutate, expected):
        row = copy.deepcopy(original)
        mutate(row)
        try:
            V.verify_snapshot(row, launched)
        except (ValueError, KeyError) as error:
            if expected not in str(error):
                raise AssertionError(name+': '+str(error)) from error
            cases.append({'name': name, 'synthetic': True, 'expected_boundary': expected, 'actual_reason': str(error)})
        else:
            raise AssertionError('host mutation accepted: '+name)
        assert V.verify_snapshot(original, launched) == positive

    def quota(row):
        ancestor = next(v for v in row['cgroup']['ancestors'] if 'cpu.max' in v['fields'])
        ancestor['fields']['cpu.max'] = '400000 100000'
        row.update(capacity_numerator=4, capacity_denominator=1, capacity_sufficient=False)

    def native_quota(row):
        next(v for v in row['threads'][0]['cgroup']['ancestors'] if 'cpu.max' in v['fields'])['fields']['cpu.max'] = '100000 100000'

    def qmp_substitute(row):
        row['qmp_identity']['cpus'][0]['thread-id'] = 99999999
        for event in row['qmp_identity']['transcript']:
            if event.get('receive', {}).get('id') == 'irq-host-query-cpus-fast':
                event['receive']['return'] = copy.deepcopy(row['qmp_identity']['cpus'])

    check('effective-quota-four', quota, 'host oversubscription')
    check('reported-capacity-inflated', lambda r: r.update(capacity_numerator=1000), 'host reported capacity differs')
    check('missing-ancestor', lambda r: r['cgroup']['ancestors'].pop(1), 'host cgroup ancestor coverage')
    check('missing-controller-availability', lambda r: r['cgroup']['ancestors'][0]['fields'].pop('cgroup.controllers'), 'host controller availability unavailable')
    check('invented-no-quota', lambda r: next(v for v in r['cgroup']['ancestors'] if 'cpu.max' in v['fields'])['fields'].pop('cpu.max'), 'host controller interface coverage')
    check('thread-quota-one', native_quota, 'host thread affinity or quota')
    check('thread-pinned', lambda r: r['threads'][0].update(affinity=[r['affinity'][0]]), 'host thread affinity or quota')
    check('extra-qemu', lambda r: r['qemu_processes'].append(copy.deepcopy(r['qemu_processes'][0])), 'host concurrent or missing VM')
    check('wrong-observed-pid', lambda r: r.update(observed_pid=r['observed_pid']+1), 'host observed VM identity')
    check('missing-vcpu', lambda r: r['qmp_identity']['cpus'].pop(), 'host raw QMP identity differs')
    check('wrong-native-thread-rebound-qmp', qmp_substitute, 'host vCPU to native thread identity')
    check('wrong-uuid', lambda r: r['qmp_identity'].update(uuid='synthetic-wrong-uuid'), 'host actual QMP UUID')
    check('duplicate-topology', lambda r: r['topology'].append(copy.deepcopy(r['topology'][0])), 'host topology coverage')
    check('placement-modified', lambda r: r.update(placement_changed=True), 'host profile or changed placement')
    check('memory-too-small', lambda r: next(v for v in r['cgroup']['ancestors'] if 'memory.max' in v['fields'])['fields'].update({'memory.max': '1048576'}), 'host memory limit below guest reservation')
    return {'schema': 1, 'status': 'PASS', 'authority': 'ACTUAL_CAPACITY_COMPARATOR_WITH_SYNTHETIC_MUTATIONS',
            'snapshot_sha256': hashlib.sha256(snapshot.read_bytes()).hexdigest(),
            'launch_sha256': hashlib.sha256(launch.read_bytes()).hexdigest(),
            'positive': positive, 'negative_cases': cases, 'restored_positives': len(cases),
            'guest_timing_acceptance': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('launch', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.snapshot, args.launch)
    with args.output.open('x') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
    print(json.dumps({'status': result['status'], 'negative_cases': len(result['negative_cases'])}))
