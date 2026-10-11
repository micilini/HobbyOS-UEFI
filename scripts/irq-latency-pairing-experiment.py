#!/usr/bin/env python3
"""Finite, candidate-bound calibration experiment and terminal-safe driver."""
import hashlib
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
import irq_latency_pairs as P
import irq_latency_workload as W
import irq_latency_production as PRODUCTION
import qemu_hmp
import irq_latency_transport as K
import irq_latency_terminal as L
import irq_latency_entry as J
import irq_latency_interior as I
import clocksource_evidence as CLOCKSOURCE
import irq_latency_writeback as B
import irq_latency_writeback_clock as Q
import request_local_cycles as CYCLES
import irq_latency_heap as F
import irq_latency_ram_backend as RAM
import irq_latency_qemu_trace as TRACE
import validation_resources as RESOURCES
import tlb_proof_evidence as PROOF

OBSERVER_UPDATES = 'nested-observer-precheck-v2'
MMIO_IDENTITY = 'continuous-mask-mmio-identity-v1'
DETAILS_RETAINED = 'bounded-details-v1'
DETAILS_OMITTED = 'mandatory-maxima-only-v1'
PRIMITIVES = {0:'ordered-tsc-irq-envelope',1:'intel-local-tsc-irq-envelope-v1'}


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def terminal(directory,pid,snapshot=H.snapshot,cpus=8):
    """Never turn failure to observe the still-owned process into quiet counters."""
    if (directory/'host-terminal.json').exists():return
    try:
        if pid is None:raise ValueError('owned VM PID unavailable before terminal capture')
        E.write(directory/'host-terminal.json',snapshot(cpus,pid))
    except Exception as error:
        E.write(directory/'host-terminal-unavailable.json',dict(status='UNAVAILABLE',reason=str(error),
            errno=getattr(error,'errno',None),pid=pid))


def lifecycle(directory,body,get_pid,stop,snapshot=H.snapshot,cpus=8):
    try:return body()
    finally:
        try:terminal(directory,get_pid(),snapshot,cpus)
        finally:stop()


def timestamp_method(build):
    info,receipt=build['candidate'],build['receipt']
    E.require(info['build_receipt_sha256']==E.identity(receipt) and receipt['status']=='PASS' and
        receipt['artifacts']==info['artifacts'] and
        receipt['inputs_before']==receipt['build_input_sha256']==info['source_sha256'],
        'timestamp candidate/build receipt binding')
    claims=receipt['commands'];commands=build['commands']
    E.require(len(claims)==len(commands) and bool(claims) and
        len({r['receipt'] for r in claims})==len(claims),'timestamp build command inventory')
    for claim,row in zip(claims,commands):
        E.require(claim['receipt_sha256']==E.identity(row) and row['exit_status']==0 and
            row['build_input_sha256']==info['source_sha256'],'timestamp compiled command binding')
    orders={int(next((arg.split('=',1)[1] for arg in row['argv'] if arg.startswith('IRQ_LOCAL_TSC_ORDER=')),'0')) for row in commands}
    E.require(len(orders)==1 and orders<=set(PRIMITIVES),'candidate timestamp ordering flags')
    return PRIMITIVES[next(iter(orders))]


