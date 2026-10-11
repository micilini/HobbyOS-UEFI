"""Decode raw latency evidence; derive clock and maximum facts independently.

This module deliberately grants no campaign acceptance. Host authority, exact
candidate roles, workload completion and inherited native evidence are separate
mandatory inputs to the final gate.
"""
import collections
import json
import re
import zlib
from pathlib import Path

U64 = (1 << 64) - 1
BUDGET_NS = 50000
PATHS = ('heap', 'pmm', 'pmm_integrity', 'pmm_reachability', 'dma', 'mapping',
         'unmap', 'stack', 'shootdown', 'cache_writeback', 'icr')


def require(condition, reason):
    if not condition:
        raise ValueError('irq latency ' + reason)


def number(row, key):
    text = row[key]
    require(bool(re.fullmatch(r'0|[1-9][0-9]*', text)), 'noncanonical integer: ' + key)
    value = int(text)
    require(value <= U64, 'integer overflow: ' + key)
    return value


def ceildiv(value, divisor):
    require(divisor > 0, 'zero conversion divisor')
    return (value + divisor - 1) // divisor


def records(serial):
    result = collections.defaultdict(list)
    for line in serial.splitlines():
        if not line.startswith('[IRQ_LATENCY]'):
            continue
        body, separator, crc = line.rpartition(' crc=')
        require(separator and re.fullmatch('[0-9a-f]{8}', crc), 'missing record CRC')
        require(zlib.crc32(body.encode()) == int(crc, 16), 'record CRC mismatch')
        marker, _, fields = body.partition(' ')
        match = re.fullmatch(r'\[IRQ_LATENCY\]\[([A-Z_]+)\]', marker)
        require(match, 'record kind')
        pairs = [field.split('=', 1) for field in fields.split()]
        require(all(len(pair) == 2 for pair in pairs), 'record field syntax')
        row = dict(pairs)
        require(len(row) == len(pairs), 'duplicate record field')
        result[match[1]].append(row)
    require(result, 'no raw records')
    return result


def index(rows, keys):
    result = {}
    for row in rows:
        key = tuple(number(row, name) for name in keys)
        require(key not in result, 'duplicate identity: ' + str(key))
        result[key] = row
    return result


