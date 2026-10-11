"""Recompute timing host eligibility from captured process and cgroup facts."""
from fractions import Fraction
from pathlib import PurePosixPath
import re

from irq_latency_evidence import require


def cpus(text):
    result = set()
    for item in text.split(','):
        require(bool(re.fullmatch(r'[0-9]+(?:-[0-9]+)?', item)), 'host CPU range')
        endpoints = list(map(int, item.split('-')))
        first, last = endpoints[0], endpoints[-1]
        require(0 <= first <= last < 1048576, 'host CPU range bounds')
        result.update(range(first, last+1))
    return result


def capacity(affinity, online, group):
    require(len(affinity) == len(set(affinity)) and set(affinity) <= set(online), 'host affinity identity')
    lines = [line[3:] for line in group['proc_cgroup'] if line.startswith('0::')]
    require(len(lines) == 1 and lines[0].startswith('/') and '..' not in PurePosixPath(lines[0]).parts,
            'host unified cgroup identity')
    root = PurePosixPath('/sys/fs/cgroup')
    path = root / lines[0].lstrip('/')
    expected = []
    while True:
        expected.append(str(path))
        if path == root:
            break
        path = path.parent
    rows = group['ancestors']
    require([row['path'] for row in rows] == expected, 'host cgroup ancestor coverage')
    usable, limits = set(affinity), []
    for i, row in enumerate(rows):
        fields = row['fields']
        require('cgroup.controllers' in fields and 'cgroup.subtree_control' in fields,
                'host controller availability unavailable')
        # Controller interfaces in a non-root group follow the parent's
        # subtree_control. Disabled descendants still inherit ancestor limits.
        if i+1 < len(rows):
            parent = rows[i+1]['fields']
            require('cgroup.subtree_control' in parent, 'host parent controllers unavailable')
            enabled = set(parent['cgroup.subtree_control'].split())
            for controller, interface in [('cpu', 'cpu.max'), ('cpuset', 'cpuset.cpus.effective'),
                                          ('memory', 'memory.max')]:
                require((interface in fields) == (controller in enabled), 'host controller interface coverage: '+interface)
            require(fields.get('cgroup.type') == 'domain', 'host threaded cgroup not covered')
        if 'cpuset.cpus.effective' in fields:
            usable &= cpus(fields['cpuset.cpus.effective'])
        if 'cpu.max' in fields:
            quota, period = fields['cpu.max'].split()
            require(period.isdecimal() and int(period) > 0, 'host quota period')
            if quota != 'max':
                require(quota.isdecimal() and int(quota) > 0, 'host quota allowance')
                limits.append(Fraction(int(quota), int(period)))
    return usable, min([Fraction(len(usable)), *limits])


def option(argv, name):
    require(argv.count(name) == 1 and argv.index(name)+1 < len(argv), 'host launch option: '+name)
    return argv[argv.index(name)+1]