def prepare(directory,candidate,role='DIAGNOSTIC_NO_ACCEPTANCE',boots=1,entry_diagnostic=False,ram_prealloc=None,qemu_exit_trace=False,qemu_counter_trace=False,proof_policy='deferred'):
    E.require(not qemu_exit_trace,'current request cycles cannot use the retired HPET trace join')
    E.require(not qemu_counter_trace or entry_diagnostic and role=='DIAGNOSTIC_NO_ACCEPTANCE' and ram_prealloc is None,
              'counter tracing requires current diagnostic candidate and ordinary RAM')
    E.require(role in ('DIAGNOSTIC_NO_ACCEPTANCE','CURRENT_TIMING_QUALIFICATION','CURRENT_FUNCTIONAL_STRESS'),'experiment role')
    E.require(boots==(3 if role=='CURRENT_TIMING_QUALIFICATION' else 1),'finite boot plan')
    E.require(not directory.exists(),'experiment cannot overwrite a previous plan')
    E.require(shutil.disk_usage(directory.parent).free>4*1024**3+64*1024**2,'BLOCKED_STORAGE experiment reserve')
    directory.mkdir()
    info=json.loads((candidate/'candidate.json').read_text())
    E.require(not entry_diagnostic or role=='DIAGNOSTIC_NO_ACCEPTANCE','entry checkpoint role cannot supply acceptance')
    E.require((info.get('profile')=='irq-entry-diagnostic')==entry_diagnostic,'entry checkpoint candidate role')
    detail_policy=DETAILS_RETAINED
    receipt=json.loads((candidate/'build-receipt.json').read_text())
    build=dict(candidate=info,receipt=receipt,
        commands=[json.loads((candidate/r['receipt']).read_text()) for r in receipt['commands']])
    primitive=timestamp_method(build)
    E.write(directory/'timestamp-build.json',build)
    if entry_diagnostic:
        receipt=json.loads((candidate/'build-receipt.json').read_text())
        commands=[json.loads((candidate/r['receipt']).read_text())['argv'] for r in receipt['commands']]
        E.require(any('kernel-check' in cmd for cmd in commands) and any('image' in cmd for cmd in commands) and
            all({'SELFTEST=1','IRQ_LATENCY=1','IRQ_ENTRY_DIAGNOSTIC=1'}<=set(cmd) for cmd in commands),
            'diagnostic candidate actual build flags')
        detail_modes={next((arg.split('=',1)[1] for arg in cmd if arg.startswith('IRQ_LATENCY_DETAILS=')),'1') for cmd in commands}
        E.require(len(detail_modes)==1 and detail_modes<={'0','1'},'diagnostic detail flag consistency')
        detail_policy=DETAILS_OMITTED if detail_modes=={'0'} else DETAILS_RETAINED
    module('memory-observation-gate').verify_local_candidate(candidate)
    E.require(info['source_sha256']==E.identity(E.manifest()),'experiment linked source')
    source=directory/'source';source.mkdir()
    names=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=E.ROOT).decode().split('\0')
    for name in sorted(set(names)-{''}):
        if Path(name).suffix in E.GENERATED_SUFFIXES:continue
        src=E.ROOT/name;E.require(src.is_file() and not src.is_symlink(),'source regular file')
        dst=source/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        dst.chmod(0o755 if src.stat().st_mode&0o111 else 0o644)
    cpus=24 if role=='CURRENT_FUNCTIONAL_STRESS' else 8
    ordinary=['mem'] if role=='DIAGNOSTIC_NO_ACCEPTANCE' else module('verify-device-mapping').COMMANDS
    proof=dict(method=PROOF.CONTRACT,policy=proof_policy,session=uuid.uuid4().hex[:16],
        invocations=1+sum(p in ('tlbtest','tlbtest run') for p in ordinary))
    plan=dict(schema=2,terminal_contract=L.CONTRACT,transport_contract=K.CONTRACT,role=role,utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        candidate=str(candidate.resolve()),candidate_id=E.identity(info),
        elf_sha256=info['artifacts']['kernel.elf']['sha256'],image_sha256=info['artifacts']['hobbyos.img']['sha256'],
        build_identity=E.identity(E.manifest()),harness_identity=E.identity(E.manifest(harness=True)),
        source_inventory=PRODUCTION.source_snapshot(source),
        entry_contract=J.CONTRACT if entry_diagnostic else None,
        identity_contract='stack-mask-identity-v2',entry_admission_contract='entry-admission-cookie-v1',
        observer_update_contract=OBSERVER_UPDATES,
        mmio_identity_contract=MMIO_IDENTITY,
        detail_policy=detail_policy,
        interior_contract=I.CONTRACT if entry_diagnostic else None,
        clocksource_contract=CLOCKSOURCE.CONTRACT,
        clock_body_contract=I.CLOCK_CONTRACT if entry_diagnostic else None,
        tick_body_contract=I.TICK_CONTRACT if entry_diagnostic else None,
        writeback_contract=B.CONTRACT if entry_diagnostic else None,
        request_timing_contract=CYCLES.CONTRACT,
        stack_coverage_contract=W.STACK_COVERAGE,
        heap_contract=F.CONTRACT if entry_diagnostic else None,
        heap_operation_contract=F.OPERATION_CONTRACT if entry_diagnostic else None,
        ram_backend=None if ram_prealloc is None else dict(method=RAM.CONTRACT,prealloc=ram_prealloc,size=RAM.GUEST_BYTES,object='diagnostic.ram'),
        qemu_exit_trace=TRACE.declaration(TRACE.COUNTER_CONTRACT) if qemu_counter_trace else None,
        pairing=json.loads((E.ROOT/'scripts/irq-latency-pairing-contract.json').read_text()),
        pairing_contract_sha256=E.sha(E.ROOT/'scripts/irq-latency-pairing-contract.json'),
        observer_contract_sha256=E.sha(E.ROOT/'scripts/irq-latency-contract.json'),timestamp_primitive=primitive,
        timestamp_build_sha256=E.sha(directory/'timestamp-build.json'),
        proof_output=proof,
        commands=[f"irqlatency proof {proof['policy']} {proof['session']} {proof['invocations']}",
            'irqlatency begin',*ordinary,'irqlatency stress','irqlatency finish','irqlatency audit','irqlatency export'],
        boots=boots,retries=0,vm_ids=[str(uuid.uuid4()) for _ in range(boots)],
        profile=dict(machine='q35',accel='kvm',smp=cpus,memory='2G',placement='default'),
        stop='First failed required predicate; never repeat an unchanged failed candidate.',
        hypothesis=('The additive entry and return timestamps subdivide the original enclosing margins; all four original endpoints and the frozen allowance remain unchanged.' if entry_diagnostic else 'Complete current role and terminal qualification with frozen CPU-local pairing.'),
        diagnostic_discriminator=('Compare each original maximum with its exact entry and return checkpoints; retain every original eligible and ineligible probe and both residual regions.' if entry_diagnostic else None),
        interpretation='Host counters are aggregate; no unique attribution of a microsecond guest event.',
        hardware=False)
    PROOF.contract(plan)
    RAM.contract(plan)
    TRACE.contract(plan)
    if qemu_counter_trace:
        plan['hypothesis']='Existing qualified LAPIC counter samples identify the corresponding QEMU HPET read events during the complete stress command.'
        plan['diagnostic_discriminator']='Join the eight same-event maximum counter samples by CPU and successful high/low/high value; retain missing matches, callback intervals, trace perturbation and unresolved interference.'
    E.write(directory/'plan.json',plan);(directory/'plan.sha256').write_text(E.sha(directory/'plan.json')+'\n')
    return plan


