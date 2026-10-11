"""Check allocator workload records; this does not grant timing acceptance."""
import argparse
import json
from pathlib import Path

from irq_latency_evidence import records, index, number, require, U64

STACK_COVERAGE = 'targeted-stack-lifecycle-v1'


def stack_coverage(raw, cpus, contract):
    require(contract in (None, STACK_COVERAGE), 'unknown stack coverage contract')
    declarations = raw.get('STACK_COVERAGE_METHOD', [])
    require(len(declarations) == int(contract == STACK_COVERAGE), 'stack coverage source/plan mismatch')
    if declarations:
        require(number(declarations[0], 'version') == 1 and
                number(declarations[0], 'cpus') == cpus and number(declarations[0], 'epoch') == 1,
                'stack coverage source method identity')
    if contract is None:
        require(not raw.get('STACK_COVERAGE') and not raw.get('STACK_COVERAGE_FAILURE'),
                'current stack coverage relabeled historical')
        return None
    require(not raw.get('STACK_COVERAGE_FAILURE'), 'incomplete targeted stack lifecycle')
    rows = index(raw.get('STACK_COVERAGE', []), ('cpu',))
    require(set(rows) == {(c,) for c in range(cpus)}, 'targeted stack CPU coverage')
    maxima = index([r for r in raw.get('MAXIMUM', []) if number(r, 'path') == 7], ('cpu',))
    require(set(maxima) == {(c,) for c in range(cpus)} and
            all(number(r, 'count') > 0 and number(r, 'invalid') == 0 for r in maxima.values()),
            'targeted stack has no actual enclosing observation')
    identities, stacks = set(), set()
    for (cpu,), row in rows.items():
        require(number(row, 'version') == 1 and row['status'] == 'PASS' and
                all(number(row, k) == cpu for k in ('before', 'after', 'child_cpu')) and
                number(row, 'worker_retired') == number(row, 'child_retired') == 1,
                'targeted stack placement/lifetime')
        for role in ('worker', 'child'):
            owner = (number(row, role), number(row, role+'_life'))
            stack = (number(row, role+'_slot'), number(row, role+'_generation'))
            require(all(owner) and owner not in identities and 128 <= stack[0] < 1664 and
                    stack[1] > 0 and stack not in stacks, 'targeted stack owner reuse')
            identities.add(owner); stacks.add(stack)
    return dict(contract=contract, workers=cpus, children=cpus, retired=2*cpus)


def stack_coverage_lifetimes(serial, raw, cpus, contract, services, work_admitted):
    result = stack_coverage(raw, cpus, contract)
    if result is None:
        return None
    import re, zlib
    expected = {}
    for row in raw['STACK_COVERAGE']:
        for role in ('worker', 'child'):
            key = number(row, role), number(row, role+'_life')
            expected[key] = number(row, role+'_slot'), number(row, role+'_generation')
    retired = set()
    cutoff = serial.index('[IRQ_LATENCY][WORK_CUTOFF] ')
    for match in re.finditer(r'^\[STACK_GUARD\]\[RETIRE\] .*$', serial, re.M):
        body, sep, crc = match[0].rstrip('\r').rpartition(' crc=')
        require(sep and re.fullmatch('[0-9a-f]{8}', crc) and
                zlib.crc32(body.encode()) == int(crc, 16), 'targeted retirement CRC')
        row = dict(field.split('=', 1) for field in body.split()[1:])
        key = number(row, 'task'), number(row, 'lifecycle')
        if key not in expected:
            continue
        request = number(row, 'request')
        require(key not in retired and match.start() < cutoff and
                (number(row, 'slot'), number(row, 'generation')) == expected[key] and
                row['status'] == 'COMPLETE' and
                int(row['requested'], 16) == int(row['acknowledged'], 16) == (1 << cpus)-1 and
                0 < request <= work_admitted and request in services and
                number(services[request], 'operation') == 1 and number(services[request], 'status') == 0,
                'targeted retirement missing service or wrong lifetime/cutoff')
        retired.add(key)
    require(retired == set(expected), 'targeted retirement missing raw proof')
    positions = [m.start() for m in re.finditer(r'^\[IRQ_LATENCY\]\[STACK_COVERAGE\] ', serial, re.M)]
    require(len(positions) == cpus and all(p < cutoff for p in positions), 'targeted stack after measured cutoff')
    return result


def singleton(raw, kind):
    require(len(raw[kind]) == 1, 'workload singleton: ' + kind)
    return raw[kind][0]


