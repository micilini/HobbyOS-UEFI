"""Bounded scheduling corroboration; never attributes one guest interval."""
from irq_latency_evidence import require


def identity(stat):
    left, separator, fields = stat.rpartition(') ')
    require(separator and '(' in left, 'scheduling process stat format')
    pid = int(left.split('(', 1)[0].strip())
    values = fields.split()
    require(len(values) >= 20 and values[19].isdecimal(), 'scheduling process start time')
    return pid, int(values[19])  # stat field 22, after comm and pid


def counters(text):
    rows = [line.split() for line in text.splitlines()]
    require(all(len(r) == 2 and r[1].isdecimal() for r in rows), 'scheduling cgroup counters')
    require(len({r[0] for r in rows}) == len(rows), 'scheduling duplicate counter')
    return {k: int(v) for k, v in rows}


def pressure(text):
    result = {}
    for line in text.splitlines():
        kind, *fields = line.split()
        require(kind in ('some', 'full') and kind not in result, 'scheduling pressure kind')
        pairs = [f.split('=') for f in fields]
        require(all(len(p) == 2 for p in pairs), 'scheduling pressure fields')
        row = dict(pairs)
        require(len(row) == len(pairs) and row.get('total', '').isdecimal(), 'scheduling pressure total')
        result[kind] = int(row['total'])
    require('some' in result, 'scheduling pressure some unavailable')
    return result


def delta(a, b):
    require(a.keys() == b.keys() and all(b[k] >= a[k] for k in a),
            'scheduling counter reset/wrap or coverage')
    return {k: b[k]-a[k] for k in a}


def verify(rows):
    require(len(rows) >= 3, 'scheduling temporal coverage')
    identities, samples = [], []
    for row in rows:
        begin, end = row['collection_begin_ns'], row['collection_end_ns']
        require(0 < begin <= end <= row['monotonic_ns'], 'scheduling collection interval')
        process = identity(row['process_stat_begin'])
        require(process == identity(row['process_stat_end']) and process[0] == row['vm_pid'],
                'scheduling process lifetime')
        qmp = row['qmp_identity']
        mapping = {c['cpu-index']: c['thread-id'] for c in qmp['cpus']}
        require(len(mapping) == len(qmp['cpus']) == row['guest_cpus'] and
                set(mapping) == set(range(row['guest_cpus'])), 'scheduling QMP mapping')
        tids = {t['tid']: t for t in row['threads']}
        require(len(tids) == len(row['threads']) and set(mapping.values()) <= set(tids),
                'scheduling thread inventory')
        lifetimes, threads = {}, {}
        for cpu, tid in mapping.items():
            t = tids[tid]
            require(begin <= t['collection_begin_ns'] <= t['collection_end_ns'] <= end,
                    'scheduling thread collection interval')
            lifetime = identity(t['stat_begin'])
            require(lifetime == identity(t['stat']) and lifetime[0] == tid, 'scheduling thread lifetime')
            values = t['schedstat'].split()
            require(len(values) == 3 and all(v.isdecimal() for v in values), 'scheduling schedstat unavailable')
            lifetimes[cpu] = lifetime
            threads[cpu] = dict(zip(('execution_ns', 'runqueue_ns', 'timeslices'), map(int, values)))
        identities.append((row['host_boot_id'], process, qmp['uuid'], lifetimes))
        groups = {}
        for g in row['cgroup']['ancestors']:
            facts = {}
            for name, parse in [('cpu.stat', counters), ('cpu.pressure', pressure)]:
                if name in g['fields']:
                    facts[name] = {'status': 'AVAILABLE', 'counters': parse(g['fields'][name])}
                else:
                    missing = g.get('unavailable', {}).get(name)
                    require(missing and missing['status'] == 'UNAVAILABLE', 'scheduling missing interface fact')
                    facts[name] = missing
            require(g['path'] not in groups, 'scheduling duplicate cgroup')
            groups[g['path']] = facts
        samples.append({'begin_ns': begin, 'end_ns': end, 'threads': threads, 'groups': groups})
    require(all(i == identities[0] for i in identities), 'scheduling process/thread epoch changed')
    intervals = []
    for a, b in zip(samples, samples[1:]):
        require(a['end_ns'] < b['begin_ns'], 'scheduling overlapping/out-of-order collections')
        require(a['groups'].keys() == b['groups'].keys(), 'scheduling cgroup changed')
        groups = {}
        for path in a['groups']:
            groups[path] = {}
            for name, before in a['groups'][path].items():
                after = b['groups'][path][name]
                if before['status'] == after['status'] == 'AVAILABLE':
                    groups[path][name] = {'status': 'AVAILABLE', 'delta': delta(before['counters'], after['counters'])}
                else:
                    groups[path][name] = {'status': 'UNAVAILABLE', 'before': before, 'after': after}
        intervals.append({'outer_collection_bounds_ns': [a['begin_ns'], b['end_ns']],
            'vcpus': {cpu: delta(a['threads'][cpu], b['threads'][cpu]) for cpu in a['threads']},
            'cgroups': groups})
    return {'schema': 1, 'status': 'PASS', 'authority': 'AGGREGATE_SCHEDULING_COUNTERS_ONLY',
            'individual_guest_event_attribution': 'NOT_ESTABLISHED', 'intervals': intervals}
