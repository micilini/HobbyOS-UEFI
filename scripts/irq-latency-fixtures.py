#!/usr/bin/env python3
"""Adversarial raw-record replay, including rejected real observations."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib
import irq_latency_evidence as V


def change_line(serial, kind, predicate, transform):
    lines = serial.splitlines()
    for i, line in enumerate(lines):
        if not line.startswith('[IRQ_LATENCY][' + kind + '] '):
            continue
        body = line.rsplit(' crc=', 1)[0]
        marker, fields = body.split(' ', 1)
        row = dict(field.split('=', 1) for field in fields.split())
        if not predicate(row):
            continue
        replacement = transform(row)
        if replacement is None:
            del lines[i]
        else:
            body = marker + ' ' + ' '.join(key + '=' + str(value) for key, value in replacement.items())
            lines[i] = body + ' crc=' + format(zlib.crc32(body.encode()), '08x')
        return '\n'.join(lines) + '\n'
    raise ValueError('fixture target missing: ' + kind)


def run(path):
    original = path.read_text()
    base = V.inspect(original)
    raw = V.records(original)
    first = next(row for row in raw['MAXIMUM'] if int(row['path']) < 11 and int(row['maximum_cycles']))
    cpu, at = first['cpu'], first['path']
    maximum = lambda row: row['kind'] == 'maximum' and row['cpu'] == cpu and row['index'] == at

    def cell(field, value):
        def transform(row):
            cells = row['cells'].split(',')
            cells[field-int(row['first'])] = str(value(cells[field-int(row['first'])]))
            return dict(row, cells=','.join(cells))
        return transform

    cases = [
        ('changed-budget', 'BEGIN', lambda row: True, lambda row: dict(row, budget_ns='50001'), 'observer activation'),
        ('missing-terminal-cpu', 'CPU', lambda row: row['cpu'] == cpu, lambda row: None, 'terminal CPU coverage'),
        ('missing-calibration-cpu', 'TIMEBASE', lambda row: row['cpu'] == cpu, lambda row: None, 'clock CPU/role coverage'),
        ('missing-window', 'WINDOW', lambda row: True, lambda row: None, 'window coverage'),
        ('missing-reference-endpoint', 'REFERENCE', lambda row: True, lambda row: None, 'reference coverage'),
        ('missing-probe', 'OVERHEAD', lambda row: True, lambda row: None, 'probe coverage'),
        ('changed-frequency', 'TIMEBASE', lambda row: True,
         lambda row: dict(row, conversion_hz=str(int(row['conversion_hz'])+1)), 'reported clock/observer facts differ'),
        ('changed-observer-cost', 'TIMEBASE', lambda row: True,
         lambda row: dict(row, overhead_max=str(int(row['overhead_max'])+1)), 'reported clock/observer facts differ'),
        ('discard-eligible-probe', 'OVERHEAD', lambda row: row['valid'] == '1',
         lambda row: dict(row, valid='0'), 'empty IRQ probe eligibility changed'),
        ('reset-epoch', 'CPU', lambda row: True, lambda row: dict(row, epoch='2'), 'epoch or interrupted snapshot'),
        ('interrupted-terminal', 'CPU', lambda row: True,
         lambda row: dict(row, sequence=str(int(row['sequence']) | 1)), 'epoch or interrupted snapshot'),
        ('missing-maximum-witness', 'WITNESS', maximum, lambda row: None, 'missing raw maximum/failure witness'),
        ('truncated-end', 'WITNESS', lambda row: maximum(row) and row['first'] == '0',
         cell(1, lambda old: int(old)-1), 'maximum differs from raw witness'),
        ('wrong-end-sequence', 'WITNESS', lambda row: maximum(row) and row['first'] == '0',
         cell(3, lambda old: int(old)+1), 'maximum endpoint/CPU/sequence identity'),
        ('nested-path-omitted', 'WITNESS', lambda row: maximum(row) and row['first'] == '0',
         cell(4, lambda old: int(old) & ~(1 << int(at))), 'maximum path/context attribution'),
        ('wrong-end-cpu', 'WITNESS', lambda row: maximum(row) and row['first'] == '8',
         cell(9, lambda old: (int(old)+1) % base['cpus']), 'maximum endpoint/CPU/sequence identity'),
        ('forged-shorter-nanoseconds', 'MAXIMUM', lambda row: row['cpu'] == cpu and row['path'] == at,
         lambda row: dict(row, maximum_ns=str(int(row['maximum_ns'])-1)), 'maximum conversion differs'),
        ('forged-maximum-status', 'MAXIMUM', lambda row: row['cpu'] == cpu and row['path'] == at,
         lambda row: dict(row, status='PASS'), 'maximum status differs'),
        ('missing-failure-witness', 'WITNESS', lambda row: row['kind'] == 'failure', lambda row: None,
         'missing raw maximum/failure witness'),
    ]
    if raw['CONTENTION']:
        cases += [
            ('missing-ticket-cpu-path', 'CONTENTION', lambda row: True, lambda row: None, 'ticket contention CPU/path coverage'),
            ('fake-zero-ticket-distance', 'CONTENTION', lambda row: int(row['queued']) > 0,
             lambda row: dict(row, maximum_distance='0'), 'ticket contention count/distance'),
            ('ticket-lock-identity-missing', 'CONTENTION', lambda row: int(row['queued']) > 0,
             lambda row: dict(row, lock='0'), 'ticket contention causal witness'),
            ('lost-violation-count', 'MAXIMUM', lambda row: int(row['path']) < 11 and int(row['violations']) > 0,
             lambda row: dict(row, violations='0'), 'maximum/violation counter contradiction'),
        ]
    if raw['CALLBACK']:
        cases += [
            ('missing-callback-cpu-operation', 'CALLBACK', lambda row: True, lambda row: None, 'component CPU/operation coverage'),
            ('truncated-callback-end', 'CALLBACK', lambda row: int(row['count']) > 0,
             lambda row: dict(row, end=str(int(row['end'])-1)), 'component maximum endpoint identity'),
            ('changed-callback-frequency', 'CALLBACK', lambda row: True,
             lambda row: dict(row, frequency_hz=str(int(row['frequency_hz'])+1)), 'callback conversion or terminal cutoff'),
            ('request-as-masked-context', 'REQUEST_ELAPSED', lambda row: int(row['count']) > 0,
             lambda row: dict(row, context='2'), 'component execution context'),
            ('request-elapsed-shortened', 'REQUEST_ELAPSED', lambda row: int(row['count']) > 0,
             lambda row: dict(row, maximum_ns='1'), 'component maximum endpoint identity'),
            ('request-stale-generation', 'REQUEST_ELAPSED', lambda row: int(row['count']) > 0,
             lambda row: dict(row, generation='0'), 'component maximum endpoint identity'),
        ]
    results = []
    for name, kind, predicate, transform, boundary in cases:
        synthetic = change_line(original, kind, predicate, transform)
        try:
            V.inspect(synthetic)
        except ValueError as error:
            V.require(boundary in str(error), 'fixture wrong rejection: ' + name + ': ' + str(error))
            results.append({'name': name, 'synthetic': True, 'expected_boundary': boundary,
                            'actual_reason': str(error), 'sha256': hashlib.sha256(synthetic.encode()).hexdigest()})
        else:
            raise ValueError('mutation accepted: ' + name)
        V.require(V.inspect(original) == base, 'restored real raw replay changed')
    return {'schema': 1, 'status': 'PASS', 'authority': 'SYNTHETIC_MUTATIONS_OF_ACTUAL_RAW_RECORDS',
            'base_sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'negative_cases': results,
            'restored_positive_replays': len(results), 'base_observer_post_failures': base['observer_post_failures'],
            'runtime_acceptance_claimed': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.serial)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'status': result['status'], 'negative_cases': len(result['negative_cases']),
                      'restored_positive_replays': result['restored_positive_replays']}))