def interval(a, b):
    """Exact integer inequalities from raw endpoints; no guest Hz is trusted."""
    for point in (a, b):
        require(all(number(point, key) == 1 for key in ('valid', 'initialized', 'if_enabled')),
                'ineligible reference endpoint')
        require(number(point, 'cpu') == number(point, 'end_cpu') and
                number(point, 'switch_before') == number(point, 'switch_after'), 'reference migration')
        require((number(point, 'mode'), number(point, 'bits')) in ((1, 64), (2, 32)), 'HPET read mode')
        require(number(point, 'tsc_after') >= number(point, 'tsc_before'), 'reference TSC order')
    require(a['cpu'] == b['cpu'] and a['mode'] == b['mode'] and a['period_fs'] == b['period_fs'],
            'reference window identity')
    period = number(a, 'period_fs')
    ticks = number(b, 'hpet') - number(a, 'hpet')
    low = number(b, 'tsc_before') - number(a, 'tsc_after')
    high = number(b, 'tsc_after') - number(a, 'tsc_before')
    require(0 < period <= 100000000 and 2 < ticks <= U64 // period - 2 and low > 0,
            'reference width or order')
    duration = ticks * period // 1000000
    require(10000000 <= duration <= 100000000, 'reference duration')
    lower = low * 10**15 // ((ticks + 2) * period)
    upper = ceildiv(high * 10**15, (ticks - 2) * period)
    require(0 < lower <= upper <= U64 and ceildiv((upper-lower)*1000000, lower) <= 10000,
            'reference dispersion')
    estimate = (low + (high-low)//2) * 10**15 // (ticks * period)
    return lower, upper, estimate


def clocks(raw, cpus):
    import irq_latency_pairs as P
    return P.clocks(raw, cpus) if P.method(raw) else legacy_clocks(raw, cpus)


def calibration_diagnostic(raw, role=0):
    import irq_latency_pairs as P
    if not P.method(raw):
        return legacy_calibration_diagnostic(raw, role)
    cpus = number(raw['BEGIN'][0], 'cpus')
    before = P.role_result(raw,0,cpus)[0] if role else None
    return P.role_result(raw,role,cpus,before)[1]


def legacy_clocks(raw, cpus):
    windows = index(raw['WINDOW'], ('role', 'index'))
    references = {}
    for row in raw['REFERENCE']:
        key = number(row, 'role'), number(row, 'index'), row['endpoint']
        require(row['endpoint'] in ('begin', 'end') and key not in references, 'reference endpoint identity')
        references[key] = row
    caps = index(raw['CAPABILITY'], ('role', 'cpu'))
    reported = index(raw['TIMEBASE'], ('role', 'cpu'))
    expected = {(role, cpu) for role in range(2) for cpu in range(cpus)}
    require(set(caps) == set(reported) == expected, 'clock CPU/role coverage')
    require(set(windows) == {(role, i) for role in range(2) for i in range(64*cpus)}, 'window coverage')
    require(set(references) == {(role, i, endpoint) for role, i in windows for endpoint in ('begin', 'end')},
            'reference coverage')
    eligible = collections.defaultdict(list)
    for key, row in windows.items():
        require(number(row, 'complete') == 1, 'unfinished calibration worker')
        a, b = references[(*key, 'begin')], references[(*key, 'end')]
        try:
            bounds = interval(a, b)
        except ValueError:
            require(number(row, 'valid') == 0, 'invalid reference accepted')
            continue
        require(number(row, 'valid') == 1, 'valid reference silently discarded')
        eligible[(key[0], number(a, 'cpu'))].append((key[1], a, b, bounds))
    probes = index(raw['OVERHEAD'], ('role', 'index', 'probe'))
    require(set(probes) == {(role, i, probe) for role, i in windows for probe in range(4)}, 'probe coverage')
    costs = collections.defaultdict(list)
    for (role, _, _), row in probes.items():
        start, inner, end, outer = (number(row, name) for name in
                                   ('outer_begin', 'inner_begin', 'inner_end', 'outer_end'))
        eligible_probe = (start <= inner < end <= outer and number(row, 'cpu') < cpus and row['cpu'] == row['end_cpu'] and
                row['switch_before'] == row['switch_after'] and
                number(row, 'sequence_after') == number(row, 'sequence_before')+1 and
                number(row, 'irq_after') == number(row, 'irq_before')+1)
        require(number(row, 'valid') == int(eligible_probe), 'empty IRQ probe eligibility changed')
        if not eligible_probe:
            continue
        costs[(role, number(row, 'cpu'))].append((outer-start)-(end-inner))
    result = {}
    for role, cpu in sorted(expected):
        capability = caps[(role, cpu)]
        require(number(capability, 'seen') == 1 and int(capability['basic'], 16) >= 1 and
                int(capability['leaf1_edx'], 16) & 16, 'TSC capability unavailable')
        candidates = sorted(eligible[(role, cpu)], key=lambda value: number(value[1], 'tsc_before'))
        selected = []
        last = 0
        for value in candidates:
            if number(value[1], 'tsc_before') > last and len(selected) < 3:
                selected.append(value)
                last = number(value[2], 'tsc_after')
        require(len(selected) == 3, 'disjoint calibration windows missing')
        lower = max(v[3][0] for v in selected)
        upper = min(v[3][1] for v in selected)
        require(lower <= upper, 'clock intervals inconsistent')
        declared = 0
        if int(capability['basic'], 16) >= 0x15 and all(number(capability, key) for key in
                                                       ('denominator', 'numerator', 'crystal_hz')):
            declared = number(capability, 'crystal_hz') * number(capability, 'numerator') // number(capability, 'denominator')
            require(lower <= declared <= upper, 'CPUID-15 frequency disagreement')
        estimate = lower + (upper-lower)//2
        conversion = lower * 990000 // 1000000
        for _, _, _, (lo, hi, middle) in candidates:
            require(lo >= conversion and (not declared or lo <= declared <= hi) and
                    ceildiv(abs(middle-estimate)*1000000, estimate) <= 10000, 'unselected clock window disagreement')
        require(len(costs[(role, cpu)]) >= 32, 'IRQ probe CPU coverage')
        overhead = max(costs[(role, cpu)])
        require(0 < overhead <= U64//2, 'observer cost range')
        observer_errors = 0
        if role:
            before = result[(0, cpu)]
            require(declared == before['declared_hz'] and lower >= before['conversion_hz'] and
                    ceildiv(abs(estimate-before['estimate_hz'])*1000000, before['estimate_hz']) <= 10000,
                    'post clock drift')
            observer_errors = 4 if overhead > 2*before['overhead_max'] else 0
        derived = dict(lower_hz=lower, upper_hz=upper, estimate_hz=estimate,
                       conversion_hz=conversion, declared_hz=declared, overhead_max=overhead,
                       windows=len(candidates), probes=len(costs[(role, cpu)]), clock_errors=0,
                       observer_errors=observer_errors,
                       **{'selected'+str(i): value[0] for i, value in enumerate(selected)})
        require(all(number(reported[(role, cpu)], key) == value for key, value in derived.items()),
                'reported clock/observer facts differ from raw inputs')
        result[(role, cpu)] = derived
    for role in range(2):
        valid = not any(result[(role, cpu)]['observer_errors'] for cpu in range(cpus))
        require(all(number(reported[(role, cpu)], 'series_valid') == int(valid) for cpu in range(cpus)),
                'calibration aggregate status differs')
    return result


def legacy_calibration_diagnostic(raw, role=0):
    """Explain failed admission from complete raw attempts, without qualification."""
    require(len(raw['BEGIN']) == 1, 'diagnostic activation inventory')
    cpus = number(raw['BEGIN'][0], 'cpus')
    windows = index([r for r in raw['WINDOW'] if number(r,'role')==role], ('index',))
    require(set(windows) == {(i,) for i in range(64*cpus)}, 'diagnostic window coverage')
    refs = {(number(r,'index'),r['endpoint']):r for r in raw['REFERENCE'] if number(r,'role')==role}
    require(len(refs)==2*len(windows) and len([r for r in raw['REFERENCE'] if number(r,'role')==role])==len(refs),
            'diagnostic reference coverage')
    accepted=collections.defaultdict(list);rejected=[]
    for (i,), row in windows.items():
        require(number(row,'complete')==1,'diagnostic unfinished window')
        a,b=refs[(i,'begin')],refs[(i,'end')]
        try:bounds=interval(a,b)
        except ValueError as error:
            require(number(row,'valid')==0,'diagnostic invalid window accepted')
            rejected.append({'index':i,'begin_cpu':number(a,'cpu'),'end_cpu':number(b,'cpu'),
                             'reason':str(error)})
        else:
            require(number(row,'valid')==1,'diagnostic eligible window discarded')
            accepted[number(a,'cpu')].append((i,a,b,bounds))
    probes=index([r for r in raw['OVERHEAD'] if number(r,'role')==role],('index','probe'))
    require(set(probes)=={(i,j) for i in range(64*cpus) for j in range(4)},'diagnostic probe coverage')
    components=[];costs=collections.defaultdict(list)
    for (i,j),row in probes.items():
        a,b,c,d=(number(row,k) for k in ('outer_begin','inner_begin','inner_end','outer_end'))
        eligible=(a<=b<c<=d and number(row,'cpu')<cpus and row['cpu']==row['end_cpu'] and
            row['switch_before']==row['switch_after'] and number(row,'sequence_after')==number(row,'sequence_before')+1 and
            number(row,'irq_after')==number(row,'irq_before')+1)
        require(number(row,'valid')==int(eligible),'diagnostic probe eligibility differs')
        result={'index':i,'probe':j,'cpu':number(row,'cpu'),'eligible':eligible,
                'outer_begin':a,'inner_begin':b,'inner_end':c,'outer_end':d,
                'leading':b-a,'interior':c-b,'trailing':d-c,'envelope':(b-a)+(d-c)}
        components.append(result)
        if eligible:costs[result['cpu']].append(result)
    reported=index([r for r in raw['TIMEBASE'] if number(r,'role')==role],('cpu',))
    require(set(reported)=={(c,) for c in range(cpus)},'diagnostic CPU coverage')
    per_cpu=[]
    for cpu in range(cpus):
        candidates=sorted(accepted[cpu],key=lambda v:number(v[1],'tsc_before'))
        selected=[];last=0
        for i,a,b,bounds in candidates:
            if number(a,'tsc_before')>last and len(selected)<3:
                selected.append(i);last=number(b,'tsc_after')
        worst=max(costs[cpu],key=lambda r:r['envelope']) if costs[cpu] else None
        row=reported[(cpu,)]
        require(number(row,'windows')==len(candidates) and number(row,'probes')==len(costs[cpu]) and
                number(row,'overhead_max')==(worst['envelope'] if worst else 0), 'diagnostic reported coverage/envelope')
        require(all(number(row,'selected'+str(i))==(selected[i] if i<len(selected) else 0) for i in range(3)),
                'diagnostic selected disjoint windows')
        require(len(selected)==3 or number(row,'clock_errors') & 2,'diagnostic missing clock rejection')
        per_cpu.append({'cpu':cpu,'eligible_windows':len(candidates),'selected':selected,
            'eligible_probes':len(costs[cpu]),'worst_probe':worst,
            'clock_errors_reported':number(row,'clock_errors'),'qualification':'NOT_ESTABLISHED'})
    return {'schema':1,'status':'DIAGNOSTIC_RECOUNT_COMPLETE','authority':'RAW_PRECALIBRATION_ONLY',
        'begin_status':raw['BEGIN'][0]['status'],'cpus':per_cpu,'rejected_windows':rejected,
        'probes':components,'stress_executed':bool(raw['STRESS']),'timing_acceptance':False}


def witness_envelope(witness, cpu, clock):
    """Bind historical primitive arithmetic without promoting its missing history.

    The current source selects the allowance at hard-IRQ close or IRET tail;
    it never selects it from the recorded adjustment. Independent event/lifetime
    coverage is additionally required by the successor's current-role gate.
    """
    begin, end, context = witness[19], witness[20], witness[7]
    require(begin in (0, 1, 2) and end in (0, 1, 3, 4), 'primitive endpoint meaning')
    require(witness[10] == 1 and witness[8] == cpu, 'primitive calibration CPU/epoch')
    if begin == 1:
        require(witness[5] <= 255 and context in (2, 4) and
                witness[15] == witness[0] and witness[16] == cpu, 'primitive endpoint meaning')
    else:
        require(witness[5] >= 0xffffffff80000000 and context in (1, 3, 4, 5, 6),
                'primitive endpoint meaning')
        if begin == 0:
            require(witness[15] == witness[0] and witness[16] == cpu, 'primitive endpoint meaning')
        else:
            require(witness[16] == (1 << 32)-1, 'primitive endpoint meaning')
    if end == 1:
        # Activation inside an already entered IRQ can observe its first masked
        # software save, then close at that IRQ's tail. This partial start is
        # retained only in the excluded EARLY bucket, never a runtime path.
        early_tail = (len(witness) == 29 and context == 3 and begin == 0 and
                      witness[2] == witness[3] == 1 and witness[4] == 0 and
                      witness[23] == (1 << 32)-1 and witness[24] == 0 and
                      32 <= witness[6] <= 255)
        require((begin == 1 or early_tail) and witness[6] <= 255 and witness[17] == witness[1] and
                witness[18] == cpu and not witness[22], 'primitive endpoint meaning')
    elif end == 0:
        require(witness[6] >= 0xffffffff80000000 and witness[17] == witness[1] and
                witness[18] == cpu and not witness[22], 'primitive endpoint meaning')
    else:
        require(witness[22] == 1 and witness[1] == witness[21] and
                witness[0] < witness[21], 'primitive captured fallback endpoint')
        # The first IRQ can interrupt after the raw end read but before its
        # publication. Its endpoint then conservatively exceeds that read.
        if end == 4:
            require(len(witness) >= 25 and witness[23] < 1665 and witness[24] > 0 and
                    witness[17] == 0 and witness[18] == (1 << 32)-1,
                    'primitive retired publisher lifetime')
    expected = 2 * clock[(0, cpu)]['overhead_max'] if end == 1 or (context == 2 and (end != 4 or len(witness) == 29)) else 0
    require(witness[14] == expected, 'primitive/calibration allowance binding')
    if len(witness) == 29:
        require(witness[25] == expected, 'frozen close allowance binding')
        if end == 4:
            require(witness[26:29] == [witness[23], witness[24], witness[2]],
                    'retired publisher captured owner/generation/sequence')
        else:
            require(not any(witness[26:29]), 'unexpected retirement evidence')
    return expected


def maxima(raw, cpus, clock, *, primitive='ordered-tsc-irq-envelope'):
    require(primitive in ('ordered-tsc-irq-envelope','intel-local-tsc-irq-envelope-v1'), 'unknown timestamp primitive')
    fields = number(raw['BEGIN'][0], 'witness_cells') if 'witness_cells' in raw['BEGIN'][0] else 23
    require(fields in (23, 25, 29), 'raw witness schema')
    if fields == 29:
        require(raw['BEGIN'][0].get('schema') == '2' and
                raw['BEGIN'][0].get('primitive') == primitive, 'measurement primitive/schema binding')
    if primitive=='intel-local-tsc-irq-envelope-v1':
        require(fields==29,'local primitive requires complete lifetime witness')
        caps=index(raw['CAPABILITY'],('role','cpu'))
        require(set(caps)=={(r,c) for r in (0,1) for c in range(cpus)},'local primitive capability inventory')
        for cap in caps.values():
            require(number(cap,'seen')==1 and int(cap['leaf1_edx'],16)&0x4000010==0x4000010 and
                tuple(int(cap[k],16) for k in ('vendor_b','vendor_d','vendor_c'))==
                (0x756e6547,0x49656e69,0x6c65746e),'local primitive Intel/SSE2/TSC authority')
    state = index(raw['CPU'], ('cpu',))
    rows = index(raw['MAXIMUM'], ('cpu', 'path'))
    require(set(state) == {(cpu,) for cpu in range(cpus)}, 'terminal CPU coverage')
    require(set(rows) == {(cpu, path) for cpu in range(cpus) for path in range(14)}, 'maximum row coverage')
    for (cpu, path), row in rows.items():
        count, invalid, maximum = (number(row, key) for key in ('count', 'invalid', 'maximum_cycles'))
        require(invalid <= count and bool(maximum) == (count > invalid),
                'count/valid-maximum relationship')
        if 'violations' in row:
            require(number(row, 'violations') <= count-invalid and
                    (path < 11 or not number(row, 'violations')), 'violation/sample counter consistency')
        if not maximum:
            require(number(row, 'maximum_ns') == 0 and row['status'] ==
                    ('INVALID_COVERAGE' if path < 11 else 'EXCLUDED_CONTEXT'), 'empty maximum status')
            require('violations' not in row or not number(row, 'violations'), 'empty maximum violations')
    pieces = {}
    for row in raw['WITNESS']:
        key = number(row, 'cpu'), row['kind'], number(row, 'index'), number(row, 'first')
        require(key not in pieces and row['kind'] in ('maximum', 'failure'), 'duplicate witness chunk')
        cells = row['cells'].split(',')
        require(len(cells) == number(row, 'count'), 'witness chunk length')
        pieces[key] = [number({'cell': value}, 'cell') for value in cells]
    expected = set()
    findings = []
    for cpu in range(cpus):
        terminal = state[(cpu,)]
        require(number(terminal, 'epoch') == 1 and number(terminal, 'sequence') % 2 == 0, 'epoch or interrupted snapshot')
        for kind, count in [('maximum', 14), ('failure', number(terminal, 'failures'))]:
            for at in range(count):
                witness = []
                for offset in range(0, fields, 8):
                    key = cpu, kind, at, offset
                    expected.add(key)
                    require(key in pieces and len(pieces[key]) == min(8, fields-offset), 'missing raw maximum/failure witness')
                    witness.extend(pieces[key])
                if kind == 'maximum' and not number(rows[(cpu, at)], 'maximum_cycles'):
                    require(not any(witness), 'empty maximum has nonempty witness')
                    continue
                if witness[11]:
                    require(kind == 'failure', 'invalid witness used as maximum')
                    continue
                require(witness[0] < witness[1] and witness[2] == witness[3] and witness[2] > 0 and
                        witness[8] == witness[9] == cpu and witness[10] == 1, 'maximum endpoint/CPU/sequence identity')
                span = witness[1]-witness[0]+witness[14]
                require(witness[1] <= number(terminal, 'cutoff'), 'maximum after terminal cutoff')
                require(span <= U64 and witness[12] <= span and witness[13] <= span, 'interval/component arithmetic')
                require(witness[19] in (0, 1, 2) and witness[20] in range(5 if fields >= 25 else 4), 'boundary class')
                if witness[20] in (3, 4):
                    require(witness[22] == 1 and witness[1] == witness[21], 'IRQ fallback endpoint')
                if witness[20] == 4:
                    require(witness[23] < 1665 and witness[24] > 0 and witness[17] == 0 and witness[18] == (1 << 32)-1,
                            'retired publisher identity or invented raw endpoint')
                if kind == 'failure':
                    witness_envelope(witness, cpu, clock)
                    require(span > 49999*clock[(0, cpu)]['conversion_hz']//1000000000,
                            'valid failure does not violate fixed budget')
                    for path in range(11):
                        if witness[4] & (1 << path):
                            require(span <= number(rows[(cpu, path)], 'maximum_cycles'), 'largest failure omitted from maximum')
                    continue
                row = rows[(cpu, at)]
                require(span == number(row, 'maximum_cycles'), 'maximum differs from raw witness')
                witness_envelope(witness, cpu, clock)
                if at < 11:
                    require(witness[4] & (1 << at) and witness[7] in (1, 2), 'maximum path/context attribution')
                else:
                    require(witness[7] == (3, 4, 5)[at-11], 'excluded context attribution')
                hz = clock[(0, cpu)]['conversion_hz']
                ns = ceildiv(span*1000000000, hz)
                if 'violations' in row:
                    violations = number(row, 'violations')
                    require(number(row, 'invalid') <= number(row, 'count') and
                            violations <= number(row, 'count')-number(row, 'invalid'),
                            'violation/sample counter consistency')
                    if at < 11:
                        require(bool(violations) == (ns >= BUDGET_NS), 'maximum/violation counter contradiction')
                require(number(row, 'frequency_hz') == hz and number(row, 'maximum_ns') == ns,
                        'maximum conversion differs')
                qualified = not clock[(1, cpu)]['observer_errors']
                require(number(row, 'ns_available') == 1 and number(row, 'measurement_qualified') == int(qualified),
                        'maximum qualification differs')
                status = ('INVALID_COVERAGE' if not number(row, 'count') or number(row, 'invalid') else
                          'UNQUALIFIED_OBSERVATION' if not qualified else
                          'BELOW_BUDGET_OBSERVATION' if ns < BUDGET_NS else 'VIOLATION')
                if at >= 11:
                    status = 'EXCLUDED_CONTEXT'
                require(row['status'] == status, 'maximum status differs')
                if at < 11:
                    findings.append({'cpu': cpu, 'path': PATHS[at], 'cycles': span, 'ns': ns,
                                     'begin_kind': witness[19], 'end_kind': witness[20],
                                     'observer_qualified': not clock[(1, cpu)]['observer_errors'],
                                     'below_budget': ns < BUDGET_NS})
    require(set(pieces) == expected, 'extra witness chunk')
    return findings


def contention(raw, cpus):
    if not raw['CONTENTION']:
        return {'available': False, 'reason': 'historical capture has no actual ticket observations'}
    rows = index(raw['CONTENTION'], ('cpu', 'path'))
    require(set(rows) == {(cpu, path) for cpu in range(cpus) for path in range(11)},
            'ticket contention CPU/path coverage')
    totals = [0] * 11
    witnesses = []
    for (cpu, path), row in sorted(rows.items()):
        observed, queued, distance = (number(row, key) for key in ('observations', 'queued', 'maximum_distance'))
        require(queued <= observed and distance <= 65535 and bool(queued) == bool(distance),
                'ticket contention count/distance')
        if queued:
            require(number(row, 'lock') > 0 and number(row, 'begin_sequence') > 0 and
                    number(row, 'paths') & (1 << path), 'ticket contention causal witness')
            witnesses.append({'cpu': cpu, 'path': PATHS[path], 'distance': distance,
                              'lock': number(row, 'lock'), 'sequence': number(row, 'begin_sequence')})
        else:
            require(not any(number(row, key) for key in ('lock', 'begin_sequence', 'paths')),
                    'empty ticket contention has a witness')
        totals[path] += queued
    return {'available': True, 'queued_by_path': dict(zip(PATHS, totals)), 'witnesses': witnesses}


def components(raw, cpus, clock):
    if not raw['CALLBACK'] and not raw['REQUEST_ELAPSED']:
        return {'available': False, 'reason': 'historical capture has no component witnesses'}
    answer = {'available': True, 'callbacks': [], 'requests': []}
    cutoffs = {number(row, 'cpu'): number(row, 'cutoff') for row in raw['CPU']}
    for kind, key in [('CALLBACK', 'callbacks'), ('REQUEST_ELAPSED', 'requests')]:
        rows = index(raw[kind], ('cpu', 'operation'))
        require(set(rows) == {(cpu, op) for cpu in range(cpus) for op in range(1, 5)},
                'component CPU/operation coverage')
        for (cpu, operation), row in sorted(rows.items()):
            count, invalid, failed = (number(row, n) for n in ('count', 'invalid', 'failures'))
            require(invalid <= count and failed <= count, 'component outcome counts')
            if not count:
                require(not any(number(row, n) for n in
                    ('invalid', 'failures', 'generation', 'root', 'context', 'first_generation', 'last_generation')),
                    'empty component has identity')
            start, end, span = (number(row, n) for n in
                (('begin', 'end', 'maximum_cycles') if kind == 'CALLBACK' else ('begin_ns', 'end_ns', 'maximum_ns')))
            if span:
                require(start > 0 and end > start and end-start == span and
                        number(row, 'first_generation') > 0 and number(row, 'last_generation') > 0 and
                        number(row, 'generation') > 0 and number(row, 'root') > 0 and number(row, 'root') % 4096 == 0,
                        'component maximum endpoint identity')
                require(number(row, 'context') in ((1, 2) if kind == 'CALLBACK' else (1,)),
                        'component execution context')
            else:
                require(start == end == 0 and count == invalid, 'component valid samples lack maximum')
            if kind == 'CALLBACK':
                hz = clock[(0, cpu)]['conversion_hz']
                require(end <= cutoffs[cpu] and number(row, 'frequency_hz') == hz and
                        number(row, 'ns_available') == 1 and number(row, 'maximum_ns') == ceildiv(span*10**9, hz),
                        'callback conversion or terminal cutoff')
            answer[key].append({'cpu': cpu, 'operation': operation, 'count': count, 'invalid': invalid,
                'failures': failed, 'maximum_ns': number(row, 'maximum_ns'), 'generation': number(row, 'generation'),
                'role': 'ORDINARY_MAINTENANCE' if operation in (1, 3) else 'TEST_PROBE'})
    return answer


def inspect(serial, *, primitive='ordered-tsc-irq-envelope'):
    raw = records(serial)
    require(len(raw['BEGIN']) == len(raw['TERMINAL']) == len(raw['CALIBRATION_END']) == 1, 'terminal lifecycle coverage')
    cpus = number(raw['BEGIN'][0], 'cpus')
    require(1 <= cpus <= 32 and number(raw['BEGIN'][0], 'budget_ns') == BUDGET_NS and
            raw['BEGIN'][0]['status'] == 'ACTIVE', 'observer activation')
    clock = clocks(raw, cpus)
    maximum = maxima(raw, cpus, clock, primitive=primitive)
    tickets = contention(raw, cpus)
    quantities = components(raw, cpus, clock)
    terminal = raw['TERMINAL'][0]
    require(number(terminal, 'cpus') == number(terminal, 'frozen') == cpus and
            terminal['transport'] == 'COMPLETE', 'incomplete terminal transport')
    observer_failures = [cpu for cpu in range(cpus) if clock[(1, cpu)]['observer_errors']]
    require(not observer_failures or raw['CALIBRATION_END'][0]['status'] != 'VALID', 'false calibration success')
    invalid = [{key: row[key] for key in ('cpu', 'active', 'pending', 'invalid', 'exhausted',
                'recursion', 'missing_begin', 'unclosed')}
               for row in raw['CPU'] if any(number(row, key) for key in
                    ('active', 'pending', 'invalid', 'exhausted', 'recursion', 'missing_begin', 'unclosed'))]
    return {'schema': 1, 'authority': 'RAW_REPLAY_ONLY_NOT_CAMPAIGN_ACCEPTANCE',
            'cpus': cpus, 'raw_records': sum(map(len, raw.values())),
            'observer_post_failures': observer_failures, 'invalid_cpu_states': invalid,
            'auxiliary_trace_incomplete': [{'cpu': number(row, 'cpu'), 'omitted_details': number(row, 'failure_overflow')}
                                          for row in raw['CPU'] if number(row, 'failure_overflow')],
            'complete_violation_accounting_fields': all('violations' in row for row in raw['MAXIMUM']),
            'contention': tickets,
            'components': quantities,
            'invalid_boundaries': number(terminal, 'invalid_boundaries'),
            'missing_runtime_paths': [{'cpu': number(row, 'cpu'), 'path': PATHS[number(row, 'path')]}
                                     for row in raw['MAXIMUM'] if number(row, 'path') < 11 and not number(row, 'count')],
            'maxima': maximum,
            'clock': [{'role': role, 'cpu': cpu, **row} for (role, cpu), row in sorted(clock.items())]}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.serial.read_text())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key not in ('clock', 'maxima')}))