def bound_plan(directory):
    plan=json.loads((directory/'plan.json').read_text())
    E.require(plan.get('stack_coverage_contract')==W.STACK_COVERAGE,'current deterministic stack coverage required')
    PROOF.contract(plan)
    E.require(plan.get('timestamp_primitive') in PRIMITIVES.values(),'declared source-bound timestamp primitive')
    build=json.loads((directory/'timestamp-build.json').read_text())
    E.require(E.sha(directory/'timestamp-build.json')==plan['timestamp_build_sha256'] and
        E.identity(build['candidate'])==plan['candidate_id'],'timestamp build/candidate identity')
    E.require(plan['timestamp_primitive']==timestamp_method(build),'plan primitive differs from compiled candidate')
    E.require(plan.get('detail_policy') in (DETAILS_RETAINED,DETAILS_OMITTED) and
        (plan['detail_policy']==DETAILS_RETAINED or plan.get('entry_contract')==J.CONTRACT and
         plan['role']=='DIAGNOSTIC_NO_ACCEPTANCE'),'detail omission is diagnostic only')
    E.require(plan['schema']==2 and plan.get('identity_contract')=='stack-mask-identity-v2' and plan.get('entry_admission_contract')=='entry-admission-cookie-v1' and plan.get('observer_update_contract')==OBSERVER_UPDATES and plan.get('mmio_identity_contract')==MMIO_IDENTITY and plan.get('entry_contract') in (None,J.CONTRACT) and
        (not plan.get('entry_contract') or plan['role']=='DIAGNOSTIC_NO_ACCEPTANCE' and plan.get('interior_contract')==I.CONTRACT and plan.get('writeback_contract')==B.CONTRACT and plan.get('heap_contract')==F.CONTRACT) and plan['terminal_contract']==L.CONTRACT and plan['transport_contract']==K.CONTRACT,'experiment integration contract')
    E.require(plan.get('request_timing_contract')==CYCLES.CONTRACT and 'writeback_clock_contract' not in plan and
        (not plan.get('qemu_exit_trace') or plan['qemu_exit_trace'].get('method')==TRACE.COUNTER_CONTRACT),'current request timing source contract')
    RAM.contract(plan)
    TRACE.contract(plan)
    E.require(not plan.get('entry_contract') or plan.get('heap_operation_contract')==F.OPERATION_CONTRACT,'current isolated operation source contract')
    E.require(plan.get('clocksource_contract')==CLOCKSOURCE.CONTRACT,'current production clocksource contract')
    E.require(plan.get('clocksource_expected') in (None,1,2),'unknown required clock provider')
    E.require(plan.get('clock_body_contract')==(I.CLOCK_CONTRACT if plan.get('entry_contract') else None),'current clock body source contract')
    E.require(plan.get('tick_body_contract')==(I.TICK_CONTRACT if plan.get('entry_contract') else None),'current tick body source contract')
    E.require(E.sha(directory/'plan.json')==(directory/'plan.sha256').read_text().strip(),'frozen plan changed')
    E.require(plan['build_identity']==E.identity(E.manifest()) and
              plan['harness_identity']==E.identity(E.manifest(harness=True)),'experiment current inputs differ')
    E.require(plan['pairing']==json.loads((E.ROOT/'scripts/irq-latency-pairing-contract.json').read_text()),'experiment method differs')
    E.require(PRODUCTION.source_snapshot(directory/'source')==plan['source_inventory'],'immutable source changed')
    return plan


