#!/usr/bin/env python3
"""Compile actual request/local code; replace exactly six privileged statements in scratch."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import memory_evidence as E
import request_local_cycles as R

MODES={'normal':[], 'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
       'asan':['-fsanitize=address','-fno-omit-frame-pointer']}
REPLACEMENTS=(
 ('__asm__ volatile("mov %%cr3, %0" : "=r"(value) :: "memory");','value=privileged_cr3();',1),
 ('__asm__ volatile("mov %%cr4, %0" : "=r"(value) :: "memory");','value=privileged_cr4();',1),
 ('__asm__ volatile("invlpg (%0)" :: "r"(address) : "memory");','privileged_invlpg(address);',1),
 ('__asm__ volatile("mov %0, %%cr3" :: "r"(root) : "memory");','privileged_reload(root);',1),
 ('__asm__ volatile("wbinvd" ::: "memory");','privileged_writeback();',2))

def transformed(root):
    source=(root/'kernel/src/memory/kernel_tlb.c').read_text()
    for old,new,count in REPLACEMENTS:
        E.require(source.count(old)==count,'request host privileged site count')
        source=source.replace(old,new)
    return source

def static(root):
    request=(root/'kernel/src/smp/cpu_request.c').read_text()
    local=(root/'kernel/src/memory/kernel_tlb.c').read_text()
    for source in (request,local):
        E.require(not any(v in source for v in ('hpet_read_clock_sample','irq_latency_writeback_clock_read',
            'handler_body_ns','initiator_irq_ns','sample->elapsed_ns')),'request direct telemetry MMIO/legacy field remains')
    E.require(request.count('clock_monotonic_read_ns(')==4 and 'clock_monotonic_ns()' not in request,'request safety clock inventory')
    for required in ('!clock_valid || !started || started > UINT64_MAX - CPU_REQUEST_DEADLINE_NS',
        'uint64_t deadline = started + CPU_REQUEST_DEADLINE_NS;',
        '!clock_valid || !now || now < previous || now >= deadline || stalled >= REQUEST_STALLED_SAMPLE_LIMIT',
        '!ended_valid || ended <= started || ended < previous || ended >= deadline',
        'result.elapsed_ns = ended_valid && ended > started ? ended - started : UINT64_MAX;',
        'cpu_request_retire(&g_request, result.status == CPU_REQUEST_OK)'):
        E.require(required in request,'request safety decision changed: '+required)
    E.require(request.count('!clock_valid || !now || now < previous || now >= deadline || stalled >= REQUEST_STALLED_SAMPLE_LIMIT')==2,
              'request admission/wait safety checks')
    state=(root/'kernel/src/smp/cpu_request_state.c').read_text()
    E.require('s->samples[slot] = sample;' in state and
        state.index('s->samples[slot] = sample;')<state.index('__atomic_store_n(&s->publication[slot], (generation << 2) | 3'),
        'request ACK precedes sample/work')
    clock=(root/'kernel/src/core/clock.c').read_text()
    E.require('irq_save()' in clock and 'hpet_read_clock_sample' in clock and 'irq_restore(' in clock,
              'system clock internal exclusion omitted')
    E.require('if (valid) *valid = known && !decision.hard_failure && returned && returned != UINT64_MAX;' in clock and
        'if (valid) *out = value;' in clock,'checked safety clock validity erased')
    return dict(status='PASS',safety_clock_sites=4,telemetry_hpet_sites=0,
                state_sha256=E.sha(root/'kernel/src/smp/cpu_request_state.c'),clock_sha256=E.sha(root/'kernel/src/core/clock.c'))

def output_proof(stdout,legacy,actual_clock=False,actual_pvclock=False):
    if actual_pvclock:
        E.require(not legacy and not actual_clock and stdout=="Actual KVM clock/request validity: PASS cases=17 ACK=AFTER_OPERATION quarantine=RETAINED\n",
                  "actual pvclock/request semantic cases")
        return
    if actual_clock:
        E.require(not legacy and stdout=="Actual HPET-backed clock/request validity: PASS cases=8 ACK=AFTER_OPERATION quarantine=RETAINED\n",
                  "actual clock/request semantic cases")
        return
    lines=stdout.splitlines()
    E.require(lines and lines[-1]=='Request actual dispatcher/state/local operations: PASS cases=20 privileged_operations=SUBSTITUTED',
              'request actual-source summary')
    if legacy:
        E.require(len(lines)==1,'request legacy actual output');return
    expected={(c,t) for c in (0,1,2,3,4,5,8,9) for t in range(2)};seen=set()
    for line in lines[:-1]:
        m=re.fullmatch(r'DATA case=([0-9]+) values=([0-9:]+)',line)
        E.require(m,'request actual serializer output')
        row=dict(data=m[2]);raw=R.decode(row);case=int(m[1]);key=(case,raw['target'])
        E.require(key in expected and key not in seen,'request actual serializer case inventory');seen.add(key)
        if case==8:
            E.require(raw['status']==raw['send_status']==4,'request backward counter outcome')
            try:R.validate(row,{0:(16,'BSP'),1:(17,'AP')},4096,1,key[1])
            except ValueError:pass
            else:raise ValueError('request host checker accepted actual invalid counter')
        else:
            result=R.validate(row,{0:(16,'BSP'),1:(17,'AP')},4096,1,key[1])
            E.require(result['epoch']==0 and result['operation_cycles']==(None if case==4 else 10),
                      'request independent actual operation clock arithmetic')
    E.require(seen==expected,'request actual serialized records missing')

def run(output,root=E.ROOT,legacy=False,local_order=False,actual_clock=False,actual_pvclock=False):
    root=root.resolve();output=output.resolve()
    E.require(not output.exists() and not output.is_relative_to(root/'kernel'), 'request host scratch output')
    output.mkdir(parents=True)
    E.require(not (legacy and (local_order or actual_clock or actual_pvclock)) and not (actual_clock and actual_pvclock), 'request source/provider role')
    fixture='scripts/request-pvclock-host.c' if actual_pvclock else 'scripts/request-clock-host.c' if actual_clock else 'scripts/request-telemetry-service-host.c'
    source=output/'kernel_tlb.host.c';source.write_text(transformed(root))
    files=['kernel/src/smp/cpu_request.c','kernel/src/smp/cpu_request.h','kernel/src/smp/cpu_request_state.c',
           'kernel/src/memory/kernel_tlb.c','kernel/src/memory/tlb_interval.c','kernel/src/core/irq_latency_tsc.h']
    if not legacy:files.extend(['kernel/src/smp/cpu_request_timing.h','kernel/src/core/clock.c','kernel/src/core/clock.h'])
    if actual_clock or actual_pvclock:files.extend(['scripts/request-telemetry-service-host.c',
        'kernel/src/core/clocksource.c','kernel/src/core/clocksource.h',
        'kernel/src/core/pvclock.c','kernel/src/core/pvclock.h'])
    before={n:E.sha(root/n) for n in files};rows=[]
    for mode,extra in MODES.items():
        binary=output/mode
        argv=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_SELFTEST=1',
            '-DHOBBYOS_DEBUG_ASSERT=0','-DIRQ_LATENCY_HOSTED_CLOCK=1',*extra,
            *( ['-DREQUEST_LEGACY=1'] if legacy else []),
            *(['-DHOBBYOS_IRQ_LOCAL_TSC_ORDER=1'] if local_order else []),
            '-DREQUEST_DISPATCH_SOURCE="'+str(root/'kernel/src/smp/cpu_request.c')+'"',
            '-DREQUEST_LOCAL_SOURCE="'+str(source)+'"','-I'+str(root/'kernel/src'),
            '-I'+str(root/'kernel/src/memory'),str(E.ROOT/fixture),
            str(root/'kernel/src/smp/cpu_request_state.c'),str(root/'kernel/src/memory/tlb_interval.c'),'-o',str(binary)]
        p=subprocess.run(argv,capture_output=True)
        (output/(mode+'.compile.log')).write_bytes(p.stdout+p.stderr)
        E.require(p.returncode==0,'request actual-source compile '+mode)
        q=subprocess.run([str(binary)],capture_output=True)
        (output/(mode+'.run.log')).write_bytes(q.stdout+q.stderr)
        E.require(q.returncode==0,'request actual-source execution '+mode)
        output_proof(q.stdout.decode(),legacy,actual_clock,actual_pvclock)
        rows.append(dict(mode=mode,argv=argv,compile_exit=p.returncode,run_exit=q.returncode,
            stdout=q.stdout.decode(),binary_sha256=E.sha(binary),
            compile_sha256=E.sha(output/(mode+'.compile.log')),run_sha256=E.sha(output/(mode+'.run.log'))))
    E.require(before=={n:E.sha(root/n) for n in files},'request host mutated immutable input')
    receipt=dict(schema=1,status='PASS',source=before,legacy=legacy,local_order=local_order,actual_clock=actual_clock,actual_pvclock=actual_pvclock,executions=rows,
        fixture_sha256=E.sha(E.ROOT/fixture),
        transformation_sha256=E.sha(source),substitutions=REPLACEMENTS,
        functional_authority='Actual C control flow with substituted privileged operations; no guest flush or duration claim.',
        static=None if legacy else static(root))
    E.write(output/'request-telemetry-host.json',receipt)
    return receipt

def verify(output,root=E.ROOT,local_order=False,actual_clock=False,actual_pvclock=False):
    r=json.loads((output/'request-telemetry-host.json').read_text())
    E.require(r['schema']==1 and r['status']=='PASS' and not r['legacy'],'request current host role')
    E.require(r.get('local_order') is local_order and r.get('actual_clock') is actual_clock and r.get('actual_pvclock') is actual_pvclock,'request timestamp/clock source role')
    fixture='scripts/request-pvclock-host.c' if actual_pvclock else 'scripts/request-clock-host.c' if actual_clock else 'scripts/request-telemetry-service-host.c'
    E.require(r['fixture_sha256']==E.sha(root/fixture),'request actual test source identity')
    E.require(r['source']=={n:E.sha(root/n) for n in r['source']} and r['static']==static(root),
              'request host current source binding')
    E.require(r['transformation_sha256']==hashlib.sha256(transformed(root).encode()).hexdigest() and
              r['substitutions']==[list(v) for v in REPLACEMENTS],'request host exact substitutions')
    E.require([x['mode'] for x in r['executions']]==list(MODES),'request host sanitizer coverage')
    for row in r['executions']:
        output_proof(row['stdout'],False,actual_clock,actual_pvclock)
        argv=row['argv'];flags=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_SELFTEST=1',
            '-DHOBBYOS_DEBUG_ASSERT=0','-DIRQ_LATENCY_HOSTED_CLOCK=1',*MODES[row['mode']],
            *(['-DHOBBYOS_IRQ_LOCAL_TSC_ORDER=1'] if local_order else [])]
        E.require(argv[:len(flags)]==flags and len(argv)==len(flags)+9 and
            argv[-2]=='-o' and argv[-3].endswith('/kernel/src/memory/tlb_interval.c') and
            argv[-4].endswith('/kernel/src/smp/cpu_request_state.c') and
            argv[-5].endswith('/'+fixture) and
            argv[len(flags)].startswith('-DREQUEST_DISPATCH_SOURCE="') and
            argv[len(flags)].endswith('/kernel/src/smp/cpu_request.c"') and
            argv[len(flags)+1].startswith('-DREQUEST_LOCAL_SOURCE="') and
            argv[len(flags)+1].endswith('/kernel_tlb.host.c"'),'request actual compile recipe')
        E.require(row['compile_exit']==row['run_exit']==0 and row['compile_sha256']==E.sha(output/(row['mode']+'.compile.log')) and
            row['run_sha256']==E.sha(output/(row['mode']+'.run.log')),'request host receipts')
    return dict(status='PASS',executions=3,cases=51 if actual_pvclock else 24 if actual_clock else 60)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--source',type=Path,default=E.ROOT);p.add_argument('--legacy',action='store_true')
    p.add_argument('--local-order',action='store_true')
    a=p.parse_args();print(json.dumps(run(a.output,a.source,a.legacy,a.local_order)))
