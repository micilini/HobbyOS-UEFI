#!/usr/bin/env python3
"""One predeclared observer diagnostic, never a final timing repetition."""
import argparse
import datetime
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid
import memory_evidence as E
import irq_latency_host_authority as H
import irq_latency_authority as A
import irq_latency_scheduling as S
import irq_latency_evidence as V
import irq_latency_workload as W
import irq_latency_production as P
import qemu_hmp

SYMBOLS = ('irq_latency_save', 'irq_latency_restore', 'irq_external_common_entry',
           'irq_latency_interrupt_enter', 'irq_latency_interrupt_exit', 'irq_latency_irq_probe',
           'irq_latency_lock_wait', 'irq_latency_lock_acquired', 'irq_latency_lock_ticket',
           'irq_latency_record_drain', 'spin_lock_irqsave', 'switch_context', 'exc_isr2')


def prepare(session):
    p = session/'experiments/token-observer'
    if p.exists() and not (p/'plan.json').exists() and not (p/'runtime').exists():
        failed=p.with_name('token-observer-preparation-failed')
        E.require(not failed.exists(),'duplicate preparation failure; inspect before continuing')
        p.rename(failed)
        E.write(failed/'failure.json',{'status':'PREPARATION_FAILED','new_boots':0,
            'reason':'PermissionError reading tracefs instances; no tracing or host setting changed'})
    p.mkdir(exist_ok=False)
    E.require(shutil.disk_usage(p).free > 4*1024**3+512*1024**2, 'BLOCKED_STORAGE diagnostic reserve')
    for name in ('kernel.elf', 'hobbyos.img'):
        shutil.copy2(E.ROOT/name, p/name)
    sources = p/'source'; sources.mkdir()
    tracked = subprocess.check_output(['git','ls-files','-z'],cwd=E.ROOT).decode().split('\0')
    added = subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=E.ROOT).decode().split('\0')
    for name in sorted(set(tracked+added)-{''}):
        src=E.ROOT/name
        if src.suffix in E.GENERATED_SUFFIXES:continue
        E.require(src.is_file() and not src.is_symlink(), 'experiment source regular path')
        target=sources/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,target)
    for name,harness in [('build',False),('harness',True)]: E.write(p/(name+'-manifest.json'),E.manifest(harness=harness))
    old=E.ROOT/'backups/20261005T141415.805240Z-memory-irq-latency'
    original=json.loads((old/'evidence/production-code-identity/result.json').read_text())
    baseline=session/'evidence/portable-production/baseline'
    production=P.compare(baseline,sources,original,p/'production-pairs')
    assembly=[]
    for role,elf in [('previous',old/'candidates/components/kernel.elf'),('observer',p/'kernel.elf')]:
        for symbol in SYMBOLS:
            command=['objdump','-d','--disassemble='+symbol,str(elf)]
            r=subprocess.run(command,capture_output=True,check=True)
            target=p/(role+'-'+symbol+'.asm');target.write_bytes(r.stdout)
            assembly.append({'role':role,'symbol':symbol,'argv':command,'sha256':E.sha(target),
                             'cpuid_instruction_lines':sum('cpuid' in line for line in r.stdout.decode().splitlines())})
    trace=[]
    for root in ('/sys/kernel/tracing','/sys/kernel/debug/tracing'):
        path=Path(root)/'instances'
        try:
            exists=path.exists()
            trace.append({'path':str(path),'exists':exists,'readable':os.access(path,os.R_OK),
                          'writable':os.access(path,os.W_OK),'session_created':False})
        except OSError as error:
            trace.append({'path':str(path),'status':'UNAVAILABLE','errno':error.errno,'session_created':False})
    plan={'schema':1,'role':'OBSERVER_CAUSAL_DIAGNOSTIC_NOT_ACCEPTANCE',
        'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'hypothesis':'One redundant CPUID in continuously masked ticket acquisition contributes observer work; its removal need not remove the pre-handler enclosure outlier.',
        'candidate':{'elf_sha256':E.sha(p/'kernel.elf'),'image_sha256':E.sha(p/'hobbyos.img'),
                     'build_identity':E.identity(E.manifest()),'harness_identity':E.identity(E.manifest(harness=True)),
                     'source_inventory':P.source_snapshot(sources)},
        'held_constant':['q35','kvm','8 vCPUs','2 GiB','default placement','16 allocator workers',
                         '1024 operations per worker','all original stress phases','50000 ns strict budget',
                         '2 times pre-stress envelope','all 64 windows per CPU and 4 probes per window'],
        'deliberate_observer_change':'Generation-bound identity token between adjacent lock wait/acquired hooks, IF=0; production algorithms unchanged.',
        'new_observation':'Before/during/after process/thread lifetimes, bounded collection times, scheduling and cgroup pressure deltas.',
        'run_count':1,'retries':0,'commands':['irqlatency begin','mem','irqlatency stress','irqlatency finish'],
        'discriminators':['Emitted acquired hook has no CPUID; previous hook has one.',
                          'Retain every eligible and ineligible probe with leading/interior/trailing split.',
                          'Compare all CPU pre/post envelopes against their own frozen allowance.',
                          'Aggregate host execution/runqueue/throttling counters cannot identify one guest pause.'],
        'alternatives_unproven':['host descheduling','VM exits','SMI/NMI','physical CPU migration','cache effects'],
        'stop':'Exactly one boot, or structured activation/stress/transport/fault failure. No restart. Stop owned VM in finally.',
        'classification':'Always diagnostic; observer failure remains BLOCKED_TIMING_AUTHORITY; no green streak.',
        'storage':{'guest_ram_bytes':2*1024**3,'durable_budget_bytes':256*1024**2,
                   'free_bytes':shutil.disk_usage(p).free,'reserve_bytes':4*1024**3+64*1024**2},
        'ownership':str(p/'runtime'),'production_pairs':len(production['rows']),
        'host_tracing':{'interfaces':trace,'collected':False,'reason':'No owned permitted host trace established; no settings changed.'},
        'assembly':assembly,'vm_id':str(uuid.uuid4())}
    E.write(p/'plan.json',plan)
    (p/'plan.sha256').write_text(E.sha(p/'plan.json')+'\n')
    print(json.dumps({'plan':str(p/'plan.json'),'sha256':E.sha(p/'plan.json'),'candidate':plan['candidate']['elf_sha256']}),flush=True)