def run(directory,index=0):
    plan=bound_plan(directory);cpus=plan['profile']['smp'];E.require(0<=index<plan['boots'],'undeclared boot')
    if index:
        E.require(analyze(directory,index-1)['qualified'],'failed previous boot cannot be retried')
    target=directory/('boot-'+str(index));E.require(not target.exists(),'boot identity already consumed')
    E.require(not H.snapshot(cpus)['qemu_processes'],'another VM owns host runtime')
    # Owned short absolute sockets. Evidence is moved only after the VM stops.
    runtime=E.ROOT/'backups'/('cal-'+plan['vm_ids'][index][:12]);runtime.mkdir(exist_ok=False)
    target.mkdir();E.write(target/'runtime-owner.json',dict(path=str(runtime),vm_id=plan['vm_ids'][index]))
    candidate=Path(plan['candidate']);working=runtime/'working.img'
    E.require(E.sha(candidate/'kernel.elf')==plan['elf_sha256'] and
              E.sha(candidate/'hobbyos.img')==plan['image_sha256'],'canonical experiment binary differs')
    RESOURCES.sparse_copy(candidate/'hobbyos.img',working)
    K.prepare(runtime,candidate/'kernel.elf')
    env=dict(os.environ,QEMU_RUNTIME=str(runtime.relative_to(E.ROOT)),ACCEL='kvm',SMP=str(cpus),MACHINE='q35',MEM='2G',
        QEMU_LAUNCH_RECEIPT=str(runtime/'launch.json'),QEMU_VM_ID=plan['vm_ids'][index],
        HOBBYOS_IMAGE=str(working),HOBBYOS_KERNEL=str(candidate/'kernel.elf'),PYTHONDONTWRITEBYTECODE='1')
    backend=RAM.contract(plan)
    if backend:env.update(IRQ_DIAGNOSTIC_RAM_CONTRACT=RAM.CONTRACT,IRQ_DIAGNOSTIC_RAM_PREALLOC=backend['prealloc'])
    else:
        env.pop('IRQ_DIAGNOSTIC_RAM_CONTRACT',None);env.pop('IRQ_DIAGNOSTIC_RAM_PREALLOC',None)
    trace_contract=TRACE.contract(plan)
    if trace_contract:env['IRQ_DIAGNOSTIC_EXIT_TRACE']=trace_contract['method']
    else:env.pop('IRQ_DIAGNOSTIC_EXIT_TRACE',None)
    sock=str((runtime/'hmp.sock').relative_to(E.ROOT));pid=None;started=False
    def record(**row):
        row['monotonic_ns']=time.monotonic_ns()
        with (runtime/'driver.jsonl').open('a') as stream:stream.write(json.dumps(row)+'\n')
        print(json.dumps(row),flush=True)
    def wait(marker,seconds):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            serial=(runtime/'qemu-serial.log').read_text(errors='replace')
            serial=serial[:serial.rfind('\n')+1]
            lines=[line for line in serial.splitlines() if marker in line]
            if lines:record(marker=marker,line=lines[-1]);return lines[-1]
            E.require('[PANIC]' not in serial,'GUEST_FAULT before '+marker)
            time.sleep(.2)
        raise TimeoutError('TRANSPORT_OR_SHELL_TIMEOUT '+marker)
    sequence=0;transport=module('foundation-qemu')
    def command(value):
        nonlocal sequence
        sequence+=1
        crc,frame=transport.build_command_frame(sequence,value)
        if value=='irqlatency export':
            prefix=(runtime/'qemu-debugcon.log').read_bytes()
            E.write(runtime/'proof-export-prefix.json',dict(bytes=len(prefix),sha256=hashlib.sha256(prefix).hexdigest(),candidate_id=plan['candidate_id'],vm_id=plan['vm_ids'][index],sequence=sequence,monotonic_ns=time.monotonic_ns()))
        record(command=value,event='HMP_INPUT_BEGIN',sequence=sequence)
        K.capture(runtime,plan,index,sequence,value)
        record(command=value,event='KEYBOARD_INPUT_VERIFIED',sequence=sequence)
        wait('[HARNESS][END] seq='+str(sequence)+' ',360)
        row=framed_command_result(plan,runtime,index,sequence,value)
        with (runtime/'framed.jsonl').open('a') as stream:stream.write(json.dumps(row)+'\n')
        return row['handler_status']
    def body():
        nonlocal pid,started
        before=H.snapshot(cpus);E.write(runtime/'host-before.json',before)
        if backend:E.write(runtime/'ram-admission.json',RAM.admit(before))
        record(event='QEMU_START_BEGIN');started=True
        with (runtime/'start.log').open('w') as stream:
            subprocess.run(['scripts/qemu-agent.sh','start'],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
        pid=int((runtime/'qemu.pid').read_text());record(event='QEMU_START_RETURNED',pid=pid)
        if backend:
            launched=json.loads((runtime/'launch.json').read_text())
            observation=RAM.observe(launched['argv']);E.write(runtime/'ram-backend.json',observation)
            RAM.verify_backend(plan,launched,observation)
            E.write(runtime/'host-started.json',H.snapshot(cpus,pid))
        wait('[RUNTIME_READY]',60);E.write(runtime/'host-running.json',H.snapshot(cpus,pid))
        E.write(runtime/'clocksource-boot.json',CLOCKSOURCE.boot((runtime/'qemu-serial.log').read_text(),cpus,plan.get('clocksource_expected')))
        E.require(command(plan['commands'][0])==0,'COMMAND_STRUCTURED proof support setup failed')
        activation=command('irqlatency begin');line=wait('[IRQ_LATENCY][BEGIN]',180)
        E.write(runtime/'host-after-activation.json',H.snapshot(cpus,pid))
        E.require(activation==0 and 'status=ACTIVE' in line,'COMMAND_STRUCTURED activation refused')
        raw=V.records((runtime/'qemu-serial.log').read_text());_,pre=P.role_result(raw,0,cpus)
        E.require(pre['valid'],'COMMAND_STRUCTURED independent pre-calibration refused')
        for value in plan['commands'][2:-4]:
            E.require(command(value)==0,'COMMAND_STRUCTURED ordinary command failed: '+value)
        trace_window=TRACE.Window(runtime,plan,index,transport.QmpInputClient) if trace_contract else None
        if trace_window:trace_window.begin()
        try:
            command('irqlatency stress');line=wait('[IRQ_LATENCY][STRESS]',300)
        finally:
            if trace_window:trace_window.finish()
        E.write(runtime/'host-after-stress.json',H.snapshot(cpus,pid))
        # Finish even when stress reports failure: retain postcheck and terminal maxima.
        command('irqlatency finish');wait('[IRQ_LATENCY][CALIBRATION_END]',180)
        E.require(command('irqlatency audit')==0,'COMMAND_STRUCTURED terminal service audit failed')
        E.require(command('irqlatency export')==0,'COMMAND_STRUCTURED immutable proof export failed')
        record(driver='COMPLETED',implementation_acceptance=False)
    def stop():
        if started:
            with (runtime/'stop.log').open('w') as stream:
                stopped=subprocess.run(['scripts/qemu-agent.sh','stop'],env=env,stdout=stream,stderr=subprocess.STDOUT)
            record(stop_exit=stopped.returncode)
            E.require(stopped.returncode==0,'TRANSPORT owned VM stop failed')
        E.require(not H.snapshot(cpus)['qemu_processes'],'owned runtime remains active')
        files=[dict(path=p.name,sha256=E.sha(p),bytes=p.stat().st_size) for p in runtime.iterdir()
               if p.is_file() and p.name!='working.img']
        E.write(runtime/'seal.json',dict(files=files,working_image_sha256=E.sha(working),
            original_image_sha256=plan['image_sha256'],retired_working_bytes=working.stat().st_size))
        working.unlink()
    try:lifecycle(runtime,body,lambda:pid,stop,H.snapshot,cpus)
    except Exception as error:
        E.write(target/'failure.json',dict(status='FAILED',reason=str(error),implementation_acceptance=False));raise
    finally:
        if not H.snapshot(cpus)['qemu_processes']:
            shutil.move(str(runtime),str(target/'runtime'))
        E.require(E.sha(candidate/'hobbyos.img')==plan['image_sha256'],'canonical image modified')


def observer_method(plan,raw):
    PROOF.contract(plan)
    E.require(len(raw['BEGIN'])==1 and raw['BEGIN'][0].get('proof_output')=='2','current proof output source/method')
    E.require(plan.get('terminal_contract')==L.CONTRACT and len(raw['BEGIN'])==1 and
        raw['BEGIN'][0].get('closure')==L.CLOSURE,'current terminal calibration closure differs')
    E.require(plan.get('timestamp_primitive') in PRIMITIVES.values() and len(raw['BEGIN'])==1 and
        raw['BEGIN'][0].get('primitive')==plan['timestamp_primitive'],'current source timestamp primitive differs')
    E.require(plan.get('request_timing_contract')==CYCLES.CONTRACT and len(raw['BEGIN'])==1 and
        raw['BEGIN'][0].get('request_timing')==CYCLES.CONTRACT and
        'writeback_clock_contract' not in plan and 'writeback_clock_method' not in raw['BEGIN'][0] and
        not raw.get('CLOCK_CALL') and not raw.get('CLOCK_CALL_POINT'),'current request telemetry method differs')
    if plan.get('entry_contract'):
        E.require(plan.get('heap_operation_contract')==F.OPERATION_CONTRACT and len(raw['BEGIN'])==1 and
            raw['BEGIN'][0].get('heap_operation_method')==F.OPERATION_CONTRACT,'isolated operation collector/method differs')
        E.require(plan.get('clock_body_contract')==I.CLOCK_CONTRACT and
            raw['BEGIN'][0].get('clock_body_method')==I.CLOCK_CONTRACT,'clock body collector/method differs')
        E.require(plan.get('tick_body_contract')==I.TICK_CONTRACT and raw['BEGIN'][0].get('tick_body_method')==I.TICK_CONTRACT,'tick body collector/method differs')
    E.require(plan.get('identity_contract')=='stack-mask-identity-v2' and len(raw['BEGIN'])==1 and
        raw['BEGIN'][0].get('identity_method')==plan['identity_contract'] and
        plan.get('entry_admission_contract')=='entry-admission-cookie-v1' and
        raw['BEGIN'][0].get('entry_admission')==plan['entry_admission_contract'] and
        plan.get('observer_update_contract')==OBSERVER_UPDATES and
        raw['BEGIN'][0].get('observer_updates')==OBSERVER_UPDATES and
        plan.get('mmio_identity_contract')==MMIO_IDENTITY and
        raw['BEGIN'][0].get('mmio_identity')==MMIO_IDENTITY,'current observer identity/admission/update method differs')
    E.require(plan.get('detail_policy') in (DETAILS_RETAINED,DETAILS_OMITTED) and
        raw['BEGIN'][0].get('detail_policy')==plan['detail_policy'] and
        (plan['detail_policy']==DETAILS_RETAINED or plan.get('entry_contract')==J.CONTRACT and
         plan['role']=='DIAGNOSTIC_NO_ACCEPTANCE'),'current detail policy or role differs')
    if plan.get('entry_contract'):
        E.require(plan.get('writeback_contract')==B.CONTRACT and raw['BEGIN'][0].get('writeback_method')==B.CONTRACT,
                  'current writeback diagnostic method differs')
        E.require(plan.get('heap_contract')==F.CONTRACT and raw['BEGIN'][0].get('heap_method')==F.CONTRACT,
                  'current heap-stage method differs')
    else:
        E.require(not plan.get('clock_body_contract') and not plan.get('tick_body_contract') and raw['BEGIN'][0].get('writeback_method')==raw['BEGIN'][0].get('heap_method')==raw['BEGIN'][0].get('heap_operation_method')==raw['BEGIN'][0].get('clock_body_method')==raw['BEGIN'][0].get('tick_body_method')=='unavailable' and not any(raw.get(k) for k in ('TICK_BODY','TICK_BODY_POINT','TICK_SAMPLE','WRITEBACK','WRITEBACK_POINT','CLOCK_CALL','CLOCK_CALL_POINT','CLOCK_BODY','CLOCK_BODY_POINT','HEAP_STAGE','HEAP_POINT','HEAP_MARK','HEAP_OPERATION','HEAP_OPERATION_POINT')), 'undeclared current diagnostic method/records')
    return True


def observer_updates(raw,cpus):
    begin=raw['BEGIN']
    E.require(len(begin)==1 and V.number(begin[0],'cpus')==cpus and
        begin[0].get('observer_updates')==OBSERVER_UPDATES and begin[0].get('mmio_identity')==MMIO_IDENTITY,
        'observer update topology/method')
    epoch=V.number(begin[0],'epoch')
    states=raw.get('CPU',[]);rows=raw.get('NESTED_OBSERVER',[])
    E.require(len(states)==len(rows)==cpus and
        {V.number(r,'cpu') for r in states}==set(range(cpus)) and
        {V.number(r,'cpu') for r in rows}==set(range(cpus)), 'observer update ledger coverage')
    E.require(epoch>0 and all(V.number(r,'epoch')==epoch for r in states+rows),
        'observer update epoch')
    counts=[]
    for row in rows:
        count=V.number(row,'elided')
        reused=V.number(row,'mmio_reused')
        prechecked=V.number(row,'prechecked')
        E.require(0<=prechecked<=count<2**64-1 and 0<=reused<2**64-1,'observer update counter exhausted/inconsistent')
        counts.append(dict(cpu=V.number(row,'cpu'),elided=count,mmio_reused=reused,prechecked=prechecked))
    return dict(status='PASS',method=OBSERVER_UPDATES,mmio_identity=MMIO_IDENTITY,epoch=epoch,cpus=sorted(counts,key=lambda r:r['cpu']),
        authority='Diagnostic update accounting; no latency subtraction or acceptance.')


def optional_details(raw,cpus):
    policy=raw['BEGIN'][0].get('detail_policy')
    E.require(policy in (DETAILS_RETAINED,DETAILS_OMITTED),'unknown detail policy')
    states=V.index(raw['CPU'],('cpu',));rows=V.index(raw['MAXIMUM'],('cpu','path'))
    E.require(set(states)=={(c,) for c in range(cpus)} and
        set(rows)=={(c,p) for c in range(cpus) for p in range(14)},'detail mandatory inventory')
    if policy==DETAILS_OMITTED:
        E.require(not any(w['kind']=='failure' for w in raw['WITNESS']),'detail-off unexpected optional record')
        for (cpu,),r in states.items():
            omitted=V.number(r,'failure_overflow')
            violations=[V.number(rows[(cpu,p)],'violations') for p in range(11)]
            E.require(V.number(r,'failures')==0 and max(violations)<=omitted<=sum(violations)+V.number(r,'invalid'),
                'detail-off omission/count relationship')
    return dict(status='PASS',policy=policy,complete_optional_history=policy==DETAILS_RETAINED and
        all(V.number(r,'failure_overflow')==0 for r in states.values()),
        omitted=[dict(cpu=c,records=V.number(states[(c,)],'failure_overflow')) for c in range(cpus)],
        authority='Compulsory maxima/counts remain independently verified; no latency subtraction or acceptance.')


def raw_qualified(result,raw):
    if 'unavailable' in result:return False
    return (len(raw.get('BEGIN',[]))==1 and raw['BEGIN'][0].get('detail_policy')==DETAILS_RETAINED and
        raw['CALIBRATION_END'][0]['status']=='VALID' and
        not any(result[k] for k in ('observer_post_failures','invalid_cpu_states',
            'auxiliary_trace_incomplete','invalid_boundaries','missing_runtime_paths')) and
        result['complete_violation_accounting_fields'] and result['contention']['available'] and
            result['components']['available'] and len(result['maxima'])==result['cpus']*11 and
        all(r['below_budget'] and r['observer_qualified'] for r in result['maxima']) and
        all(V.number(r,'invalid')==0 for r in raw['MAXIMUM']) and
        all(not r['invalid'] and not r['failures'] for r in result['components']['callbacks']))


def framed_command_result(plan,runtime,index,sequence,value):
    transport=module('foundation-qemu');crc,_=transport.build_command_frame(sequence,value)
    lines=(runtime/'qemu-serial.log').read_text().splitlines()
    accepts,begins,ends,replays,rejects=[transport.matching(lines,0,regex,sequence) for regex in
        (transport.ACCEPT_RE,transport.BEGIN_RE,transport.END_RE,transport.REPLAY_RE,transport.REJECT_RE)]
    E.require(len(accepts)==len(begins)==len(ends)==1 and not replays and not rejects and
        accepts[0][0]<begins[0][0]<ends[0][0] and accepts[0][1].group(2)==crc and
        int(accepts[0][1].group(3))==len(value),'TRANSPORT framed command identity/order')
    K.verify_input(runtime,plan,index,sequence,value)
    status=int(ends[0][1].group(2))
    result=dict(sequence=sequence,payload=value,crc32=crc,handler_status=status,
        classification='PASS' if status==0 else 'COMMAND_STRUCTURED',candidate_id=plan['candidate_id'],
        vm_id=plan['vm_ids'][index],monotonic_ns=time.monotonic_ns(),begin_line=begins[0][0],end_line=ends[0][0],
        input_sha256=E.sha(runtime/f'input-{sequence}-capture.json'))
    if value=='irqlatency export':
        result['proof_prefix']=json.loads((runtime/'proof-export-prefix.json').read_text())
        result['proof_prefix_sha256']=E.sha(runtime/'proof-export-prefix.json')
    return result


def command_proof(plan,runtime,index):
    E.require(plan['transport_contract']==K.CONTRACT,'command transport contract')
    rows=[json.loads(line) for line in (runtime/'framed.jsonl').read_text().splitlines()]
    E.require([r['payload'] for r in rows]==plan['commands'],'complete planned command coverage')
    transport=module('foundation-qemu');lines=(runtime/'qemu-serial.log').read_text().splitlines();previous=0
    for sequence,row in enumerate(rows,1):
        crc,_=transport.build_command_frame(sequence,row['payload'])
        a,b,c,replays,rejects=[transport.matching(lines,0,regex,sequence) for regex in
               (transport.ACCEPT_RE,transport.BEGIN_RE,transport.END_RE,transport.REPLAY_RE,transport.REJECT_RE)]
        E.require(len(a)==len(b)==len(c)==1 and not replays and not rejects and previous<a[0][0]<b[0][0]<c[0][0] and
                  a[0][1].group(2)==crc==row['crc32'] and int(a[0][1].group(3))==len(row['payload']) and
                  int(c[0][1].group(2))==row['handler_status'] and row['sequence']==sequence and
                  row['begin_line']==b[0][0] and row['end_line']==c[0][0] and
                  row['vm_id']==plan['vm_ids'][index] and row['candidate_id']==plan['candidate_id'],
                  'structured handler/candidate/boot evidence differs')
        E.require(E.sha(runtime/f'input-{sequence}-capture.json')==row['input_sha256'],'command input receipt changed')
        K.verify_input(runtime,plan,index,sequence,row['payload']);previous=c[0][0]
    verify_export_prefix(runtime,rows)
    return rows


def verify_export_prefix(runtime,rows):
    if rows and rows[-1]['payload']=='irqlatency export':
        last=rows[-1];prefix=json.loads((runtime/'proof-export-prefix.json').read_text())
        E.require(last['proof_prefix']==prefix and last['proof_prefix_sha256']==E.sha(runtime/'proof-export-prefix.json'),'proof prefix receipt changed')
        events=[json.loads(line) for line in (runtime/'driver.jsonl').read_text().splitlines()]
        starts=[r for r in events if r.get('event')=='HMP_INPUT_BEGIN' and r.get('sequence')==last['sequence']]
        E.require(len(starts)==1 and rows[-2]['monotonic_ns']<prefix['monotonic_ns']<starts[0]['monotonic_ns']<last['monotonic_ns'],'proof prefix not captured after audit and before export input')


def semantic_commands(role,commands,raw,post,functional):
    E.require(role in ('DIAGNOSTIC_NO_ACCEPTANCE','CURRENT_TIMING_QUALIFICATION','CURRENT_FUNCTIONAL_STRESS'),'command role')
    finish=[r for r in commands if r['payload']=='irqlatency finish']
    E.require(len(finish)==1,'finish command cardinality')
    ordinary=all(r['handler_status']==0 for r in commands if r['payload']!='irqlatency finish')
    status=finish[0]['handler_status'];ending=raw['CALIBRATION_END']
    E.require(len(ending)==1,'finish raw outcome missing')
    failed_clock=any(r['clock_errors'] for r in post['cpus'])
    failed_observer=any(r['observer_errors'] for r in post['cpus'])
    precise_refusal=status==1 and ending[0]['status']=='INVALID_CALIBRATION' and (failed_clock or failed_observer)
    success=status==0 and ending[0]['status']=='VALID' and not failed_clock and not failed_observer
    return dict(ordinary_success=ordinary,finish_handler_status=status,timing_only_refusal=precise_refusal,
        timing_commands_success=ordinary and success,
        functional_commands_success=ordinary and functional and (success or
            role=='CURRENT_FUNCTIONAL_STRESS' and precise_refusal))


def functional_raw(raw,cpus):
    E.require(len(raw['TERMINAL'])==1,'functional terminal cardinality')
    terminal=raw['TERMINAL'][0];n=V.number
    E.require(n(terminal,'cpus')==n(terminal,'frozen')==cpus and terminal['transport']=='COMPLETE' and
        n(terminal,'invalid_boundaries')==0,'functional terminal incomplete/invalid')
    states=V.index(raw['CPU'],('cpu',));E.require(set(states)=={(cpu,) for cpu in range(cpus)},'functional CPU coverage')
    E.require(all(not any(n(r,k) for k in ('active','pending','invalid','exhausted','recursion','missing_begin','unclosed')) for r in states.values()),'functional invalid recorder state')
    return True


def seal_analysis(path,result):
    # Compare the persisted JSON representation, including tuple/list and CPU keys.
    result=json.loads(E.canonical(result))
    if path.exists():E.require(json.loads(path.read_text())==result,'sealed analysis changed')
    else:E.write(path,result)
    return result


def analyze(directory,index=0):
    plan=bound_plan(directory);cpus=plan['profile']['smp'];runtime=directory/('boot-'+str(index))/'runtime'
    launch=json.loads((runtime/'launch.json').read_text())
    E.require(launch['elf']['sha256']==plan['elf_sha256'] and launch['image']['sha256']==plan['image_sha256'] and
        A.option(launch['argv'],'-uuid')==plan['vm_ids'][index] and not launch['fault_trace'],
        'actual boot candidate/trace identity')
    raw=V.records((runtime/'qemu-serial.log').read_text());P.method(raw);observer_method(plan,raw)
    result=dict(schema=1,role=plan['role'],candidate_id=plan['candidate_id'],elf_sha256=plan['elf_sha256'],
        image_sha256=plan['image_sha256'],vm_id=plan['vm_ids'][index],method=P.METHOD,qualified=False,
        implementation_acceptance=False,complete_portable_acceptance=False,hardware=False)
    try:
        result['clocksource']=CLOCKSOURCE.boot((runtime/'qemu-serial.log').read_text(),cpus,plan.get('clocksource_expected'))
        result['clocksource_terminal']=CLOCKSOURCE.terminal(raw,result['clocksource'])
        if plan.get('entry_contract'):
            result['clocksource_observations']=CLOCKSOURCE.observations(raw,result['clocksource'])
    except (ValueError,KeyError) as error:result['clocksource_terminal']=dict(status='UNAVAILABLE',reason=str(error))
    pre,detail=P.role_result(raw,0,cpus);result['pre']=detail
    hosts=[]
    for name in ('host-running.json','host-after-activation.json','host-after-stress.json','host-terminal.json'):
        if (runtime/name).exists():hosts.append(json.loads((runtime/name).read_text()))
    try:result['host_capacity']=A.verify_profile_series(hosts,launch,plan['role'],cpus,diagnostic_backend=bool(plan.get('ram_backend')))
    except (ValueError,KeyError,OSError) as error:result['host_capacity']=dict(status='UNAVAILABLE',reason=str(error))
    if (runtime/'host-terminal-unavailable.json').exists():result['terminal_unavailable']=json.loads((runtime/'host-terminal-unavailable.json').read_text())
    if raw['CALIBRATION_END']:
        _,detail=P.role_result(raw,1,cpus,pre);result['post']=detail
        try:result['observer_updates']=observer_updates(raw,cpus)
        except (ValueError,KeyError) as error:result['observer_updates']=dict(status='UNAVAILABLE',reason=str(error))
        try:result['optional_details']=optional_details(raw,cpus)
        except (ValueError,KeyError) as error:result['optional_details']=dict(status='UNAVAILABLE',reason=str(error))
        if plan.get('entry_contract'):
            for field,checker in (('entry_checkpoints',J.verify),('interior',I.verify),('clock_body',I.verify_clock),('tick_body',I.verify_tick),('writeback',B.verify),('heap_stages',F.verify),('heap_operation',F.verify_operation)):
                try:result[field]=checker(raw,cpus)
                except (ValueError,KeyError) as error:
                    result[field]=dict(status='UNAVAILABLE',reason=str(error),implementation_acceptance=False)
        try:result['raw']=V.inspect((runtime/'qemu-serial.log').read_text(),primitive=plan['timestamp_primitive'])
        except (ValueError,KeyError) as error:result['raw']=dict(unavailable=str(error))
        try:result['workload']=W.verify(raw,cpus,stack_contract=plan['stack_coverage_contract'])
        except (ValueError,KeyError) as error:result['workload']=dict(status='FAIL',reason=str(error))
        commands=command_proof(plan,runtime,index)
        try:
            result['service_audit']=L.verify((runtime/'qemu-serial.log').read_text(),(runtime/'qemu-debugcon.log').read_bytes(),plan,commands,index)
        except (ValueError,KeyError,OSError) as error:
            result['service_audit']=dict(status='FAIL',reason=str(error))
        try:
            functional=functional_raw(raw,cpus) and result['workload']['status']=='PASS' and result['service_audit']['status']=='PASS' and result['clocksource_terminal']['status']=='PASS'
            result['functional']=dict(status='PASS' if functional else 'FAIL')
        except (ValueError,KeyError) as error:
            result['functional']=dict(status='FAIL',reason=str(error));functional=False
        result['command_semantics']=semantic_commands(plan['role'],commands,raw,detail,functional)
        result['functional_complete']=(functional and result['command_semantics']['functional_commands_success'] and
            result['host_capacity']['status']=='PASS' and (runtime/'host-terminal.json').exists())
        result['qualified']=(plan['role']=='CURRENT_TIMING_QUALIFICATION' and result['pre']['valid'] and detail['valid'] and
            result['clocksource_terminal']['status']=='PASS' and
            result['observer_updates']['status']=='PASS' and
            result['optional_details']['status']=='PASS' and result['optional_details']['complete_optional_history'] and
            result['host_capacity']['status']=='PASS' and result['host_capacity']['timing_authority_eligible'] and
            (runtime/'host-terminal.json').exists() and result['service_audit']['status']=='PASS' and
            result['service_audit'].get('request_telemetry',{}).get('frequency_status')=='PASS' and
            result['service_audit']['request_telemetry']['required_local_valid'] and
            not result['service_audit']['request_telemetry']['invalid_send'] and
            result['workload']['status']=='PASS' and raw_qualified(result['raw'],raw) and
            result['command_semantics']['timing_commands_success'])
    else:result['post']=dict(status='NOT_COLLECTED',reason='activation refused or command/transport failure')
    if plan.get('ram_backend'):
        try:
            result['ram_backend']=RAM.verify_backend(plan,launch,json.loads((runtime/'ram-backend.json').read_text()))
            result['guest_memory']=RAM.guest_inventory((runtime/'qemu-serial.log').read_text())
            result['host_memory']=RAM.host([json.loads((runtime/name).read_text()) for name in RAM.PHASES])
        except (ValueError,KeyError,OSError) as error:result['host_memory']=dict(status='UNAVAILABLE',reason=str(error))
    if len(hosts)>=2:
        try:result['scheduling']=S.verify(hosts)
        except (ValueError,KeyError) as error:result['scheduling']=dict(status='UNAVAILABLE',reason=str(error))
    if plan.get('qemu_exit_trace'):
        try:result['qemu_exit_trace']=TRACE.verify(plan,launch,runtime,TRACE.counter_clocks(result['tick_body']),hosts)
        except (ValueError,KeyError,OSError) as error:result['qemu_exit_trace']=dict(status='UNAVAILABLE',reason=str(error),timing_authority=False)
        if result['qemu_exit_trace']['status']=='UNAVAILABLE':
            try:result['qemu_counter_prefix']=TRACE.verify_counter_prefix(plan,launch,runtime,result['tick_body'],hosts)
            except (ValueError,KeyError,OSError) as error:result['qemu_counter_prefix']=dict(status='UNAVAILABLE',reason=str(error),timing_authority=False)
    result.setdefault('functional_complete',False)
    result['status']='QUALIFIED_TIMING_BOOT' if result['qualified'] else 'BLOCKED_TIMING_AUTHORITY'
    path=directory/('boot-'+str(index))/'analysis.json'
    return seal_analysis(path,result)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('prepare','run','analyze'))
    p.add_argument('--directory',type=Path,required=True);p.add_argument('--candidate',type=Path)
    p.add_argument('--index',type=int,default=0);p.add_argument('--entry-diagnostic',action='store_true');a=p.parse_args()
    result=prepare(a.directory,a.candidate,entry_diagnostic=a.entry_diagnostic) if a.mode=='prepare' else run(a.directory,a.index) if a.mode=='run' else analyze(a.directory,a.index)
    print(json.dumps(result),flush=True)
