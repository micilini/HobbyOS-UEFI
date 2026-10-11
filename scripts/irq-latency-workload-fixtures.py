#!/usr/bin/env python3
"""Exercise the workload comparator on actual captures and isolated mutations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from irq_latency_evidence import records, require
from irq_latency_workload import verify


def run(serial, cpus):
    base = records(serial)
    expected = verify(base, cpus)
    cases = []

    def negative(name, mutate, boundary):
        changed = copy.deepcopy(base)
        mutate(changed)
        try:
            verify(changed, cpus)
        except ValueError as error:
            require(boundary in str(error), 'wrong workload rejection: ' + name + ': ' + str(error))
            cases.append(dict(name=name, synthetic=True, expected='REJECT', reason=str(error)))
        else:
            raise ValueError('workload mutation accepted: ' + name)
        require(verify(base, cpus) == expected, 'workload restored positive changed')

    def field(kind, key, value, at=0):
        return lambda r: r[kind][at].__setitem__(key, str(value))

    negative('missing-worker', lambda r: r['WORKER'].pop(), 'worker lifecycle/resource coverage')
    negative('duplicate-worker', lambda r: r['WORKER'].append(copy.deepcopy(r['WORKER'][0])), 'duplicate identity')
    negative('short-workload', field('WORKER', 'operations', 1023), 'worker completion or retirement')
    negative('unreaped-worker', field('WORKER', 'retired', 0), 'worker completion or retirement')
    negative('failed-worker-hidden-by-terminal', field('WORKER', 'failed', 1), 'worker completion or retirement')
    negative('reused-task-identity', lambda r: r['WORKER'][1].update(
        {key: r['WORKER'][0][key] for key in ('task', 'lifecycle')}), 'worker task identity')
    negative('heap-leak', field('WORKER_RESOURCES', 'heap_released', 1023), 'resource imbalance: heap')
    negative('dma-leak', field('WORKER_RESOURCES', 'dma_released', 0), 'resource imbalance: dma')
    negative('live-view', field('WORKER_RESOURCES', 'views_released', 15), 'view lifetime count')
    negative('mapping-timeout', field('WORKER_MAPPING', 'release_status', 4), 'mapping admission result')
    negative('unbounded-admission', field('WORKER_MAPPING', 'acquire_busy', 16385), 'bounded admission count')
    negative('missing-cpu-row', lambda r: r['PARTICIPATION'].pop(), 'local payload CPU inventory')
    negative('forged-payload-count', field('PARTICIPATION', 'local_payload_accesses', 1041), 'payload access count')
    negative('missing-api-kind', lambda r: r['API_ELAPSED'].pop(), 'API elapsed inventory')
    negative('api-duration-forged', field('API_ELAPSED', 'maximum_ns', 0), 'API endpoint/duration identity')
    negative('api-operation-outside-workload', field('API_ELAPSED', 'operation', 1024), 'API operation count/index')
    negative('api-mislabeled-irq-time', field('API_ELAPSED', 'includes_enabled_time', 0), 'API elapsed authority')
    negative('growth-not-observed', lambda r: r['GROWTH'][0].update(heap_after=r['GROWTH'][0]['heap_before']),
             'actual heap growth/headroom')
    negative('refusal-modified-output', field('REFUSALS', 'pmm_output', 0), 'allocation refusal changed result/output')
    negative('fragmentation-falsely-allocated', field('FRAGMENTATION', 'allocated', 1), 'fragmented constrained allocation result')
    negative('short-mapping-series', field('MAPPING', 'completed', 191), 'coordinator mapping lifetime completion')
    negative('backing-not-released', field('MAPPING', 'backing_released', 0), 'coordinator mapping lifetime completion')
    negative('terminal-inventory-forged', field('STRESS', 'created', 1), 'workload terminal inventory')
    negative('failed-terminal', field('STRESS', 'status', 'FAIL'), 'workload terminal inventory')
    return dict(schema=1, status='PASS', authority='ACTUAL_WORKLOAD_COMPARATOR_WITH_CAPTURED_RECORDS',
                base_sha256=hashlib.sha256(serial.encode()).hexdigest(), negative=len(cases),
                restored_positive=len(cases), cases=cases, timing_acceptance=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', type=Path)
    parser.add_argument('--cpus', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.serial.read_text(), args.cpus)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'cases'}))
