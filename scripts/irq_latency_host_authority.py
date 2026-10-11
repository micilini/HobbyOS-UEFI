"""Capture usable host capacity without changing affinity, quotas or placement."""
import argparse
import datetime
from fractions import Fraction
import json
import os
from pathlib import Path
import socket
import time


def read(path):
    return path.read_text().strip()


def cpu_set(text):
    result = set()
    if not text:
        return result
    for item in text.split(','):
        endpoints = item.split('-')
        if len(endpoints) == 1:
            result.add(int(endpoints[0]))
        elif len(endpoints) == 2:
            first, last = map(int, endpoints)
            if first > last:
                raise ValueError('reversed CPU range')
            result.update(range(first, last+1))
        else:
            raise ValueError('invalid CPU range')
    return result


def effective_capacity(affinity, online, ancestors):
    usable = set(affinity) & set(online)
    quotas = []
    for row in ancestors:
        fields = row['fields']
        if 'cpuset.cpus.effective' in fields:
            usable &= cpu_set(fields['cpuset.cpus.effective'])
        if 'cpu.max' in fields:
            quota, period = fields['cpu.max'].split()
            if int(period) <= 0:
                raise ValueError('invalid quota period')
            if quota != 'max':
                if int(quota) <= 0:
                    raise ValueError('invalid quota allowance')
                quotas.append(Fraction(int(quota), int(period)))
    capacity = min([Fraction(len(usable)), *quotas])
    return sorted(usable), capacity


def cgroups(pid):
    lines = read(Path('/proc') / str(pid) / 'cgroup').splitlines()
    unified = [line[3:] for line in lines if line.startswith('0::')]
    if len(unified) != 1:
        raise ValueError('unified cgroup capacity unavailable')
    relative = Path(unified[0])
    if not relative.is_absolute() or '..' in relative.parts:
        raise ValueError('cgroup namespace ancestry unavailable')
    root = Path('/sys/fs/cgroup')
    current = root / relative.relative_to('/')
    rows = []
    while True:
        if not current.is_dir():
            raise ValueError('cgroup ancestor unavailable')
        fields = {}
        unavailable = {}
        for name in ('cgroup.controllers', 'cgroup.subtree_control', 'cgroup.type',
                     'cpu.max', 'cpu.weight', 'cpu.stat', 'cpu.pressure', 'cpuset.cpus.effective',
                     'cpuset.mems.effective', 'memory.max', 'memory.high', 'memory.current', 'memory.events',
                     'memory.stat', 'memory.pressure'):
            file = current / name
            try:
                fields[name] = read(file)
            except OSError as error:
                unavailable[name] = {'status': 'UNAVAILABLE', 'errno': error.errno}
        rows.append({'path': str(current), 'fields': fields, 'unavailable': unavailable})
        if current == root:
            break
        current = current.parent
    return {'proc_cgroup': lines, 'ancestors': rows}


def qmp_identity(argv):
    argument = argv[argv.index('-qmp')+1]
    if not argument.startswith('unix:'):
        raise ValueError('host CPU identity needs the owned QMP Unix socket')
    transcript = []
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(argument[5:].split(',')[0])
        with client.makefile('rwb', buffering=0) as stream:
            def receive():
                row = json.loads(stream.readline(1048576))
                transcript.append({'receive': row})
                return row
            if 'QMP' not in receive():
                raise ValueError('host QMP greeting missing')
            results = {}
            for request in ('qmp_capabilities', 'query-uuid', 'query-cpus-fast'):
                command = {'execute': request, 'id': 'irq-host-'+request}
                transcript.append({'send': command})
                stream.write((json.dumps(command)+'\n').encode())
                for _ in range(256):
                    response = receive()
                    if response.get('id') == command['id']:
                        if 'error' in response or 'return' not in response:
                            raise ValueError('host QMP identity request failed')
                        results[request] = response['return']
                        break
                else:
                    raise ValueError('host QMP event limit')
    return {'transcript': transcript, 'uuid': results['query-uuid']['UUID'],
            'cpus': results['query-cpus-fast']}


