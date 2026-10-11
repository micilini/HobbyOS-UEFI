#!/usr/bin/env python3
"""Adversarial raw-cycle and source-role controls; all endpoints are synthetic."""
import copy
import importlib.util
from pathlib import Path
import shutil
import zlib
import memory_evidence as E
import request_local_cycles as R

def module(name):
    spec=importlib.util.spec_from_file_location(name,E.ROOT/'scripts'/(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def row(**changes):
    r=dict(generation=1,target=0,root=4096,local_generation=1,local_root=4096,cpu=0,apic=4,epoch=1,end_epoch=1,status=1,
        begin_flags=0,end_flags=0,handler_begin=100,operation_begin=110,operation_end=180,handler_end=200,
        send_cpu=0,send_apic=4,send_epoch=1,send_end_epoch=1,send_status=1,send_begin_flags=0,send_end_flags=0,send_begin=90,send_end=210)
    r.update(changes);return dict(data=':'.join(str(r[k]) for k in R.COLUMNS))

def tlb_cases():
    """Full synthetic functional workload through the actual source-bound decoder."""
    T=module('verify-tlb-shootdown');cases=[]
    def fixture(schema):
        lines=[];root=4096;requests=[];base=0xffffc000001ff000
        def emit(kind,**fields):
            lines.append('[TLB]['+kind+'] '+' '.join(k+'='+str(v) for k,v in fields.items()))
        emit('BEGIN',schema=schema,invocation=1,cpus=1,x2apic=0,sequential=256,concurrent_each=0,
             threshold=64,deadline_ns=1000000000,window_pages=65,deferred=1,observers=3)
        for round in [*range(256),320]:
            a=0xa000000001000000|round;b=0xb000000001000000|round
            emit('REPLACE',invocation=1,window=0,round=round,pte=8192,old=12291,new=16387,
                 frame_a=12288,frame_b=16384,marker_a=a,marker_b=b,readers=0)
            for phase in range(4):requests.append((round,phase,(1,2,63,64,65)[round%5],a if phase==1 else b))
        requests.append((0xffffffff,0,65,0))
        for generation,(round,phase,pages,marker) in enumerate(requests,1):
            fields=dict(invocation=1,window=0,round=round,phase=phase,generation=generation,first=base,
                last=base+(pages-1)*4096,pages=pages,strategy=1 if pages<=64 else 2,targets=1,ack=1,
                missing=0,failed=0,sent=0,send_failed=0,elapsed_ns=1000,contention=0,status=0,notifications=0)
            fields['timing_schema' if schema==7 else 'irq_ns']=1 if schema==7 else 100
            emit('REQUEST',**fields)
            sample=phase in (1,3);value=marker if sample else 0;loads=pages if sample else 0
            cells=f'0:4:{value:x}:1000:{loads}'+(':1' if schema==7 else ':10:1:20')
            emit('C',generation=generation,entries=cells)
            if schema==7:
                t=generation*1000
                emit('LOCAL',**row(generation=generation,local_generation=generation,epoch=0,end_epoch=0,
                    send_epoch=0,send_end_epoch=0,handler_begin=t+100,operation_begin=t+110,
                    operation_end=t+180,handler_end=t+200,send_begin=t+90,send_end=t+210))
            emit('SERVICE',generation=generation,operation=2 if sample else 1,mapping=0,root=root,
                first=base,last=base+(pages-1)*4096,pages=pages,strategy=1 if pages<=64 else 2,
                targets=1,ack=1,missing=0,failed=0,sent=0,send_failed=0,notifications=0,status=0,
                elapsed_ns=1000,identities='0:4')
        emit('AUDIT',invocation=1,before=0,completed_before=0,after=1029,emitted_before=0,root=root)
        emit('RETIRE',invocation=1,window=0,generation=1029,data_frames=2,released=2,retained_tables='OWNED')
        emit('DEFERRED',invocation=1,window=0,round=320,token=1,slot=0,queued=0,initial=1,
             completed=2,generation=1027,old_marker=0xa000000001000140,handle_released=1)
        for g in range(1,4):emit('OBSERVER',invocation=1,generation=g,targets=1,complete=1,
                               errors=0,start_ns=10,end_ns=20,status=1)
        emit('TABLES',invocation=1,before=0,after=4,retained='OWNED')
        emit('END',invocation=1,status='PASS',replacements=257,retained=0)
        return lines
    def check(lines,contract):
        sealed=[line+f' crc={zlib.crc32(line.encode())&0xffffffff:08x}' for line in lines]
        return T.verify_tlb(sealed,{0:(4,'BSP')},4096,False,timing_contract=contract)
    current=fixture(7);historical=fixture(6)
    positive=check(current,R.CONTRACT);old=check(historical,None)
    assert positive['local_cycles']['valid']==1029 and positive['maximum_local_operation_ns'] is None
    assert old['maximum_local_operation_ns']==10
    cases.append(dict(name='actual-full-current-and-historical-TLB-decoders',status='PASS'))
    first=next(i for i,line in enumerate(current) if line.startswith('[TLB][LOCAL]'))
    for name,lines,contract in [
        ('current-selects-retired-TLB-schema',current,None),
        ('historical-HPET-selects-current-TLB-schema',historical,R.CONTRACT),
        ('missing-TLB-local-target',current[:first]+current[first+1:],R.CONTRACT),
        ('duplicate-TLB-local-target',current[:first]+[current[first]]+current[first:],R.CONTRACT),
        ('missing-TLB-operation-end',current[:first]+['[TLB][LOCAL] data='+row(epoch=0,end_epoch=0,
            send_epoch=0,send_end_epoch=0,operation_end=0)['data']]+current[first+1:],R.CONTRACT),
        ('unknown-TLB-schema',[current[0].replace('schema=7','schema=8'),*current[1:]],R.CONTRACT)]:
        try:check(lines,contract)
        except (ValueError,KeyError) as error:cases.append(dict(name=name,status='REJECTED',reason=str(error)))
        else:raise AssertionError(name)
        assert check(current,R.CONTRACT)==positive and check(historical,None)==old
    cases.append(dict(name='TLB-current-selector-does-not-leak-to-history',status='PASS'))
    return cases

def run(output):
    output.mkdir(parents=True,exist_ok=False);cases=[];topology={0:(4,'BSP'),1:(7,'AP')}
    def check(value):return R.validate(value,topology,4096,1,0)
    positive=check(row());assert positive['operation_cycles']==70 and positive['handler_cycles']==100
    cases.append(dict(name='nested-local-and-initiator',status='PASS'))
    remote=row(target=1,cpu=1,apic=7,handler_begin=10000000100,operation_begin=10000000110,
        operation_end=10000000180,handler_end=10000000200)
    assert R.validate(remote,topology,4096,1,1)['operation_cycles']==70
    cases.append(dict(name='unsynchronized-CPU-offsets-never-subtracted',status='PASS'))
    no_tsc=row(status=2,handler_begin=0,operation_begin=0,operation_end=0,handler_end=0,
        send_status=2,send_begin=0,send_end=0,epoch=0,end_epoch=0,send_epoch=0,send_end_epoch=0)
    assert check(no_tsc)['handler_cycles'] is None
    cases.append(dict(name='unavailable-is-not-zero',status='PASS'))
    crossing=row(epoch=0,end_epoch=1,status=6)
    value=R.validate(crossing,topology,4096,1,0,allow_clock_failure=True)
    assert value['operation_cycles'] is None and value['handler_cycles'] is None
    cases.append(dict(name='honest-epoch-crossing-is-unavailable-not-functional-failure',status='PASS'))
    for name,changed in [
        ('wrong-generation',row(local_generation=2)),('wrong-root',row(local_root=8192)),
        ('wrong-target',row(cpu=1)),('copied-other-target',remote),('wrong-APIC',row(apic=7)),
        ('unknown-epoch',row(epoch=2)),('epoch-crossing-hidden',row(end_epoch=0)),('missing-begin',row(handler_begin=0)),
        ('missing-operation',row(operation_begin=0)),('missing-end',row(handler_end=0)),
        ('equal-endpoints',row(operation_end=110)),('backward-endpoints',row(operation_end=109)),
        ('operation-outside-handler',row(operation_end=201)),('initiator-does-not-enclose-local',row(send_end=199)),
        ('unknown-actor',row(send_cpu=2)),('wrong-actor-APIC',row(send_apic=7)),
        ('IF-enabled-start',row(begin_flags=512)),('IF-enabled-end',row(end_flags=512)),
        ('IF-enabled-send',row(send_begin_flags=512)),('invalid-represented-valid',row(status=4)),
        ('omitted-in-required-debug-role',row(status=0)),('unavailable-with-invented-value',row(status=2)),
        ('invented-ns-authority',dict(row(),elapsed_ns='0')),('minimum-promoted-upper',dict(row(),below_budget='PASS')),
        ('overflow',row(handler_end=1<<64)),('unknown-version-field',dict(row(),schema='2')),
        ('missing-column',dict(data=row()['data'].rsplit(':',1)[0]))]:
        try:check(changed)
        except (ValueError,KeyError) as e:cases.append(dict(name=name,status='REJECTED',reason=str(e)))
        else:raise AssertionError(name)
        assert check(row())==positive
    for ns in (49999,50000,50001):
        assert R.upper_ns(ns*3,3000000000)==ns
    assert R.upper_ns(1,3000000000)==1
    for cycles,hz in ((1,0),(0,1),(R.V.U64,1),(R.V.U64+1,1000000000)):
        try:R.upper_ns(cycles,hz)
        except ValueError:pass
        else:raise AssertionError('conversion invalid accepted')
    cases.append(dict(name='strict-boundary-upward-arithmetic-and-overflow',status='PASS'))
    H=module('request-telemetry-host');baseline=H.static(E.ROOT)
    scratch=output/'source-controls'
    names=['kernel/src/smp/cpu_request.c','kernel/src/memory/kernel_tlb.c',
           'kernel/src/smp/cpu_request_state.c','kernel/src/core/clock.c']
    for n in names:
        p=scratch/n;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(E.ROOT/n,p)
    assert H.static(scratch)==baseline
    for name,path,old,new in [
        ('telemetry-MMIO-hidden-by-short-report',names[0],'    (void)context;',
         '    (void)context; (void)hpet_read_clock_sample(NULL);'),
        ('removed-admission-clock',names[0],'clock_monotonic_read_ns(&started)', '(started=1,true)'),
        ('removed-deadline-check',names[0],'ended < previous || ended >= deadline','ended < previous'),
        ('changed-one-second-admission',names[0],'started + CPU_REQUEST_DEADLINE_NS','started + CPU_REQUEST_DEADLINE_NS*2'),
        ('ACK-before-work',names[2],'s->samples[slot] = sample;','/* early sample lost */'),
        ('system-clock-mask-hidden',names[3],'irq_save()','0'),
        ('invalid-clock-hidden-by-floor',names[3],'known && !decision.hard_failure && returned','known && returned'),
        ('invalid-clock-value-published',names[3],'if (valid) *out = value;','*out = value;'),
        ('stalled-terminal-success',names[0],'!ended_valid || ended <= started || ended < previous','!ended_valid || ended < previous')]:
        p=scratch/path;original=p.read_text();assert old in original;p.write_text(original.replace(old,new))
        try:H.static(scratch)
        except ValueError as e:cases.append(dict(name=name,status='REJECTED',reason=str(e)))
        else:raise AssertionError(name)
        finally:p.write_text(original)
    assert H.static(scratch)==baseline
    cases+=tlb_cases()
    result=dict(schema=1,status='PASS',synthetic=True,cases=cases,implementation_acceptance=False)
    E.write(output/'result.json',result);return result

def verify(output):
    r=E.load(output/'result.json') if hasattr(E,'load') else __import__('json').loads((output/'result.json').read_text())
    E.require(r['schema']==1 and r['status']=='PASS' and r['synthetic'] and
        len(r['cases'])==49 and len({c['name'] for c in r['cases']})==49 and not r['implementation_acceptance'],
        'request telemetry fixture inventory')
    return r