def verify(raw, cpus, *, stack_contract=None):
    require(1 <= cpus <= 32, 'workload CPU count')
    coverage = stack_coverage(raw, cpus, stack_contract)
    workers = 2 * cpus
    expected = {(i,) for i in range(workers)}
    lifecycle = index(raw['WORKER'], ('index',))
    resources = index(raw['WORKER_RESOURCES'], ('worker',))
    mapping = index(raw['WORKER_MAPPING'], ('worker',))
    participation = index(raw['PARTICIPATION'], ('worker', 'cpu'))
    elapsed = index(raw['API_ELAPSED'], ('worker', 'kind'))
    require(set(lifecycle) == set(resources) == set(mapping) == expected,
            'worker lifecycle/resource coverage')
    require(set(participation) == {(w, cpu) for w in range(workers) for cpu in range(cpus)},
            'local payload CPU inventory')
    require(set(elapsed) == {(w, kind) for w in range(workers) for kind in range(3)},
            'API elapsed inventory')
    identities, touches, busy, api_maxima = set(), [0] * cpus, 0, []
    for w in range(workers):
        row = lifecycle[(w,)]
        identity = number(row, 'task'), number(row, 'lifecycle')
        require(all(identity) and identity not in identities, 'worker task identity')
        identities.add(identity)
        start, end = number(row, 'start_ns'), number(row, 'end_ns')
        require(start < end and number(row, 'operations') == 1024 and
                number(row, 'failed') == 0 and number(row, 'retired') == 1,
                'worker completion or retirement')
        require(number(row, 'overlap') <= 1024, 'worker overlap count')
        resource = resources[(w,)]
        require(resource['status'] == 'PASS', 'worker resource status')
        for prefix, amount in [('heap', 1024), ('frames', 1024), ('dma', 16 * 65536)]:
            require(number(resource, prefix + '_allocated') ==
                    number(resource, prefix + '_released') == amount,
                    'worker owned resource imbalance: ' + prefix)
        require(number(resource, 'views_acquired') == number(resource, 'views_released') == 16,
                'worker view lifetime count')
        admission = mapping[(w,)]
        require(number(admission, 'acquire_status') == number(admission, 'release_status') == 0,
                'worker mapping admission result')
        for key in ('acquire_busy', 'release_busy'):
            count = number(admission, key)
            require(count <= 16 * 1024, 'worker bounded admission count')
            busy += count
        local = [number(participation[(w, cpu)], 'local_payload_accesses') for cpu in range(cpus)]
        require(sum(local) == 1040, 'worker actual payload access count')
        touches = [a + b for a, b in zip(touches, local)]
        for kind in range(3):
            sample = elapsed[(w, kind)]
            begin, finish = number(sample, 'begin_ns'), number(sample, 'end_ns')
            operation = number(sample, 'operation')
            require(start <= begin < finish <= end and finish - begin == number(sample, 'maximum_ns'),
                    'API endpoint/duration identity')
            require(number(sample, 'count') == (16 if kind == 2 else 1024) and operation < 1024 and
                    (kind != 2 or operation % 64 == 0), 'API operation count/index')
            require(sample['authority'] == 'MONOTONIC_CLOCK' and
                    number(sample, 'includes_enabled_time') == 1, 'API elapsed authority')
            api_maxima.append(dict(worker=w, kind=kind, maximum_ns=finish-begin))
    require(all(touches), 'worker actual CPU participation missing')
    growth = singleton(raw, 'GROWTH')
    require(growth['status'] == 'PASS' and number(growth, 'request') == 34603008 and
            number(growth, 'headroom') == 134217728 and
            number(growth, 'eligible_frames') * 4096 >= 134217728 and
            number(growth, 'heap_after') > number(growth, 'heap_before'), 'actual heap growth/headroom')
    refusal = singleton(raw, 'REFUSALS')
    require(refusal['status'] == 'PASS' and all(number(refusal, key) == 1 for key in
            ('heap', 'dma32', 'dma_status', 'pmm')) and number(refusal, 'pmm_output') == U64,
            'allocation refusal changed result/output')
    fragmented = singleton(raw, 'FRAGMENTATION')
    require(fragmented['status'] == 'PASS' and number(fragmented, 'first') % 4096 == 0 and
            all(number(fragmented, key) == value for key, value in
                [('frames', 32), ('isolated_free', 16), ('request', 2), ('allocated', 0), ('output', U64)]),
            'fragmented constrained allocation result')
    mapped = singleton(raw, 'MAPPING')
    require(mapped['status'] == 'PASS' and number(mapped, 'completed') == number(mapped, 'required') == 192 and
            number(mapped, 'backing_released') == 1 and
            number(mapped, 'acquire_status') == number(mapped, 'release_status') == 0,
            'coordinator mapping lifetime completion')
    terminal = singleton(raw, 'STRESS')
    require(terminal['status'] == 'PASS' and number(terminal, 'created') == number(terminal, 'retired') == workers and
            int(terminal['participating'], 16) == int(terminal['required'], 16) == (1 << cpus)-1,
            'workload terminal inventory')
    return {'schema': 1, 'status': 'PASS', 'authority': 'ALLOCATOR_WORKLOAD_RECORD_CONSISTENCY',
            'timing_acceptance': False, 'workers': workers, 'heap_operations': workers*1024,
            'pmm_operations': workers*1024, 'dma_lifetimes': workers*16,
            'owned_view_lifetimes': 192+workers*16, 'payload_accesses_by_cpu': touches,
            'mapping_busy_returns': busy, 'api_elapsed_maxima': api_maxima,
            **({'stack_coverage': coverage} if coverage else {}),
            'limits': ['Ticket contention and TLB audit require separate causal checks.',
                       'Candidate, command transport and host authority are checked by the campaign gate.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', type=Path)
    parser.add_argument('--cpus', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = verify(records(args.serial.read_text()), args.cpus)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'api_elapsed_maxima'}))
