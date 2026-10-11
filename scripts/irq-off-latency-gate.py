#!/usr/bin/env python3
"""Current latency gate layered on the canonical guarded-stack/native chain."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
os.environ['HOBBYOS_MEMORY_OBSERVER_ROLE']='irq-off-latency'
import memory_evidence as E
import irq_latency_current as C
import validation_resources as R
import validation_stages as S


def module(name):
    spec=importlib.util.spec_from_file_location('latency_gate_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
D=module('stack-guard-gate');G=D.G;T=D.T;X=module('irq-latency-pairing-experiment')
MODES=('static','host','build','runtime','production','fixtures')

def read(path):return json.loads(path.read_text())
def receipt(evidence,mode,result):T.receipt(evidence,'latency-'+mode,result)

def source_contract():
    C.contract();C.audit_sites();C.source_guard()
    contract=read(E.ROOT/'scripts/irq-latency-pairing-contract.json')
    E.require(contract['method']=='cpu-local-reference-pairs' and contract['version']==1 and
        contract['target_ns']==20000000 and contract['width_ppm']==contract['drift_ppm']==10000 and
        contract['budget_ns_exclusive']==50000,'frozen pairing numerics')
    source=(E.ROOT/'kernel/src/memory/irq_latency_test.c').read_text()
    E.require('irq_latency_pairs_build(series->points' in source and
        'attempt->acquisition = irq_latency_reference_acquire' in source and
        'attempt->valid = irq_latency_reference_window' not in source,'collector/method implementation mismatch')
    observer=read(E.ROOT/'scripts/irq-latency-contract.json')
    boundary=(E.ROOT/'kernel/src/core/irq_latency_boundary.c').read_text()
    E.require(observer['accounting']['nested_update_method']==X.OBSERVER_UPDATES and
        'observer_updates='+X.OBSERVER_UPDATES in source and
        'nested_record_unchanged(r, b->flags, paths, same)' in boundary and
        'nested_record_unchanged(recorder_for(slot), b->flags, paths_at(b->stack_pointer), true)' in boundary and
        'irq_latency_nested_elisions(cpu, &elided)' in source and
        'irq_latency_nested_prechecks(cpu, &prechecked)' in source,
        'current nested observer collector/policy mismatch')
    mmio=(E.ROOT/'kernel/src/cpu/mmio.c').read_text()
    E.require(observer['accounting']['mmio_identity_method']==X.MMIO_IDENTITY and
        'mmio_identity='+X.MMIO_IDENTITY in source and
        'identified = irq_latency_mmio_identity(&local_id)' in mmio and
        'irq_latency_mmio_reuses(cpu, &mmio_reused)' in source,
        'current MMIO observer identity source/policy mismatch')
    make=(E.ROOT/'makefile').read_text()
    E.require('IRQ_LATENCY_DETAILS ?= 1' in make and
        'IRQ_LATENCY_DETAILS=0 requires IRQ_ENTRY_DIAGNOSTIC=1' in make and
        'detail_policy=%s' in source and 'mandatory-maxima-only-v1' in source,
        'optional detail contrast must remain explicit and diagnostic')
    heap=(E.ROOT/'kernel/src/memory/heap.c').read_text()
    E.require('heap_stage_report(cpu)' in source and 'continuous-mask-heap-stages-v1' in source and
        'HEAP_BEGIN(IRQ_HEAP_FREE, 0)' in heap and 'HEAP_END();' in heap and
        'irq_latency_heap_record(&g_irq_heap_max' in boundary,'heap diagnostic collector/source binding')
    E.require('heap_operation_report(cpu)' in source and 'heap-metadata-callsite-v1' in source and
        heap.index('HEAP_OPERATION_BEGIN();')<heap.index('heap_try_split_block(b, size);')<heap.index('HEAP_OPERATION_END();') and
        'irq_latency_heap_operation_record(&g_irq_heap_operation_max' in boundary,
        'isolated operation actual call-site/controller binding')
    clock=(E.ROOT/'kernel/src/core/clock.c').read_text()
    tsc=(E.ROOT/'kernel/src/core/irq_latency_tsc.h').read_text()
    E.require('#define HOBBYOS_IRQ_LOCAL_TSC_ORDER 0' in tsc and
        '__asm__ volatile("lfence; rdtsc; lfence"' in tsc and
        '__asm__ volatile("mfence; lfence; rdtsc; lfence"' in tsc and
        'vendor_b==0x756e6547u && vendor_d==0x49656e69u && vendor_c==0x6c65746eu' in tsc and
        'irq_latency_tsc_available()' in boundary,'timestamp capability/ordering source binding')
    body=clock[clock.index('static uint64_t clock_read_ns(bool *valid, uintptr_t site)'):clock.index('uint64_t clock_monotonic_us(void)')]
    E.require('clock_body_report(cpu)' in source and 'masked-clock-segment-v3' in source and
        body.index('irq_save()')<body.index('CLOCK_BODY_BEGIN(flags,site)')<body.index('clocksource_read_local(index,&local)') and
        'irq_restore(flags);\n        sampled = clock_hpet_sample(&sample,NULL);\n        flags = irq_save();' in body and
        'resumed_slot != index || cpu->read_revision != revision' in body and
        'if (!preclassified)' in body and body.count('clock_hpet_sample(&sample,NULL)')==3 and
        'CLOCK_BODY_PHASE(4)' in body and '__atomic_exchange_n(&cpu->reading,1,__ATOMIC_ACQUIRE)' in body and
        '!smp_current_cpu_slot(&after_slot) || after_slot!=index' in body and
        'if(!valid && clock_monotonic_is_ready())kpanic("CLOCK: no valid monotonic sample")' in body and
        'CLOCK_BODY_END();\n    irq_restore(entry_flags);' in body and
        'r->pending!=t.sequence' in boundary,'clock diagnostic actual body and retirement binding')
    tick=module('irq-latency-host').tick_handler_source()
    E.require('tick_body_report(cpu)' in source and 'lapic-handler-body-v4' in source and
        tick.index('tick.begin.tsc=')<tick.index('clock_monotonic_ns()')<tick.index('tick.after_clock=')<
        tick.index('timer_clockevent_on_lapic_tick(')<tick.index('tick.after_work=')<tick.rindex('lapic_eoi();')<
        tick.index('irq_latency_tick_complete('),'tick actual handler boundary contract')
    sampled=body[body.index('irq_latency_tick_sample_t *tick_sample=known'):body.index('clock_stats_max_u64') ]
    E.require(sampled.index('tick_sample->begin=irq_latency_read_tsc()')<sampled.index('sampled = clock_hpet_sample(')<sampled.index('tick_sample->end=irq_latency_read_tsc()') and
        'hpet_read_clock_sample_attributed(&h,counter_end)' in clock and
        'tick_sample->source=sample.source;tick_sample->source_epoch=sample.epoch' in sampled,
        'tick HPET actual sample boundary contract')
    provider=(E.ROOT/'kernel/src/core/clocksource.c').read_text()
    pvclock=(E.ROOT/'kernel/src/core/pvclock.c').read_text()
    E.require('CLOCKSOURCE_REFERENCE_NS UINT64_C(1000000000)' in provider and
        'physical_image_address_range(' in provider and
        'cpu_write_msr_safe(PVCLOCK_SYSTEM_TIME_MSR,physical|1)' in provider and
        'cpu_read_msr_safe(PVCLOCK_SYSTEM_TIME_MSR,&cpu->registration)' in provider and
        'raw.ns-cpu->last_reference_ns>=CLOCKSOURCE_REFERENCE_NS' in provider and
        '__atomic_exchange_n(&cpu->busy,1,__ATOMIC_ACQUIRE)' in provider and
        'if(failure)source_failed(cpu,failure)' in provider and
        'attempt < PVCLOCK_SNAPSHOT_ATTEMPTS' in pvclock and
        'clocksource_terminal_capture(cpu)' in boundary and
        'clocksource_terminal_report(cpu)' in source,'qualified provider lifetime/reference/terminal source contract')
    hpet=(E.ROOT/'kernel/src/timer/hpet.c').read_text();sample_function=hpet[hpet.index('bool hpet_read_clock_sample_attributed('):hpet.index('bool hpet_counter_extend32_test(')]
    E.require(sample_function.index('hpet_read_counter_sample(&counter)')<sample_function.index('*counter_end=irq_latency_read_tsc()')<sample_function.index('hpet_counter_to_ns(counter.ticks)'),'tick counter/conversion source boundary')
    proof=(E.ROOT/'kernel/src/memory/tlb_fixture.c').read_text()
    functions=__import__('stack_current').functions(proof)
    E.require(functions['emit_line'].index('IRQ_LATENCY_SCOPE(IRQ_PATH_SHOOTDOWN)') <
        functions['emit_line'].index('spin_lock_irqsave'), 'proof capture wait/copy lacks mandatory path attribution')
    E.require('serial_write_all' not in functions['tlb_fixture_proof_export'] and
        'serial_write_all' not in functions['proof_export_line'] and
        'spin_lock' not in functions['proof_export_line'] and
        'irq_is_enabled()' in functions['proof_export_line'], 'proof export has IRQ-disabling serial/lock dependency')
    E.require('proof_output=2' in source and
        'tlb_fixture_proof_configured()' in source and 'tlb_fixture_proof_export(g_proof_terminal_service)' in source and
        'tlb_proof_append(&g_proof,line+1,bytes)' in proof and
        'if (!g_proof_deferred)' in proof and 'g_proof_completed==g_proof_invocations' in proof,
        'current immutable proof source binding')
    module('request-telemetry-host').static(E.ROOT)
    E.require('request_timing=request-local-cycles-v1' in source and 'REQUEST_CYCLES' in source and
        'writeback_clock_report' not in source and 'schema=7' in (E.ROOT/'kernel/src/memory/tlb_fixture.c').read_text(),
        'request current source/telemetry collector mismatch')
    return dict(method=contract['method'],version=1,contract_sha256=E.sha(E.ROOT/'scripts/irq-latency-pairing-contract.json'),
        collector_sha256=E.sha(E.ROOT/'kernel/src/memory/irq_latency_test.c'))

def static(evidence):
    D.static(evidence);receipt(evidence,'static',source_contract())

def host(evidence):
    D.host(evidence)
    path=evidence/'latency-host'/G.stamp();module('irq-latency-host').run(path)
    proof_output=module('tlb-proof-fixtures').run(path/'proof-output-fixtures')
    pairing=module('irq-latency-pairing-fixtures').run(path/'collector-normal',path/'pairing-fixtures')
    driver=module('irq-latency-driver-fixtures').run(path/'driver-fixtures')
    integration=module('irq-latency-integration-fixtures').run(path/'integration-fixtures')
    interior=module('irq-latency-interior-fixtures').run(path/'collector-normal',path/'interior-fixtures')
    writeback=module('irq-latency-writeback-fixtures').run(path/'interior-fixtures/raw.json',path/'writeback-fixtures')
    clock_calls=module('irq-latency-writeback-clock-fixtures').run(path/'writeback-fixtures/raw.json',path/'clock-call-fixtures')
    heap_stages=module('irq-latency-heap-fixtures').run(path/'interior-fixtures/raw.json',path/'heap-stage-fixtures')
    heap_operation=module('irq-latency-heap-operation-fixtures').run(path/'heap-stage-fixtures/raw.json',path/'heap-operation-fixtures')
    ram_backend=module('irq-latency-ram-backend-fixtures').run(path/'ram-backend-fixtures')
    qemu_trace=module('irq-latency-qemu-trace-fixtures').run(path/'qemu-trace-fixtures')
    activation=module('irq-latency-activation-envelope-fixtures').run(path,path/'activation-envelope-fixtures')
    receipt(evidence,'host',dict(path=path.relative_to(evidence).as_posix(),
        pairing_negatives=len(pairing['negative_cases']),driver_cases=driver['cases'],
        proof_output_cases=proof_output['cases'],proof_output_sha256=E.sha(path/'proof-output-fixtures/result.json'),
        integration_cases=integration['cases'],integration_sha256=E.sha(path/'integration-fixtures/result.json'),
        interior_cases=interior['cases'],interior_sha256=E.sha(path/'interior-fixtures/result.json'),
        writeback_cases=writeback['cases'],writeback_sha256=E.sha(path/'writeback-fixtures/result.json'),
        clock_call_cases=clock_calls['cases'],clock_call_sha256=E.sha(path/'clock-call-fixtures/result.json'),
        heap_stage_cases=heap_stages['cases'],heap_stage_sha256=E.sha(path/'heap-stage-fixtures/result.json'),
        heap_operation_cases=heap_operation['cases'],heap_operation_sha256=E.sha(path/'heap-operation-fixtures/result.json'),
        ram_backend_cases=ram_backend['cases'],ram_backend_sha256=E.sha(path/'ram-backend-fixtures/result.json'),
        qemu_trace_cases=qemu_trace['cases'],qemu_trace_sha256=E.sha(path/'qemu-trace-fixtures/result.json'),
        activation_envelope_cases=activation['cases'],activation_envelope_sha256=E.sha(path/'activation-envelope-fixtures/result.json'),
        pairing_sha256=E.sha(path/'pairing-fixtures/result.json'),driver_sha256=E.sha(path/'driver-fixtures/result.json')))

def build(evidence):
    D.build(evidence);receipt(evidence,'build',D.semantic(evidence,'build'))

def timing_proof(evidence):
    directory=evidence/'timing';plan=X.bound_plan(directory)
    E.require(plan['role']=='CURRENT_TIMING_QUALIFICATION' and plan['boots']==3 and plan['retries']==0,
              'diagnostic/partial series cannot be timing acceptance')
    candidate=read(evidence/'candidates/selftest/candidate.json')
    E.require(plan['candidate_id']==E.identity(candidate),'unbooted/different candidate credited')
    rows=[]
    for i in range(3):
        row=X.analyze(directory,i)
        E.require(row['qualified'] and row['role']=='CURRENT_TIMING_QUALIFICATION','qualified complete timing boot missing')
        rows.append(row)
    E.require(len({r['vm_id'] for r in rows})==3,'distinct timing boot inventory')
    return rows

def runtime(evidence):
    directory=evidence/'timing'
    if not directory.exists():X.prepare(directory,evidence/'candidates/selftest','CURRENT_TIMING_QUALIFICATION',3)
    for i in range(3):
        target=directory/('boot-'+str(i))
        if not target.exists():X.run(directory,i)
        E.require(X.analyze(directory,i)['qualified'],'timing failed; no unchanged retry or expensive matrix')
    timing_proof(evidence)
    D.runtime(evidence)
    stress=evidence/'functional-stress-smp24'
    if not stress.exists():X.prepare(stress,evidence/'candidates/selftest','CURRENT_FUNCTIONAL_STRESS',1)
    if not (stress/'boot-0').exists():X.run(stress,0)
    E.require(X.analyze(stress,0)['functional_complete'],'SMP24 actual concurrent stress incomplete')
    receipt(evidence,'runtime',runtime_proof(evidence))

def runtime_proof(evidence):
    return dict(timing=timing_proof(evidence),functional=D.runtime_proof(evidence),
        smp24=X.analyze(evidence/'functional-stress-smp24',0))

def production(evidence):
    # Whole production builds/smokes remain the inherited predicates.
    D.production(evidence);receipt(evidence,'production',D.semantic(evidence,'production'))

def fixtures(evidence):
    D.fixtures(evidence)
    path=evidence/'latency-host'/G.stamp();module('irq-latency-host').run(path)
    a=module('irq-latency-pairing-fixtures').run(path/'collector-normal',path/'pairing-fixtures')
    b=module('irq-latency-driver-fixtures').run(path/'driver-fixtures')
    integration=module('irq-latency-integration-fixtures').run(path/'integration-fixtures')
    interior=module('irq-latency-interior-fixtures').run(path/'collector-normal',path/'interior-fixtures')
    writeback=module('irq-latency-writeback-fixtures').run(path/'interior-fixtures/raw.json',path/'writeback-fixtures')
    clock_calls=module('irq-latency-writeback-clock-fixtures').run(path/'writeback-fixtures/raw.json',path/'clock-call-fixtures')
    heap_stages=module('irq-latency-heap-fixtures').run(path/'interior-fixtures/raw.json',path/'heap-stage-fixtures')
    heap_operation=module('irq-latency-heap-operation-fixtures').run(path/'heap-stage-fixtures/raw.json',path/'heap-operation-fixtures')
    ram_backend=module('irq-latency-ram-backend-fixtures').run(path/'ram-backend-fixtures')
    qemu_trace=module('irq-latency-qemu-trace-fixtures').run(path/'qemu-trace-fixtures')
    activation=module('irq-latency-activation-envelope-fixtures').run(path,path/'activation-envelope-fixtures')
    history=read(evidence/'latency-history.json')
    controls=module('irq-latency-regression-controls').run(Path(history['session']),Path(history['baseline']),
        Path(history['current']),path/'regression-controls')
    E.require(controls['status']=='PASS','preserved regression controls')
    receipt(evidence,'fixtures',dict(path=path.relative_to(evidence).as_posix(),pairing_negatives=len(a['negative_cases']),driver_cases=b['cases'],
        integration_cases=integration['cases'],integration_sha256=E.sha(path/'integration-fixtures/result.json'),
        interior_cases=interior['cases'],interior_sha256=E.sha(path/'interior-fixtures/result.json'),
        writeback_cases=writeback['cases'],writeback_sha256=E.sha(path/'writeback-fixtures/result.json'),
        clock_call_cases=clock_calls['cases'],clock_call_sha256=E.sha(path/'clock-call-fixtures/result.json'),
        heap_stage_cases=heap_stages['cases'],heap_stage_sha256=E.sha(path/'heap-stage-fixtures/result.json'),
        heap_operation_cases=heap_operation['cases'],heap_operation_sha256=E.sha(path/'heap-operation-fixtures/result.json'),
        ram_backend_cases=ram_backend['cases'],ram_backend_sha256=E.sha(path/'ram-backend-fixtures/result.json'),
        qemu_trace_cases=qemu_trace['cases'],qemu_trace_sha256=E.sha(path/'qemu-trace-fixtures/result.json'),
        activation_envelope_cases=activation['cases'],activation_envelope_sha256=E.sha(path/'activation-envelope-fixtures/result.json'),
        pairing_sha256=E.sha(path/'pairing-fixtures/result.json'),driver_sha256=E.sha(path/'driver-fixtures/result.json'),
        controls_sha256=E.sha(path/'regression-controls/result.json')))

def semantic(evidence,mode):
    row=read(evidence/('latency-'+mode+'.json'))
    E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()) and
              row['harness_sha256']==E.identity(E.manifest(harness=True)),'current latency stage identity')
    D.semantic(evidence,mode)
    if mode=='static':result=source_contract()
    elif mode=='runtime':result=runtime_proof(evidence)
    elif mode in ('build','production'):result=D.semantic(evidence,mode)
    else:
        path=evidence/row['result']['path'];module('irq-latency-host').verify(path)
        proof_output=read(path/'proof-output-fixtures/result.json')
        E.require(proof_output['status']=='PASS' and proof_output['cases'] and
            E.sha(path/'proof-output-fixtures/result.json')==row['result']['proof_output_sha256'],
            'current proof transport hosted evidence missing')
        a=read(path/'pairing-fixtures/result.json');b=read(path/'driver-fixtures/result.json')
        integration=read(path/'integration-fixtures/result.json')
        interior=read(path/'interior-fixtures/result.json')
        writeback=read(path/'writeback-fixtures/result.json')
        clock_calls=read(path/'clock-call-fixtures/result.json')
        heap_stages=read(path/'heap-stage-fixtures/result.json')
        heap_operation=read(path/'heap-operation-fixtures/result.json')
        ram_backend=read(path/'ram-backend-fixtures/result.json')
        qemu_trace=read(path/'qemu-trace-fixtures/result.json')
        activation=read(path/'activation-envelope-fixtures/result.json')
        for name,value in (('heap-operation',heap_operation),('ram-backend',ram_backend),('qemu-trace',qemu_trace),('activation-envelope',activation)):
            E.require(value['status']=='PASS' and value['cases'] and E.sha(path/(name+'-fixtures/result.json'))==row['result'][name.replace('-','_')+'_sha256'],'current diagnostic hosted receipt: '+name)
        import irq_latency_heap as F
        E.require(F.verify_operation(read(path/'heap-operation-fixtures/raw.json'),2)==read(path/'heap-operation-fixtures/positive.json'),'isolated operation raw fixture reconstruction')
        E.require(heap_stages['status']=='PASS' and heap_stages['cases'] and E.sha(path/'heap-stage-fixtures/result.json')==row['result']['heap_stage_sha256'],'heap-stage hosted receipt')
        E.require(F.verify(read(path/'heap-stage-fixtures/raw.json'),2)==read(path/'heap-stage-fixtures/positive.json'),'heap-stage independent fixture reconstruction')
        import irq_latency_writeback_clock as H
        E.require(clock_calls['status']=='PASS' and clock_calls['cases'] and E.sha(path/'clock-call-fixtures/result.json')==row['result']['clock_call_sha256'], 'clock-call hosted receipt')
        E.require(H.verify(read(path/'clock-call-fixtures/raw.json'),2)==read(path/'clock-call-fixtures/positive.json'), 'independent clock-call fixture reconstruction')
        import irq_latency_writeback as B
        E.require(writeback['status']=='PASS' and writeback['cases'] and E.sha(path/'writeback-fixtures/result.json')==row['result']['writeback_sha256'], 'writeback hosted receipt')
        E.require(B.verify(read(path/'writeback-fixtures/raw.json'),2)==read(path/'writeback-fixtures/positive.json'), 'independent writeback fixture reconstruction')
        E.require(interior['status']=='PASS' and interior['cases'] and E.sha(path/'interior-fixtures/result.json')==row['result']['interior_sha256'], 'interior hosted receipt')
        import irq_latency_interior as I
        E.require(I.verify(read(path/'interior-fixtures/raw.json'),2)==read(path/'interior-fixtures/positive.json'), 'independent interior fixture reconstruction')
        E.require(I.verify_clock(read(path/'interior-fixtures/clock-raw.json'),2)==read(path/'interior-fixtures/clock-positive.json'), 'independent clock body fixture reconstruction')
        E.require(I.verify_tick(read(path/'interior-fixtures/tick-raw.json'),2)==read(path/'interior-fixtures/tick-positive.json'),'independent tick body fixture reconstruction')
        E.require(integration['status']=='PASS' and integration['cases'] and not integration['vm_launched'] and
            E.sha(path/'integration-fixtures/result.json')==row['result']['integration_sha256'], 'hosted integration receipt')
        E.require(a['status']==b['status']=='PASS' and a['negative_cases'] and b['cases'], 'latency fixture coverage')
        if mode=='fixtures':
            E.require(read(path/'regression-controls/result.json')['status']=='PASS','preserved regression controls missing')
        import irq_latency_evidence as V
        E.require(E.sha(path/'pairing-fixtures/result.json')==row['result']['pairing_sha256'] and
            E.sha(path/'driver-fixtures/result.json')==row['result']['driver_sha256'] and
            E.sha(path/'pairing-fixtures/synthetic-collector.serial')==a['serial_sha256'], 'fixture raw/result binding')
        E.require(len(V.clocks(V.records((path/'pairing-fixtures/synthetic-collector.serial').read_text()),2))==4,
                  'actual collector independent reconstruction')
        result=dict(path=row['result']['path'],pairing_negatives=len(a['negative_cases']),driver_cases=b['cases'],
            proof_output_cases=proof_output['cases'],proof_output_sha256=E.sha(path/'proof-output-fixtures/result.json'),
            integration_cases=integration['cases'],integration_sha256=E.sha(path/'integration-fixtures/result.json'),
        interior_cases=interior['cases'],interior_sha256=E.sha(path/'interior-fixtures/result.json'),
        writeback_cases=writeback['cases'],writeback_sha256=E.sha(path/'writeback-fixtures/result.json'),
        clock_call_cases=clock_calls['cases'],clock_call_sha256=E.sha(path/'clock-call-fixtures/result.json'),
        heap_stage_cases=heap_stages['cases'],heap_stage_sha256=E.sha(path/'heap-stage-fixtures/result.json'),
        heap_operation_cases=heap_operation['cases'],heap_operation_sha256=E.sha(path/'heap-operation-fixtures/result.json'),
        ram_backend_cases=ram_backend['cases'],ram_backend_sha256=E.sha(path/'ram-backend-fixtures/result.json'),
        qemu_trace_cases=qemu_trace['cases'],qemu_trace_sha256=E.sha(path/'qemu-trace-fixtures/result.json'),
        activation_envelope_cases=activation['cases'],activation_envelope_sha256=E.sha(path/'activation-envelope-fixtures/result.json'),
            pairing_sha256=E.sha(path/'pairing-fixtures/result.json'),driver_sha256=E.sha(path/'driver-fixtures/result.json'))
        if mode=='fixtures':result['controls_sha256']=E.sha(path/'regression-controls/result.json')
    E.require(json.loads(E.canonical(result))==row['result'],'latency semantic replay differs: '+mode)
    return result

def verify(evidence):
    stages={mode:semantic(evidence,mode) for mode in MODES}
    inherited=D.verify(evidence)
    # D.verify includes the exact native inventory and layout roles via its chain.
    E.require(stages['runtime']['smp24']['functional_complete'],'SMP24 stress predicate')
    return dict(status='PASS',implementation_acceptance=True,complete_portable_acceptance=False,
        predicates=dict(source_integrity=source_contract(),frequency='VERIFIED_RAW_LOCAL_PAIRS',
            observer='OWN_FROZEN_PRE_ALLOWANCE_AND_POSTCHECK',host='VERIFIED_NON_OVERSUBSCRIBED_Q35_KVM_SMP8',
            stress_terminal='COMPLETE',strict_path_budget_ns=50000,functional_native_production=inherited),
        stages=stages,portable_boundary='Fresh transport extraction/replay is a separate required result.',
        independent_review='PENDING',hardware=False)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=(*MODES,'verify','all'))
    p.add_argument('--evidence',type=Path,required=True);p.add_argument('--scratch-parent',type=Path)
    p.add_argument('--observation-baseline',type=Path);a=p.parse_args();e=a.evidence.resolve()
    e.relative_to(E.ROOT/'backups');e.mkdir(parents=True,exist_ok=True)
    if a.scratch_parent:R.configure_scratch(a.scratch_parent)
    if a.observation_baseline:D.DEVICE.adopt_observation(e,a.observation_baseline.resolve())
    if a.mode=='all':
        D.DEVICE.P.storage_admission(e,D.DEVICE)
        for mode in MODES[:-1]:
            try:semantic(e,mode)
            except (OSError,ValueError,KeyError):globals()[mode](e)
        if not R.require_successor(S.ledger(D.DEVICE,e)):
            G.run(e,'current-native-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(e)])
        fixtures(e);T.finalize(e);receipt(e,'acceptance',verify(e))
    elif a.mode=='verify':print(json.dumps(verify(e)))
    else:globals()[a.mode](e)
if __name__=='__main__':main()