def run(session):
    p=session/'experiments/token-observer';plan=json.loads((p/'plan.json').read_text())
    E.require(E.sha(p/'plan.json')==(p/'plan.sha256').read_text().strip(),'experiment plan changed')
    E.require(E.sha(p/'kernel.elf')==plan['candidate']['elf_sha256'] and
              E.sha(p/'hobbyos.img')==plan['candidate']['image_sha256'], 'experiment candidate changed')
    successor=p/'transport-successor.json'
    expected_harness=plan['candidate']['harness_identity']
    if successor.exists():
        repair=json.loads(successor.read_text())
        E.require(repair['prior_plan_sha256']==E.sha(p/'plan.json') and
                  repair['driver_sha256']==E.sha(Path(__file__)), 'transport successor binding')
        expected_harness=repair['harness_identity']
    E.require(E.identity(E.manifest(harness=True))==expected_harness,'experiment harness changed')
    directory=p/'runtime';directory.mkdir(exist_ok=False)
    E.require(not H.snapshot(8)['qemu_processes'],'experiment concurrent VM')
    # The common QEMU harness opens its drive writable. Give it owned scratch,
    # never the predeclared immutable candidate image.
    working_image=directory/'working.img'
    shutil.copy2(p/'hobbyos.img',working_image)
    E.require(E.sha(working_image)==plan['candidate']['image_sha256'],'experiment working image copy')
    E.write(directory/'working-image-before.json',{'source_sha256':plan['candidate']['image_sha256'],
        'working_sha256':E.sha(working_image),'path':str(working_image)})
    env=dict(os.environ,QEMU_RUNTIME=str(directory.relative_to(E.ROOT)),ACCEL='kvm',SMP='8',MACHINE='q35',MEM='2G',
             QEMU_LAUNCH_RECEIPT=str(directory/'launch.json'),QEMU_VM_ID=plan['vm_id'],
             HOBBYOS_IMAGE=str(working_image),HOBBYOS_KERNEL=str(p/'kernel.elf'),PYTHONDONTWRITEBYTECODE='1')
    events=[];sock=str((directory/'hmp.sock').relative_to(E.ROOT))
    E.require(len(sock.encode())<108,'transport socket path length')
    def record(**row):
        events.append(dict(row,monotonic_ns=time.monotonic_ns()))
        with (directory/'driver.jsonl').open('a') as out:out.write(json.dumps(events[-1])+'\n')
        print(json.dumps(events[-1]),flush=True)
    def wait(marker,limit):
        end=time.monotonic()+limit
        while time.monotonic()<end:
            raw=(directory/'qemu-serial.log').read_text(errors='replace')
            matches=[line for line in raw.splitlines() if marker in line]
            if matches:record(marker=marker,line=matches[-1]);return matches[-1]
            E.require('[PANIC]' not in raw,'GUEST_FAULT before '+marker)
            time.sleep(.2)
        raise TimeoutError('TRANSPORT_OR_COMMAND_TIMEOUT '+marker)
    def command(value):
        record(command=value,event='HMP_INPUT_BEGIN')
        for ch in value:
            qemu_hmp.command(sock,'sendkey '+qemu_hmp.keyname(ch)+' 30');time.sleep(.10)
        qemu_hmp.command(sock,'sendkey ret 30');record(command=value,event='HMP_INPUT_SENT')
    started=False;pid=None
    try:
        E.write(directory/'host-before.json',H.snapshot(8))
        started=True
        with (directory/'start.log').open('w') as f:
            subprocess.run(['scripts/qemu-agent.sh','start'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
        wait('[RUNTIME_READY]',60)
        pid=int((directory/'qemu.pid').read_text())
        E.write(directory/'host-running.json',H.snapshot(8,pid))
        command('irqlatency begin');line=wait('[IRQ_LATENCY][BEGIN]',150)
        E.write(directory/'host-after-activation.json',H.snapshot(8,pid))
        E.require('status=ACTIVE' in line,'COMMAND_STRUCTURED activation refused')
        command('mem');wait('[MEM][SUMMARY]',30)
        command('irqlatency stress');wait('[IRQ_LATENCY][STRESS]',300)
        E.write(directory/'host-after-stress.json',H.snapshot(8,pid))
        command('irqlatency finish');wait('[IRQ_LATENCY][CALIBRATION_END]',180)
        E.write(directory/'host-terminal.json',H.snapshot(8,pid))
        record(driver='COMPLETED',acceptance=False)
    except Exception as error:
        if pid is not None and not (directory/'host-terminal.json').exists():
            try:E.write(directory/'host-terminal.json',H.snapshot(8,pid))
            except Exception as unavailable:
                E.write(directory/'host-terminal-unavailable.json',{
                    'status':'UNAVAILABLE','errno':getattr(unavailable,'errno',None),'reason':str(unavailable)})
        record(driver='FAILED',error=str(error),acceptance=False);raise
    finally:
        if started:
            with (directory/'stop.log').open('w') as f:
                stopped=subprocess.run(['scripts/qemu-agent.sh','stop'],env=env,stdout=f,stderr=subprocess.STDOUT)
            record(stop_exit=stopped.returncode)
        E.write(directory/'working-image-after.json',{'sha256':E.sha(working_image),
            'source_sha256':E.sha(p/'hobbyos.img'),'stopped':not H.snapshot(8)['qemu_processes']})
        E.require(E.sha(p/'hobbyos.img')==plan['candidate']['image_sha256'],'experiment immutable source image changed')


def repair_transport(session):
    p=session/'experiments/token-observer';d=p/'runtime'
    E.require('is too long' in (d/'qemu-trace.log').read_text() and
              (d/'qemu-serial.log').stat().st_size==0 and (d/'qemu-debugcon.log').stat().st_size==0,
              'transport repair requires no guest execution')
    E.require(not H.snapshot(8)['qemu_processes'],'transport repair VM still active')
    original=json.loads((p/'plan.json').read_text())
    E.require(E.identity(E.manifest())==original['candidate']['build_identity'],'transport repair linked inputs changed')
    d.rename(p/'runtime-transport-failed')
    shutil.copy2(Path(__file__),p/'transport-driver.py')
    E.write(p/'transport-successor.json',{'schema':1,'reason':'UNIX_SOCKET_PATH_TOO_LONG_BEFORE_GUEST_BOOT',
        'prior_plan_sha256':E.sha(p/'plan.json'),'driver_sha256':E.sha(Path(__file__)),
        'harness_identity':E.identity(E.manifest(harness=True)),'harness_manifest':E.manifest(harness=True),
        'changed_driver_behavior':['root-relative owned socket path under 108 bytes','append-only event log'],
        'guest_boots_before':0,'remaining_guest_boots':1,'unchanged_candidate':original['candidate']['elf_sha256'],
        'timing_retry':False})
    print('TRANSPORT_REPAIRED guest_boots_before=0 remaining_guest_boots=1')


def analyze(session):
    p=session/'experiments/token-observer';d=p/'runtime'
    serial=(d/'qemu-serial.log').read_text();raw=V.records(serial)
    if raw['BEGIN'][0]['status']!='ACTIVE':
        result=V.calibration_diagnostic(raw)
        E.write(p/'admission-diagnostic.json',result)
        host=json.loads((d/'host-running.json').read_text())
        E.write(p/'admission-capacity.json',A.verify_snapshot(host,json.loads((d/'launch.json').read_text())))
        E.write(p/'admission-limits.json',{'status':'BLOCKED_TIMING_AUTHORITY','boot_count':1,
            'host_after_failure':'NOT_CAPTURED','host_scheduling_deltas':'UNAVAILABLE_MISSING_TERMINAL_SNAPSHOT',
            'memory_path_maxima':'NOT_MEASURED_ACTIVATION_REFUSED','post_stress':'NOT_RUN',
            'original_spike_cause':'NOT_ESTABLISHED','guest_event_host_correlation':'NOT_ESTABLISHED',
            'serial_sha256':E.sha(d/'qemu-serial.log'),'candidate_elf_sha256':E.sha(p/'kernel.elf')})
        print(json.dumps({'status':result['status'],'begin_status':result['begin_status'],
                          'cpus':result['cpus']}),flush=True)
        return
    result=V.inspect(serial)
    hosts=[json.loads((d/name).read_text()) for name in ('host-running.json','host-after-stress.json','host-terminal.json')]
    E.write(p/'host-capacity.json',A.verify_series(hosts,json.loads((d/'launch.json').read_text())))
    E.write(p/'host-scheduling.json',S.verify(hosts));E.write(p/'raw-result.json',result)
    E.write(p/'workload.json',W.verify(raw,8))
    probes=[]
    for r in raw['OVERHEAD']:
        q={k:int(r[k]) for k in ('role','index','probe','cpu','valid','outer_begin','inner_begin','inner_end','outer_end')}
        q.update(leading=q['inner_begin']-q['outer_begin'],interior=q['inner_end']-q['inner_begin'],
                 trailing=q['outer_end']-q['inner_end'])
        probes.append(q)
    E.write(p/'probe-components.json',probes)
    print(json.dumps(result),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('prepare','run','analyze','repair-transport'))
    parser.add_argument('--session',type=Path,required=True);args=parser.parse_args()
    {'prepare':prepare,'run':run,'analyze':analyze,'repair-transport':repair_transport}[args.mode](args.session.resolve())