def snapshot(guest_cpus, vm_pid=None):
    collection_begin = time.monotonic_ns()
    pid = vm_pid if vm_pid is not None else os.getpid()
    process_stat_begin = read(Path('/proc') / str(pid) / 'stat')
    group = cgroups(pid)
    online = sorted(cpu_set(read(Path('/sys/devices/system/cpu/online'))))
    affinity = sorted(os.sched_getaffinity(pid))
    usable, capacity = effective_capacity(affinity, online, group['ancestors'])
    topology = []
    for cpu in online:
        base = Path('/sys/devices/system/cpu') / ('cpu'+str(cpu)) / 'topology'
        topology.append({'cpu': cpu, **{name: read(base/name) for name in
                         ('physical_package_id', 'core_id', 'thread_siblings_list')}})
    vms = []
    for directory in Path('/proc').iterdir():
        if not directory.name.isdigit():
            continue
        try:
            if not read(directory/'comm').startswith('qemu-system'):
                continue
            argv = (directory/'cmdline').read_bytes().split(b'\0')
            vms.append({'pid': int(directory.name), 'argv': [arg.decode() for arg in argv if arg]})
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    threads = []
    identity = None
    if vm_pid is not None:
        matched = [vm for vm in vms if vm['pid'] == vm_pid]
        if len(matched) != 1:
            raise ValueError('host observed VM not inventoried')
        identity = qmp_identity(matched[0]['argv'])
        for directory in sorted((Path('/proc') / str(vm_pid) / 'task').iterdir()):
            tid = int(directory.name)
            begin = time.monotonic_ns()
            stat_begin = read(directory/'stat')
            threads.append({'tid': tid, 'comm': read(directory/'comm'),
                            'affinity': sorted(os.sched_getaffinity(tid)),
                            'cgroup': cgroups(tid),
                            'schedstat': read(directory/'schedstat'), 'stat': read(directory/'stat'),
                            'stat_begin': stat_begin, 'collection_begin_ns': begin,
                            'collection_end_ns': time.monotonic_ns()})
    memory = read(Path('/proc/meminfo'))
    kvm = os.open('/dev/kvm', os.O_RDWR)
    os.close(kvm)
    process_stat_end = read(Path('/proc') / str(pid) / 'stat')
    return {'schema': 1, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'collection_begin_ns': collection_begin, 'collection_end_ns': time.monotonic_ns(),
            'process_stat_begin': process_stat_begin, 'process_stat_end': process_stat_end,
            'monotonic_ns': time.monotonic_ns(), 'host_boot_id': read(Path('/proc/sys/kernel/random/boot_id')),
            'guest_cpus': guest_cpus, 'observed_pid': pid, 'vm_pid': vm_pid,
            'online': online, 'affinity': affinity, 'usable_cpus': usable,
            'capacity_numerator': capacity.numerator, 'capacity_denominator': capacity.denominator,
            'capacity_sufficient': capacity >= guest_cpus, 'topology': topology, 'cgroup': group,
            'qemu_processes': vms, 'threads': threads, 'meminfo': memory,
            'qmp_identity': identity,
            'loadavg': read(Path('/proc/loadavg')), 'cpuinfo': read(Path('/proc/cpuinfo')),
            'kvm_open': 'PASS', 'placement_changed': False,
            'scope': 'HOST_CAPACITY_INPUTS_NOT_GUEST_TIMING_ACCEPTANCE'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest-cpus', type=int, required=True)
    parser.add_argument('--vm-pid', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.guest_cpus <= 32:
        parser.error('guest CPU count outside discovered-kernel capacity')
    result = snapshot(args.guest_cpus, args.vm_pid)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({name: result[name] for name in
                      ('guest_cpus', 'usable_cpus', 'capacity_numerator', 'capacity_denominator', 'capacity_sufficient')}))
