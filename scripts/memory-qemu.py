#!/usr/bin/env python3
"""One isolated, recorded boot using the existing framed command transport."""
import argparse
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
import importlib.util
import memory_evidence as E
import validation_candidates as candidates
from memory_table_proof import STATIC_NAMES


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''): h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--machine', choices=('q35', 'pc'), required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), required=True)
    parser.add_argument('--smp', type=int, required=True)
    parser.add_argument('--control-profile', choices=('normal', 'nx-disabled', 'xapic'), default='normal')
    parser.add_argument('--ram-profile', choices=('normal','high','high-fallback'), default='normal')
    parser.add_argument('--device-profile', choices=('direct', 'hub-keyboard'), default='direct')
    parser.add_argument('--serial-backend', choices=('socket', 'file'), default='socket')
    parser.add_argument('--fault-trace', action='store_true')
    parser.add_argument('--production', action='store_true')
    parser.add_argument('--readback', action='store_true', help='Separate diagnostic run: read selected table bytes with GDB after commands.')
    parser.add_argument('--low-bootstrap-readback',action='store_true',help='Separate stopped pre-AP and retired-root physical readback.')
    parser.add_argument('--command', action='append')
    args = parser.parse_args()
    if args.control_profile == 'nx-disabled' and (args.machine, args.accel, args.smp, args.production) != ('q35','tcg',4,False):
        parser.error('nx-disabled requires the supplemental Q35/TCG/SMP4 diagnostic build')
    if args.control_profile == 'xapic' and (args.machine, args.accel, args.smp, args.production) != ('q35','kvm',4,False):
        parser.error('xapic requires the supplemental Q35/KVM/SMP4 diagnostic build')
    if args.device_profile == 'hub-keyboard' and (args.machine, args.smp, args.production, args.control_profile) != ('q35',4,False,'normal'):
        parser.error('hub-keyboard requires the named Q35/SMP4 diagnostic topology')
    if args.ram_profile != 'normal' and (args.machine,args.accel,args.smp,args.production,args.control_profile,args.device_profile) != (
            'q35','kvm' if args.ram_profile=='high' else 'tcg',4,False,'normal','direct'):
        parser.error('RAM supplement requires its exact Q35/SMP4 diagnostic topology')
    if args.low_bootstrap_readback and (not args.readback or args.production or args.smp!=4 or
        os.environ.get('HOBBYOS_VALIDATION_CONTRACT') not in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks')):
        parser.error('bootstrap readback requires the named SELFTEST SMP4 no-low diagnostic')
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    evidence, candidate = args.evidence.resolve(), candidates.resolve(args.candidate.resolve())
    evidence.relative_to(root / 'backups')
    evidence.mkdir(parents=True, exist_ok=False)
    candidate_receipt = json.loads((candidate / 'candidate.json').read_text())
    build_receipt = json.loads((candidate / 'build-receipt.json').read_text())
    candidate_id = E.verify_candidate(candidate_receipt, build_receipt)
    launch_id, vm_id, boot_id = (str(uuid.uuid4()) for _ in range(3))
    started_ns = time.monotonic_ns()
    started_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
    shutil.copy2(candidate / 'build-receipt.json', evidence / 'build-receipt.json')
    shutil.copytree(candidate / 'build-commands', evidence / 'build-commands')
    for name in ('kernel.elf', 'hobbyos.img'):
        if sha(candidate / name) != candidate_receipt['artifacts'][name]['sha256']:
            raise ValueError('candidate artifact hash mismatch: ' + name)
    shutil.copy2(candidate / 'candidate.json', evidence / 'candidate.json')
    for name in ('symbols.txt', 'elf.txt'):
        shutil.copy2(candidate / name, evidence / name)
    (evidence / 'elf-facts.json').write_text(json.dumps({
        'elf_sha256': sha(candidate / 'kernel.elf'),
        'symbols_sha256': sha(evidence / 'symbols.txt'),
        'headers_sha256': sha(evidence / 'elf.txt'),
        'scope': 'Captured tool output; portable replay does not execute the excluded binary.'}, indent=2) + '\n')
    if args.production:
        spec = importlib.util.spec_from_file_location('memory_build_gate', root / 'scripts/memory-observation-gate.py')
        gate = importlib.util.module_from_spec(spec); spec.loader.exec_module(gate)
        # Inspect the frozen binary used by this launch, retaining raw string facts.
        E.write(evidence / 'absence.json', gate.absent(candidate))
        shutil.copy2(candidate / 'compiled-strings.txt', evidence / 'compiled-strings.txt')
    # A compact timestamped session keeps filesystem socket names below sun_path.
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    session = root / 'backups' / stamp
    runtime = session / 'r'
    runtime.mkdir(parents=True)
    (session / 'session.json').write_text(json.dumps({'kind': 'isolated-runtime',
        'parent_evidence': str(evidence), 'created_utc': stamp, 'pull': 'NOT_RUN'}, indent=2) + '\n')
    subprocess.run(['cp','--sparse=always','--preserve=mode,timestamps',
                    str(candidate / 'hobbyos.img'),str(runtime / 'working.img')],check=True)
    E.require(sha(runtime/'working.img')==candidate_receipt['artifacts']['hobbyos.img']['sha256'],
              'sparse working image copy differs from frozen payload')
    profile = f'{args.machine}-{args.accel}-smp{args.smp}'
    if args.ram_profile != 'normal': profile += '-high-ram' + ('-fallback' if args.ram_profile=='high-fallback' else '')
    if args.control_profile != 'normal': profile += '-' + args.control_profile
    if args.device_profile != 'direct': profile += '-' + args.device_profile
    serial_path = runtime / 'qemu-serial.log'
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    serial_socket = runtime / 's.sock'
    listener.bind(str(serial_socket))
    listener.listen(1)
    listener.settimeout(1)
    stopped = threading.Event()
    collector_error = []

    def collect():
        try:
            if args.serial_backend == 'file':
                while not serial_path.exists() and not stopped.wait(0.0005): pass
                if not serial_path.exists(): return
                with serial_path.open('rb') as source, \
                        (runtime/'serial-receipts.bin').open('wb',buffering=65536) as events:
                    while True:
                        data=source.read(64)
                        received=time.monotonic_ns()
                        if data:
                            events.write(struct.pack('<QI',received,len(data)));events.write(data)
                        elif stopped.is_set(): break
                        else: stopped.wait(0.0005)
                return
            connection = None
            while not stopped.is_set():
                try: connection, _ = listener.accept(); break
                except socket.timeout: pass
            if connection is None: return
            connection.settimeout(1)
            # Drain the UART promptly. JSON/base64 formatting is deferred until
            # the VM has stopped; the original per-recv timestamps are retained.
            with connection, serial_path.open('ab', buffering=65536) as raw, \
                    (runtime / 'serial-receipts.bin').open('wb', buffering=65536) as events:
                last_flush = 0
                while not stopped.is_set():
                    try: data = connection.recv(64)
                    except socket.timeout: continue
                    received = time.monotonic_ns()
                    if not data: break
                    raw.write(data)
                    if b'\n' in data or received - last_flush >= 1000000:
                        raw.flush(); last_flush = received
                    events.write(struct.pack('<QI', received, len(data)))
                    events.write(data)
        except BaseException as error:
            collector_error.append(repr(error))

    collector = threading.Thread(target=collect, daemon=True)
    collector.start()
    env = dict(os.environ, QEMU_RUNTIME=str(runtime), QEMU_GDB_SOCKET=str(runtime / 'gdb.sock'),
        QEMU_SERIAL_SOCKET=str(serial_socket) if args.serial_backend == 'socket' else '', QEMU_LAUNCH_RECEIPT=str(evidence / 'launch.json'),
        QEMU_CANDIDATE_RECEIPT=str(candidate / 'candidate.json'), QEMU_VM_ID=vm_id,
        QEMU_LAUNCH_ID=launch_id, QEMU_BOOT_ID=boot_id, QEMU_FAULT_TRACE='1' if args.fault_trace else '0', HOBBYOS_IMAGE=str(runtime / 'working.img'),
        HOBBYOS_KERNEL=str(candidate / 'kernel.elf'), SMP=str(args.smp), ACCEL=args.accel,
        MACHINE=args.machine, MEM='2G', PYTHONDONTWRITEBYTECODE='1', QEMU_CONTROL_PROFILE=args.control_profile, QEMU_DEVICE_PROFILE=args.device_profile, QEMU_RAM_PROFILE=args.ram_profile)
    if args.low_bootstrap_readback:env['QEMU_START_PAUSED']='1'
    commands = []

    def run(name, argv, environment=None):
        started = time.monotonic_ns()
        shared=(re.fullmatch(r'command-[1-9][0-9]*',name) is not None and
                os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'))
        framed=evidence/'framed.jsonl';offset=framed.stat().st_size if framed.exists() else 0
        stdout_reference=None
        with (evidence / (name + '.stderr.log')).open('w') as err:
            if shared:
                process=subprocess.run(argv,env=environment or env,stdout=subprocess.PIPE,stderr=err)
                # The transport emits the exact appended transaction as stdout.
                # Store that immutable byte sequence once; preserve unexpected
                # output separately rather than discard it or invent a summary.
                with framed.open('rb') if framed.exists() else __import__('io').BytesIO() as stream:
                    stream.seek(offset);recorded=stream.read()
                if process.stdout==recorded and 0<len(recorded)<=64*1024*1024:
                    stdout_reference={'codec':'exact-framed-range','path':'framed.jsonl','offset':offset,
                        'size':len(recorded),'sha256':hashlib.sha256(recorded).hexdigest()}
                else:(evidence/(name+'.stdout.log')).write_bytes(process.stdout)
            else:
                with (evidence/(name+'.stdout.log')).open('w') as out:
                    process=subprocess.run(argv,env=environment or env,stdout=out,stderr=err)
        record = {'name': name, 'argv': argv, 'exit_status': process.returncode,
                  'start_ns': started, 'end_ns': time.monotonic_ns()}
        if stdout_reference:record['stdout_evidence']=stdout_reference
        commands.append(record)
        with (evidence / 'commands.jsonl').open('a') as stream: stream.write(json.dumps(record) + '\n')
        if process.returncode: raise RuntimeError(f'{name}: exit {process.returncode}')

    result = {'schema': 1, 'profile': profile, 'runtime_session': str(session),
        'canonical_candidate': str(candidate),
        'candidate_elf': sha(candidate / 'kernel.elf'), 'candidate_image': sha(candidate / 'hobbyos.img'),
        'serial_backend':args.serial_backend, 'device_profile':args.device_profile, 'ram_profile':args.ram_profile,
        'timing_method': ('Host CLOCK_MONOTONIC receipt after each socket recv(64); buffering and host scheduling remain uncertainty, not guest execution time.'
            if args.serial_backend=='socket' else 'Host CLOCK_MONOTONIC observation after each file read(64), polling EOF at 0.5 ms; file buffering and host scheduling remain unquantified uncertainty, not guest execution time.'),
        'clock_resolution_ns': time.get_clock_info('monotonic').resolution * 1e9,
        'trace_enabled': args.fault_trace, 'production': args.production,
        'readback': args.readback, 'low_bootstrap_readback':args.low_bootstrap_readback, 'status': 'RUNNING', 'candidate_id': candidate_id,
        'launch_id': launch_id, 'vm_id': vm_id, 'boot_id': boot_id, 'started_ns': started_ns, 'started_utc': started_utc}
    try:
        run('launch', ['scripts/qemu-agent.sh', 'start'])
        pid = int((runtime / 'qemu.pid').read_text())
        process_stat = Path(f'/proc/{pid}/stat').read_text()
        spec = importlib.util.spec_from_file_location('memory_transport', root / 'scripts/foundation-qemu.py')
        transport = importlib.util.module_from_spec(spec); spec.loader.exec_module(transport)
        client = transport.QmpInputClient(runtime / 'qmp.sock', evidence / 'vm-identity.jsonl', time.monotonic() + 5)
        try:
            _, answer = client.command('query-uuid', {}, time.monotonic() + 5)
            E.require(answer['return']['UUID'] == vm_id, 'actual QEMU UUID differs from launch')
            cpu_identity = []
            if args.readback or (args.production and os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks'):
                _, response = client.command('query-cpus-fast', {}, time.monotonic() + 5)
                for cpu in response['return']:
                    _, identity = client.command('qom-get', {'path':cpu['qom-path'], 'property':'apic-id'}, time.monotonic() + 5)
                    cpu_identity.append({'cpu_index':cpu['cpu-index'],'qom_path':cpu['qom-path'],
                                         'apic_id':identity['return']})
                E.require(len(cpu_identity) == args.smp, 'debugger CPU identity coverage')
        finally: client.close()
        E.write(evidence / 'boot-receipt.json', {'schema': 1, 'launch_id': launch_id, 'vm_id': vm_id,
            'boot_id': boot_id, 'candidate_id': candidate_id, 'pid': pid, 'process_stat': process_stat,
            'observed_ns': time.monotonic_ns(), 'observed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'qmp_sha256': sha(evidence / 'vm-identity.jsonl')})
        if args.low_bootstrap_readback:
            import low_readback
            low_readback.collect(root,evidence,runtime,candidate,result,run,before=True)
        markers = [('runtime-ready', f'[BOOT][RUNTIME_READY] PASS cpus={args.smp}/{args.smp}', 240)]
        if not args.production:
            markers.append(('test-ready', '[BOOT][TEST_READY] PASS autorun=0 selftests=0', 30))
        for name, marker, timeout in markers:
            run(name, ['python3', 'scripts/foundation-qemu.py', 'wait-ready', '--runtime', str(runtime),
                '--marker', marker, '--timeout', str(timeout), '--output', str(evidence / (name + '.json'))])
        if args.production and os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
            workers=re.findall(r'^\[BOOT\]\[DEVICE_OWNER\] id=([1-9][0-9]*) generation=([1-9][0-9]*)$',
                               serial_path.read_text(),re.M)
            E.require(len(workers)==1,'production device worker identity missing/duplicate')
            worker,lifecycle=workers[0]
            marker=f'[REAPER] FREE id={worker}'
            run('device-worker-retired',['python3','scripts/foundation-qemu.py','wait-ready',
                '--runtime',str(runtime),'--marker',marker,'--timeout','30',
                '--output',str(evidence/'device-worker-retired.json')])
            # The reaper records its zombie generation, not the task lifecycle
            # generation. Task IDs do not recycle within this boot.
            E.require(len(re.findall(r'^'+re.escape(marker)+r' generation=[1-9][0-9]* reason=NORMAL stack_bytes=16384$',
                serial_path.read_text(),re.M))==1,'production device worker retirement mismatch')
        if args.device_profile == 'hub-keyboard':
            run('usb-topology', ['python3', 'scripts/qemu_hmp.py', '--socket', str(runtime / 'hmp.sock'), 'command', 'info usb'])
        layout = evidence / 'input-layout'
        run('layout-build', ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I.',
            'scripts/command-transport-layout.c', '-o', str(layout)])
        run('layout', [str(layout)])
        (evidence / 'input-layout.json').write_text((evidence / 'layout.stdout.log').read_text().splitlines()[-1] + '\n')
        payloads = args.command or (['ptinspect', 'cpustate', 'memmap', 'mem', 'cpu', 'irq check'] if args.production else
                                  ['cpustate', 'cpustate', 'cpustate', 'ptinspect samples', 'memmap',
                                   'mem', 'cpu', 'irq check', 'irq controllers', 'irq routes', 'irq boot']
                                  )
        for sequence, payload in enumerate(payloads, 1):
            extra = (['--gdb-log', str(evidence / f'gdb-{sequence}.log')] if args.production else
                     ['--input-layout', str(evidence / 'input-layout.json'), '--input-profile', 'framed'])
            expected = '127' if args.production and payload in ('ptinspect', 'cpustate', 'memmap', 'tlbtest', 'phystest', 'mappingtest', 'rammap', 'rammap high', 'lowmap', 'lowmap crash', 'wxtest', 'stacktest', 'stacktest overflow', 'stacktest timeout') else '0'
            run(f'command-{sequence}', ['python3', 'scripts/foundation-qemu.py', 'production-command' if args.production else 'frame',
                '--root', str(root), '--runtime', str(runtime), '--gdb-socket', str(runtime / 'gdb.sock'),
                '--elf', str(candidate / 'kernel.elf'),
                '--record', str(evidence / 'framed.jsonl'), '--transport-log', str(evidence / f'transport-{sequence}.log'),
                '--profile-id', profile, '--vm-id', vm_id, '--candidate-sha256', result['candidate_elf'],
                '--sequence', str(sequence), '--payload', payload, '--timeout', '180',
                '--allowed-status', expected, *extra])
        if args.production and os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
            import device_production_readback
            device_production_readback.collect(root,evidence,runtime,candidate,result,run,cpu_identity=cpu_identity)
        if args.low_bootstrap_readback:
            low_readback.collect(root,evidence,runtime,candidate,result,run)
        if args.readback:
            commands_path = evidence / 'page-readback.gdb'
            targets = []
            for line in serial_path.read_text().splitlines():
                if not line.startswith('[PAGE_TABLE][ENTRY] '): continue
                fields = dict(item.split('=', 1) for item in line.split()[1:])
                if fields['sample'] in ('kernel', 'mmio', 'framebuffer'):
                    targets.append({'address': int(fields['table'], 16) + int(fields['index']) * 8,
                                    'sample': fields['sample'], 'level': int(fields['level']),
                                    'observed_raw': int(fields['raw'], 16)})
            if not targets: raise ValueError('no table readback targets')
            script = ['set pagination off', 'set confirm off', 'target remote ' + str(runtime / 'gdb.sock')]
            direct_map=os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks')
            if direct_map:script+=['maintenance packet Qqemu.PhyMemMode:1']
            script += [f'x/gx 0x{target["address"]:x}' for target in targets]
            if direct_map:script+=['maintenance packet Qqemu.PhyMemMode:0']
            fields = lambda line: dict(item.split('=', 1) for item in line.split()[1:])
            serial_lines = serial_path.read_text().splitlines()
            tables = fields(next(l for l in serial_lines if l.startswith('[PAGE_TABLE][TABLES] ')))
            control = fields(next(l for l in serial_lines if l.startswith('[PAGE_TABLE][CONTROL] ')))
            symbols = {}
            needed = set(STATIC_NAMES) | {'g_boot', 'g_observer_count', 'g_observer_limit', 'g_observer_failed',
                'g_observer_nonidentity', 'g_observer_epoch', 'g_observer_tables', 'g_kernel_pml4',
                'g_observer_boot_root', 'g_observer_reachable'}
            if direct_map:
                needed|={'g_kernel_root_physical','g_cpus','g_cpu_count','g_owned_window','g_boot_build',
                    'g_bitmap','g_reserved_bitmap','g_bitmap_buffer','g_reserved_bitmap_buffer'}
                run('ram-layout-build',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    'scripts/ram-readback-layout.c','-o',str(evidence/'ram-layout')])
                run('ram-layout',[str(evidence/'ram-layout')])
            stack_proof=os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks'
            if stack_proof:
                needed|={'g_stack_owners','g_stack_phase','g_stack_count_sequence','g_stack_allocated',
                    'g_stack_retired','g_stack_quarantined','g_stack_high_water'}
                run('stack-layout-build',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    'scripts/stack-readback-layout.c','-o',str(evidence/'stack-layout')])
                run('stack-layout',[str(evidence/'stack-layout')])
            tlb_proof = any(command in ('tlbtest', 'tlbtest run') for command in payloads)
            physical_proof = 'phystest' in payloads
            device_proof = 'mappingtest' in payloads
            if device_proof:
                layout_binary=evidence/'device-layout'
                run('device-layout-build',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie',
                    'scripts/device-readback-layout.c','-o',str(layout_binary)])
                run('device-layout',[str(layout_binary)])
                needed|={'g_views','g_dma','xhci_driver'}
            if physical_proof:
                needed |= {'g_kernel_root_physical', 'g_objects', 'g_view', 'g_map',
                           'g_framebuffer', 'g_font_header', 'g_logo'}
            if tlb_proof:
                needed |= {'g_idtr', 'g_idt', 'irq_vector_stub_table', 'irq_vector_225',
                           'irq_external_common_entry', 'irq_external_dispatch'}
            for line in (evidence / 'symbols.txt').read_text().splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[2] in needed:
                    E.require(parts[2] not in symbols, 'ambiguous capture symbol'); symbols[parts[2]] = int(parts[0], 16)
            E.require(set(symbols) == needed, 'capture symbol inventory incomplete')
            if physical_proof:
                run('physical-symbols', ['readelf', '-sW', str(candidate/'kernel.elf')])
                owner=None;matches=[]
                for line in (evidence/'physical-symbols.stdout.log').read_text().splitlines():
                    parts=line.split()
                    if len(parts)!=8 or not parts[0].endswith(':'):continue
                    if parts[3]=='FILE':owner=parts[7]
                    if owner=='boot_address.c' and parts[3]=='OBJECT' and parts[7]=='g_font':
                        E.require(parts[2]=='16','adapted font ELF extent');matches.append(int(parts[1],16))
                E.require(len(matches)==1,'adapted font ELF local identity')
                symbols['g_font']=matches[0]
            acpi_names = {'APIC', 'FACP', 'DSDT', 'HPET', 'MCFG', 'WAET', 'RSDP', 'XSDT', 'RSDT'}
            ranges = [fields(l) for l in serial_lines if l.startswith('[MEMORY_MAP][RANGE] ')]
            cfg = {'output': str(evidence), 'symbols': symbols, 'static_names': STATIC_NAMES,
                'reachable': int(tables['reachable']), 'physical_bits': int(control['physical_bits']),
                'page1g': control['page1g_supported'] == '1', 'candidate_id': candidate_id, 'vm_id': vm_id,
                'control_readback': True, 'cpu_identity': cpu_identity,
                'acpi_targets': [{'name': r['name'], 'base': int(r['base'], 16), 'bytes': int(r['bytes'])}
                                 for r in ranges if r['name'] in acpi_names]}
            cfg['physical_readback'] = physical_proof
            if stack_proof:cfg['stack_layout']=json.loads((evidence/'stack-layout.stdout.log').read_text())
            if direct_map:
                cfg['direct_map']=True
                cfg['ram_layout']=json.loads((evidence/'ram-layout.stdout.log').read_text())
            if device_proof:
                cfg['device_layout']=json.loads((evidence/'device-layout.stdout.log').read_text())
            if tlb_proof:
                spec = importlib.util.spec_from_file_location('tlb_trace_decoder', root / 'scripts/verify-tlb-shootdown.py')
                decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)
                cfg['tlb_readback'] = {'vector': 225, 'fixture_entries': sorted({
                    int(fields(line)['pte'], 16) for line in decoder.expand_trace(
                        (runtime/'qemu-debugcon.log').read_text().splitlines())
                    if line.startswith('[TLB][REPLACE] ')})}
            E.write(evidence / 'capture-config.json', cfg)
            shutil.copyfile(root / 'scripts/memory-gdb-capture.py', evidence / 'capture.py')
            capture_path = json.dumps(str(evidence / 'capture.py'))
            config_path = json.dumps(str(evidence / 'capture-config.json'))
            script += [f'python exec(compile(open({capture_path}).read(), "memory-capture", "exec"), dict(MEMORY_CAPTURE_CONFIG={config_path}))']
            if direct_map:
                shutil.copyfile(root/'scripts/ram-gdb-capture.py',evidence/'ram-capture.py')
                ram_capture_path=json.dumps(str(evidence/'ram-capture.py'))
                script += [f'python exec(compile(open({ram_capture_path}).read(), "ram-owner-capture", "exec"), dict(MEMORY_CAPTURE_CONFIG={config_path}))']
            if stack_proof:
                shutil.copyfile(root/'scripts/stack-gdb-capture.py',evidence/'stack-capture.py')
                stack_capture_path=json.dumps(str(evidence/'stack-capture.py'))
                script += [f'python exec(compile(open({stack_capture_path}).read(), "stack-owner-capture", "exec"), dict(STACK_CAPTURE_CONFIG={config_path}))']
            script += ['detach','quit']
            commands_path.write_text('\n'.join(script) + '\n')
            (evidence / 'page-readback-targets.json').write_text(json.dumps(targets, indent=2) + '\n')
            run('page-readback', ['gdb', '-q', '-batch', '-x', str(commands_path), str(candidate / 'kernel.elf')])
            values = re.findall(r'^0x([0-9a-f]+)(?:\s+<[^>]+>)?:\s+0x([0-9a-f]+)',
                                (evidence / 'page-readback.stdout.log').read_text(), re.M)
            if len(values) != len(targets): raise ValueError('GDB readback count differs')
            for target, (address, value) in zip(targets, values):
                raw, level = target['observed_raw'], target['level']
                allowed = (0x20 | (0x40 if level == 1 or level in (2, 3) and raw & 128 else 0)) if raw & 1 else 0
                if int(address, 16) != target['address'] or (int(value, 16) ^ raw) & ~allowed:
                    raise ValueError('GDB paging bytes differ (excluding hardware A/D)')
        result['status'] = 'COLLECTED'
    except BaseException as error:
        result['status'] = 'FAIL'
        result['error'] = str(error)
        # This post-failure stop is diagnostic evidence, never a passing retry.
        if (runtime / 'qemu.pid').is_file():
            failure_script = evidence / 'failure.gdb'
            failure_script.write_text('set pagination off\nset confirm off\n' +
                'target remote ' + str(runtime / 'gdb.sock') + '\n' +
                'info registers\nthread apply all bt 12\nx/12i $pc\n' +
                'x/gx &g_total_frames\nx/gx &g_free_frames\n' +
                'x/wx &g_serial_count\nx/32bx &g_serial_dead\nx/32bx &g_serial_fail\n' +
                'x/64gx &g_request\ndetach\nquit\n')
            try:
                run('failure-gdb', ['gdb', '-q', '-batch', '-x', str(failure_script), str(candidate / 'kernel.elf')])
            except BaseException as diagnostic_error:
                result['diagnostic_error'] = str(diagnostic_error)
    finally:
        try: run('stop', ['scripts/qemu-agent.sh', 'stop'])
        except BaseException as error: result['cleanup_error'] = str(error); result['status'] = 'FAIL'
        if args.serial_backend == 'file': stopped.set()
        collector.join(timeout=5)
        stopped.set()
        collector.join(timeout=2)
        listener.close()
        if serial_socket.exists(): serial_socket.unlink()
        if collector_error or collector.is_alive():
            result['collector_error'] = collector_error; result['status'] = 'FAIL'
        if not collector.is_alive() and (runtime / 'serial-receipts.bin').is_file():
            offset = 0
            with (runtime / 'serial-receipts.bin').open('rb') as events, \
                    (evidence / 'serial-observations.jsonl').open('x') as output:
                while header := events.read(12):
                    E.require(len(header) == 12, 'truncated serial receipt header')
                    received, length = struct.unpack('<QI', header)
                    data = events.read(length)
                    E.require(0 < length <= 64 and len(data) == length, 'truncated serial receipt bytes')
                    output.write(json.dumps({'offset': offset, 'length': length,
                        'monotonic_ns': received,
                        'bytes_base64': base64.b64encode(data).decode()}) + '\n')
                    offset += length
            E.require(offset == serial_path.stat().st_size, 'serial receipt size differs')
        for name in ('qemu-serial.log', 'qemu-debugcon.log', 'qemu-trace.log', 'launch.env'):
            if (runtime / name).is_file(): shutil.copy2(runtime / name, evidence / name)
        if args.production:
            (evidence / 'qemu').mkdir(exist_ok=True)
            for path in runtime.glob('*armed*'):
                if path.is_file(): shutil.copy2(path, evidence / 'qemu' / path.name)
        result['cleanup'] = not (runtime / 'qemu.pid').exists()
        result['finished_ns'] = time.monotonic_ns()
        result['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        E.write(evidence / 'result.json', result)
    print(json.dumps(result))
    return 0 if result['status'] == 'COLLECTED' and result['cleanup'] else 1


if __name__ == '__main__':
    sys.exit(main())