def verify_snapshot(row, launch, guest_cpus=8, *, require_capacity=True, diagnostic_backend=False):
    require(row['schema'] == 1 and row['guest_cpus'] == guest_cpus and row['placement_changed'] is False,
            'host profile or changed placement')
    usable, rate = capacity(row['affinity'], row['online'], row['cgroup'])
    require(row['usable_cpus'] == sorted(usable) and
            Fraction(row['capacity_numerator'], row['capacity_denominator']) == rate and
            row['capacity_sufficient'] == (rate >= guest_cpus), 'host reported capacity differs')
    require(not require_capacity or rate >= guest_cpus, 'host oversubscription')
    require(row['kvm_open'] == 'PASS' and row['vm_pid'] == row['observed_pid'], 'host observed VM identity')
    processes = row['qemu_processes']
    require(len(processes) == 1 and processes[0]['pid'] == row['vm_pid'], 'host concurrent or missing VM')
    argv = processes[0]['argv']
    require(argv[1:] == launch['argv'][1:] and
            option(argv, '-machine') == ('q35,memory-backend=diagnostic.ram' if diagnostic_backend else 'q35') and option(argv, '-accel') == 'kvm' and
            option(argv, '-smp') == f'{guest_cpus},sockets=1,cores={guest_cpus},threads=1',
            'host actual launch identity')
    threads = row['threads']
    require(len({thread['tid'] for thread in threads}) == len(threads), 'host duplicate thread')
    identity = row['qmp_identity']
    require(identity['uuid'] == option(argv, '-uuid'), 'host actual QMP UUID')
    replies = {entry['receive']['id']: entry['receive']['return'] for entry in identity['transcript']
               if 'receive' in entry and 'id' in entry['receive'] and 'return' in entry['receive']}
    require(replies.get('irq-host-query-uuid') == {'UUID': identity['uuid']} and
            replies.get('irq-host-query-cpus-fast') == identity['cpus'], 'host raw QMP identity differs')
    require({cpu['cpu-index'] for cpu in identity['cpus']} == set(range(guest_cpus)) and
            len(identity['cpus']) == guest_cpus, 'host actual vCPU inventory')
    tids = [cpu['thread-id'] for cpu in identity['cpus']]
    require(len(set(tids)) == guest_cpus and set(tids) <= {thread['tid'] for thread in threads},
            'host vCPU to native thread identity')
    vcpus = [thread for thread in threads if thread['tid'] in tids]
    for thread in threads:
        thread_usable, thread_rate = capacity(thread['affinity'], row['online'], thread['cgroup'])
        require(thread_usable == usable and thread['affinity'] == row['affinity'] and
                thread_rate == rate and (not require_capacity or thread_rate >= guest_cpus),
                'host thread affinity or quota contradicts default placement')
        require(len(thread['schedstat'].split()) == 3 and all(v.isdecimal() for v in thread['schedstat'].split()),
                'host scheduler observation unavailable')
    topology = row['topology']
    require({entry['cpu'] for entry in topology} == set(row['online']) and len(topology) == len(row['online']),
            'host topology coverage')
    for entry in topology:
        require(entry['cpu'] in cpus(entry['thread_siblings_list']) and
                entry['physical_package_id'].isdecimal() and entry['core_id'].isdecimal(), 'host topology identity')
    memory = option(argv, '-m')
    match = re.fullmatch(r'([0-9]+)([MG])?', memory)
    require(match, 'host guest memory size')
    guest_bytes = int(match[1]) * (1024**3 if match[2] == 'G' else 1024**2)
    memory_limits = []
    for ancestor in row['cgroup']['ancestors']:
        fields = ancestor['fields']
        if 'memory.max' not in fields:
            continue
        limit = fields['memory.max']
        require(limit == 'max' or limit.isdecimal() and int(limit) >= guest_bytes,
                'host memory limit below guest reservation')
        require(fields.get('memory.current', '').isdecimal() and 'memory.events' in fields,
                'host memory accounting unavailable')
        memory_limits.append({'path': ancestor['path'], 'maximum': limit, 'high': fields['memory.high']})
    return {'usable_cpus': sorted(usable), 'capacity': str(rate), 'vm_pid': row['vm_pid'],
            'vcpu_threads': [thread['tid'] for thread in vcpus], 'memory_limits': memory_limits}


def verify_series(rows, launch, guest_cpus=8, *, require_capacity=True, diagnostic_backend=False):
    require(len(rows) >= 3, 'host temporal coverage')
    results = [verify_snapshot(row, launch, guest_cpus, require_capacity=require_capacity, diagnostic_backend=diagnostic_backend) for row in rows]
    require(len({row['host_boot_id'] for row in rows}) == 1 and
            all(a['monotonic_ns'] < b['monotonic_ns'] for a, b in zip(rows, rows[1:])), 'host epoch or chronology')
    require(all(result == results[0] for result in results), 'host execution capacity changed')
    for before, after in zip(rows, rows[1:]):
        for a, b in zip(before['cgroup']['ancestors'], after['cgroup']['ancestors']):
            if 'memory.events' not in a['fields']:
                continue
            initial = dict(line.split() for line in a['fields']['memory.events'].splitlines())
            final = dict(line.split() for line in b['fields']['memory.events'].splitlines())
            require(all(int(final[key]) == int(initial[key]) for key in ('oom', 'oom_kill')),
                    'host memory exhaustion during series')
    return {'status': 'PASS', 'authority': 'CAPTURED_HOST_CAPACITY_ONLY', **results[0]}


def verify_profile_series(rows, launch, role, guest_cpus, *, diagnostic_backend=False):
    require(role in ('DIAGNOSTIC_NO_ACCEPTANCE','CURRENT_TIMING_QUALIFICATION',
                     'CURRENT_FUNCTIONAL_STRESS'), 'host unknown execution role')
    functional = role == 'CURRENT_FUNCTIONAL_STRESS'
    require(role=='DIAGNOSTIC_NO_ACCEPTANCE' or not any(arg in launch.get('argv',[]) for arg in ('-trace','-msg')),
            'diagnostic QEMU tracing cannot supply acceptance')
    require(not diagnostic_backend or role=='DIAGNOSTIC_NO_ACCEPTANCE', 'diagnostic RAM cannot supply timing acceptance')
    require(guest_cpus == (24 if functional else 8), 'host role/topology contract')
    result = verify_series(rows, launch, guest_cpus, require_capacity=not functional, diagnostic_backend=diagnostic_backend)
    return dict(result, guest_cpus=guest_cpus, role=role,
                capacity_sufficient=Fraction(result['capacity']) >= guest_cpus,
                timing_authority_eligible=role=='CURRENT_TIMING_QUALIFICATION' and not diagnostic_backend,
                authority='FUNCTIONAL_IDENTITY_ONLY' if functional else 'CAPTURED_HOST_CAPACITY_ONLY')
