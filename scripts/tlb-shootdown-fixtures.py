#!/usr/bin/env python3
"""Targeted semantic negatives and compiled mutations of the real C service."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import zlib
import memory_evidence as E
import tlb_current as T
import importlib.util

ROOT=Path(__file__).resolve().parent.parent
def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
V=module('verify-tlb-shootdown')

COMPILED=[
 ('xapic-interrupt-window','kernel/src/apic/lapic_delivery.c','    ops->publish(context);',
  '    ops->irq_restore(context, flags);\n    ops->publish(context);'),
 ('xapic-invalid-destination','kernel/src/apic/lapic_delivery.c','destination >= 255u','destination > 255u'),
 ('x2-destination-truncation','kernel/src/apic/lapic_delivery.c','((uint64_t)destination << 32) | low','((uint64_t)(destination & 255u) << 32) | low'),
 ('missing-publication-barrier','kernel/src/apic/lapic_delivery.c','    ops->publish(context);','    (void)context;'),
 ('silent-busy-timeout','kernel/src/apic/lapic_delivery.c','result = LAPIC_DELIVERY_PRE_TIMEOUT;','result = LAPIC_DELIVERY_SUBMITTED;'),
 ('ack-before-invalidation','kernel/src/smp/cpu_request_state.c','    sample.success = s->spec.apic[slot] == apic &&',
  '    __atomic_store_n(&s->publication[slot], (generation << 2) | 3, __ATOMIC_RELEASE);\n    sample.success = s->spec.apic[slot] == apic &&'),
 ('timeout-storage-reuse','kernel/src/smp/cpu_request_state.c','if (!safe) s->quarantined = true;','if (!safe) s->quarantined = false;'),
 ('if-disabled-admitted','kernel/src/smp/cpu_request_state.c','return interrupts_enabled && !irq_depth','return (interrupts_enabled || true) && !irq_depth'),
 ('irq-context-admitted','kernel/src/smp/cpu_request_state.c','return interrupts_enabled && !irq_depth','return interrupts_enabled && (irq_depth || true)'),
 ('missing-small-invalidation','kernel/src/memory/tlb_interval.c','ops->invalidate_page(context, i->first + page * 4096);','(void)context;'),
 ('missing-full-invalidation','kernel/src/memory/tlb_interval.c','ops->reload_cr3(context, cr3);','(void)context;'),
 ('wrong-full-threshold','kernel/src/memory/tlb_interval.h','#define TLB_SMALL_PAGE_LIMIT 64u','#define TLB_SMALL_PAGE_LIMIT 63u'),
 ('queue-saturation-dropped','kernel/src/memory/tlb_deferred.c','    return TLB_DEFERRED_BUSY;\n}', '    return TLB_DEFERRED_QUEUED;\n}'),
 ('post-send-timeout-accepted','kernel/src/apic/lapic_delivery.c','result = LAPIC_DELIVERY_INDETERMINATE;','result = LAPIC_DELIVERY_SUBMITTED;'),
 ('target-identity-ignored','kernel/src/smp/cpu_request_state.c','sample.success = s->spec.apic[slot] == apic &&','sample.success = true &&'),
 ('premature-deferred-release','kernel/src/memory/tlb_deferred.c','uint64_t complete = (h.generation << 3) | 4;',
  'uint64_t complete = (h.generation << 3) | 2;'),
]
RUNTIME_COMPILED=[
 ('pin-held-across-wait','        irq_restore(flags);\n    }\n    for (;;) {',
  '        (void)flags;\n    }\n    for (;;) {'),
 ('offline-target-filtered','__atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) != CPU_STATE_ONLINE',
  '(__atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) != CPU_STATE_ONLINE && false)'),
 ('panic-admission-ignored','if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) {\n        result.status = CPU_REQUEST_PANIC; return result;\n    }',
  'if (false) { result.status = CPU_REQUEST_PANIC; return result; }'),
]
SEMANTIC={
 'missing-target':'missing/failed remote completion','delivery-is-not-ack':'missing/failed remote completion',
 'wrong-apic':'slot/APIC/root/operation mismatch','wrong-root':'slot/APIC/root/operation mismatch',
 'stale-a-after-return':'stale marker or missing actual load','pte-without-load':'stale marker or missing actual load',
 'constant-observations':'stale marker or missing actual load','per-page-notifications':'per-page or duplicate notification round',
 'wrong-strategy':'normalized strategy/range','duplicate-completion':'duplicate/missing CPU completion',
 'stale-generation':'request identity/duplicate','missing-generation':'incomplete request phases',
 'frame-reuse-before-retirement':'storage reused before retirement','premature-retirement':'premature or incomplete retirement',
 'queued-is-not-complete':'queued work mistaken for completed retirement','false-concurrency':'no observed concurrent admission',
 'wrong-apic-mode':'topology/APIC mode binding','growing-scratch-tables':'scratch topology grows',
 'missing-observer-traffic':'observer traffic missing/duplicated',
}
REQUIRED=set(SEMANTIC)|{r[0] for r in COMPILED}|{r[0] for r in RUNTIME_COMPILED}|{'vector-collision','changed-timer-assignment',
    'missing-request-dispatch','missing-request-eoi','duplicate-request-eoi','production-service-absent','production-test-present',
    'trace-record-checksum','trace-missing','trace-truncated','trace-reordered','trace-binding',
    'production-test-marker','serial-backend-relabel','timer-header-unrelated-declaration',
    'ack-diagnostic-contradiction','ack-diagnostic-incomplete','ack-diagnostic-wrong-prefix',
    'harness-replay-linked-input-drift'}

def compiled_reason(name, stderr):
    if name != 'ack-before-invalidation': return stderr.strip()
    lines=stderr.splitlines()
    expected=r"test: scripts/tlb-shootdown-host\.c:[0-9]+: execute: Assertion `__atomic_load_n\(&storage\.publication\[sample->slot\],2\)==\(\(spec->generation<<2\)\|2\)' failed\."
    complete=bool(lines and re.fullmatch(expected,lines[0]))
    repeated=complete and all(line==lines[0] for line in lines[:-1])
    final=complete and (lines[-1]==lines[0] or
        len(lines)>1 and not stderr.endswith('\n') and lines[0].startswith(lines[-1]))
    E.require(complete and repeated and final,
              'concurrent ACK assertion differs from the publication invariant')
    # Several real targets can report the same failed invariant before abort.
    # A process-wide abort can cut off the final duplicate. Require a complete
    # invariant first, reject any contradictory suffix, and retain raw run.log.
    return lines[0]

def verify_receipt(result):
    names=[r['name'] for r in result['cases']]
    E.require(result['status']=='PASS' and set(names)==REQUIRED and len(names)==len(set(names)),
              'TLB fixture inventory missing/duplicated')
    E.require(result['positive_controls']==['real-capture-semantic','actual-vector-compile','request-dispatch','real-C-host'],
              'TLB fixture positive controls missing')
    for row in result['cases']:
        E.require(row['synthetic'] and row['actual_exit']!=0 and row['expected_boundary'] in row['actual_reason'],
                  'TLB fixture unrelated rejecting boundary: '+row['name'])
        if row['name']=='ack-before-invalidation':
            E.require(compiled_reason(row['name'],row['actual_reason'])==row['actual_reason'],
                      'ACK fixture receipt is not the verified publication invariant')

def run(evidence,output):
    output.mkdir(parents=True,exist_ok=False);cases=[]
    def reject(name,boundary,operation,recipe):
        try:operation()
        except Exception as error:
            E.require(boundary in str(error),'wrong TLB fixture boundary: '+name+': '+str(error))
            cases.append({'name':name,'synthetic':True,'expected_boundary':boundary,'actual_reason':str(error),
                          'expected_exit':1,'actual_exit':1,'recipe':recipe})
        else:raise ValueError('TLB negative accepted: '+name)
    base=evidence/'profiles/q35-kvm-smp4-1'
    if not base.is_dir():
        eligible=sorted(evidence.glob('diagnostics/q35-kvm-smp4-readback-*/result.json'))
        E.require(eligible,'a genuine current KVM capture is required for semantic fixtures')
        base=eligible[-1].parent
    V.verify_run(base)
    with tempfile.TemporaryDirectory(prefix='tlb-backend-fixture-') as temp:
        synthetic=Path(temp)/'capture';shutil.copytree(base,synthetic)
        result_path=synthetic/'result.json';result=json.loads(result_path.read_text())
        E.require(result['serial_backend']=='file','current trace fixture requires the declared serial backend')
        result['serial_backend']='socket';result_path.write_bytes(E.canonical(result))
        reject('serial-backend-relabel','serial socket observation/launch mismatch',lambda:V.verify_run(synthetic),
               {'synthetic':True,'operation':'relabel file observation as socket while retaining actual launch',
                'base_result_sha256':E.sha(base/'result.json'),'materialized_result_sha256':E.sha(result_path)})
    O=module('verify-memory-observation');lines=(base/'qemu-serial.log').read_text().splitlines()
    row=next(json.loads(l) for l in (base/'framed.jsonl').read_text().splitlines() if json.loads(l)['payload']=='tlbtest')
    serial_segment=O.FOUNDATION.transaction_segment(lines,row)
    blocks=V.trace_blocks(base)
    raw_segment=V.bind_trace(serial_segment,blocks[0])
    segment=V.expand_trace(raw_segment)
    decoded=V.rows(raw_segment)
    fixture_generations=sorted(V.n(r,'generation') for r in decoded['REQUEST'])
    post_generation=V.n(next(r for r in decoded['REQUEST'] if
        V.n(r,'window')==0 and V.n(r,'round')==0 and V.n(r,'phase')==3),'generation')
    topology={int(m[1]):(int(m[2]),m[3]) for line in lines if
              (m:=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line))}
    root=V.module('verify-cpu-control-state').verify_run(base)['root']
    V.verify_tlb(raw_segment,topology,root,True)
    with tempfile.TemporaryDirectory(prefix='tlb-trace-fixture-') as temp:
        missing=Path(temp)
        reject('trace-missing','TLB debug-console trace missing',lambda:V.trace_blocks(missing),
               {'operation':'omit required raw debug-console artifact'})
        (missing/'qemu-debugcon.log').write_bytes(blocks[0][:-1])
        reject('trace-truncated','TLB debug-console trace incomplete',lambda:V.trace_blocks(missing),
               {'operation':'truncate final newline','base_sha256':E.sha(base/'qemu-debugcon.log')})
    reject('trace-reordered','TLB serial/debug-console binding mismatch',
           lambda:V.bind_trace(serial_segment,blocks[1]),
           {'operation':'swap complete invocation blocks','base_sha256':E.sha(base/'qemu-debugcon.log')})
    altered=[re.sub(r'fnv1a=0x[0-9a-f]+','fnv1a=0x0',line) for line in serial_segment]
    reject('trace-binding','TLB serial/debug-console binding mismatch',
           lambda:V.bind_trace(altered,blocks[0]),{'operation':'alter trace digest in synthetic serial receipt'})
    damaged=list(raw_segment)
    for index,line in enumerate(damaged):
        if line.startswith('[TLB][C] '):
            damaged[index]=line.replace('entries=0:', 'entries=9:',1);break
    reject('trace-record-checksum','TLB record checksum mismatch',
           lambda:V.verify_tlb(damaged,topology,root,True),
           {'base_trace_sha256':E.sha(base/'qemu-debugcon.log'),'operation':'change CPU identity without changing record CRC'})
    def change(name):
        copy_lines=list(segment)
        def edit(kind,predicate,update):
            for index,line in enumerate(copy_lines):
                if not line.startswith('[TLB]['+kind+'] '):continue
                fields=dict(pair.split('=',1) for pair in line.split()[1:])
                if predicate(fields):
                    update(fields);copy_lines[index]='[TLB]['+kind+'] '+' '.join(k+'='+v for k,v in fields.items());return
            raise ValueError('synthetic anchor missing: '+name)
        first=lambda r:True
        post=lambda r:int(r['generation'])==post_generation
        if name=='missing-target':edit('REQUEST',first,lambda r:r.update(ack='0x7',missing='0x8'))
        elif name=='delivery-is-not-ack':edit('REQUEST',lambda r:int(r['sent'],16)!=int(r['targets'],16),lambda r:r.update(ack=r['sent']))
        elif name in ('wrong-apic','wrong-root','stale-a-after-return','pte-without-load','constant-observations','duplicate-completion'):
            def cpu(r):
                entries=r['entries'].split(',');parts=entries[0].split(':')
                if name=='wrong-apic':parts[1]='31'
                elif name=='wrong-root':parts[3]='2000'
                elif name=='stale-a-after-return':parts[2]='a000000001000000'
                elif name=='pte-without-load':parts[4]='0'
                elif name=='constant-observations':parts[2]='b000000001000001'
                else:entries.append(entries[0]);r['count']=str(len(entries))
                entries[0]=':'.join(parts);r['entries']=','.join(entries)
            edit('CPUS',post,cpu)
        elif name=='per-page-notifications':edit('REQUEST',first,lambda r:r.update(notifications=str(int(r['notifications'])+1)))
        elif name=='wrong-strategy':edit('REQUEST',first,lambda r:r.update(strategy='2'))
        elif name=='stale-generation':edit('REQUEST',lambda r:int(r['generation'])==fixture_generations[1],
                                         lambda r:r.update(generation=str(fixture_generations[0])))
        elif name=='missing-generation':copy_lines.remove(next(l for l in copy_lines if l.startswith('[TLB][REQUEST]')))
        elif name=='frame-reuse-before-retirement':
            def reuse(r):
                frame=int(r['frame_a'],16)+4096*100;r['frame_a']=hex(frame)
                r['old']=hex(frame|(int(r['old'],16)&~0x000ffffffffff000))
            edit('REPLACE',first,reuse)
        elif name=='premature-retirement':edit('RETIRE',first,lambda r:r.update(generation='1'))
        elif name=='queued-is-not-complete':edit('DEFERRED',first,lambda r:r.update(completed='1'))
        elif name=='false-concurrency':
            for i,l in enumerate(copy_lines):
                if l.startswith('[TLB][INITIATOR]'):copy_lines[i]=re.sub(r'contention=\d+','contention=0',l)
        elif name=='wrong-apic-mode':edit('BEGIN',first,lambda r:r.update(x2apic='0'))
        elif name=='growing-scratch-tables':edit('TABLES',first,lambda r:r.update(after='8'))
        elif name=='missing-observer-traffic':copy_lines.remove(next(l for l in copy_lines if l.startswith('[TLB][OBSERVER]')))
        # Explicit synthetic semantic fixture: update the wire checksum so the
        # intended contradiction reaches the same semantic validator.
        return [line+' crc='+f'{zlib.crc32(line.encode("ascii"))&0xffffffff:08x}'
                if line.startswith('[TLB]') else line for line in copy_lines]
    for name,boundary in SEMANTIC.items():
        mutated=change(name)
        recipe={'base':base.relative_to(evidence).as_posix(),'base_serial_sha256':E.sha(base/'qemu-serial.log'),
                'base_trace_sha256':E.sha(base/'qemu-debugcon.log'),
                'operation':name,'materialized_segment_sha256':E.identity(mutated),
                'boundary':'synthetic semantic input after validated transport; no boot repetition'}
        reject(name,'TLB '+boundary,lambda mutated=mutated:V.verify_tlb(mutated,topology,root,True),recipe)
    original=(ROOT/'kernel/src/core/interrupts.c').read_text();T.dispatch_semantics(original)
    header=(ROOT/'kernel/src/apic/lapic.h').read_text()
    header_contract=T.lapic_header_contract
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        from device_current import lapic_header_contract as header_contract
    header_contract(header)
    changed=header.replace('uint32_t desired_period_us', 'uint32_t unrelated_period_us', 1)
    E.require(changed != header, 'timer header mutation anchor missing')
    reject('timer-header-unrelated-declaration','LAPIC declarations exceed the checked-send contract',
           lambda:header_contract(changed),
           {'path':'kernel/src/apic/lapic.h','base_sha256':E.identity(header),
            'materialized_sha256':E.identity(changed),'operation':'change an unrelated timer declaration'})
    for name,source in [
        ('missing-request-dispatch',original.replace('cpu_request_irq();','(void)vector;',1)),
        ('missing-request-eoi',original.replace('cpu_request_irq();\n        lapic_eoi();','cpu_request_irq();',1)),
        ('duplicate-request-eoi',original.replace('cpu_request_irq();\n        lapic_eoi();','cpu_request_irq();\n        lapic_eoi();\n        lapic_eoi();',1))]:
        reject(name,'request dispatch EOI/order mismatch',lambda source=source:T.dispatch_semantics(source),
               {'base_sha256':E.identity(original),'materialized_sha256':E.identity(source),'operation':name})
    symbols=(evidence/'candidates/production/symbols.txt').read_text();V.production_symbols(symbols)
    reject('production-service-absent','production service/initializer compiled out',lambda:V.production_symbols(
        re.sub(r'^.*\bcpu_request_irq$','',symbols,flags=re.M)),{'remove_symbol':'cpu_request_irq'})
    reject('production-test-present','production test payload present',lambda:V.production_symbols(symbols+'\n1 T cmd_tlbtest\n'),
           {'append_symbol':'cmd_tlbtest'})
    strings=(evidence/'candidates/production/compiled-strings.txt').read_text();V.production_payload(strings)
    reject('production-test-marker','production test marker present',lambda:V.production_payload(strings+'\n[TLB_TRACE]\n'),
           {'append_compiled_string':'[TLB_TRACE]'})
    assertion="test: scripts/tlb-shootdown-host.c:107: execute: Assertion `__atomic_load_n(&storage.publication[sample->slot],2)==((spec->generation<<2)|2)' failed."
    E.require(compiled_reason('ack-before-invalidation',assertion)==
              compiled_reason('ack-before-invalidation',assertion+'\n'+assertion),
              'identical concurrent ACK diagnostics differ')
    E.require(compiled_reason('ack-before-invalidation',assertion+'\n'+assertion[:97])==assertion,
              'complete assertion with an interrupted duplicate differs')
    reject('ack-diagnostic-contradiction','concurrent ACK assertion differs',
           lambda:compiled_reason('ack-before-invalidation',assertion+'\nAssertion unrelated'),
           {'scope':'synthetic diagnostic parser input; raw concurrent stderr remains retained',
            'operation':'append a different failure to the publication-invariant diagnostic'})
    reject('ack-diagnostic-incomplete','concurrent ACK assertion differs',
           lambda:compiled_reason('ack-before-invalidation',assertion[:97]),
           {'operation':'omit every complete publication-invariant assertion'})
    reject('ack-diagnostic-wrong-prefix','concurrent ACK assertion differs',
           lambda:compiled_reason('ack-before-invalidation',assertion+'\n'+assertion[:97]+'WRONG'),
           {'operation':'append a nonmatching partial diagnostic after a complete assertion'})
    G=module('tlb-shootdown-gate')
    with tempfile.TemporaryDirectory(prefix='tlb-verifier-revision-') as temp:
        root=Path(temp)
        prior={'status':'PASS','synthetic':True,'build_input_sha256':E.identity(E.manifest()),'harness_sha256':'0'*64}
        for name in ('tlb-acceptance.json','tlb-fixtures.json','acceptance.json'):E.write(root/name,prior)
        original={name:(root/name).read_bytes() for name in ('tlb-acceptance.json','tlb-fixtures.json','acceptance.json')}
        saved=G.begin_harness_reverification(root)
        E.require(saved and all(not (root/name).exists() and (saved/name).read_bytes()==data
                  for name,data in original.items()),'invalidation verifier revision lost prior receipts')
        prior['build_input_sha256']='f'*64;E.write(root/'tlb-acceptance.json',prior)
        reject('harness-replay-linked-input-drift','harness re-verification cannot reuse changed build inputs',
               lambda:G.begin_harness_reverification(root),
               {'scope':'synthetic version-transition fixture; no new boot or acceptance claim'})
    compiled(output,cases)
    result={'status':'PASS','scope':'Synthetic semantic/compiled negatives; not runtime boot repetitions',
            'source_sha256':E.identity(E.manifest()),'harness_sha256':E.identity(E.manifest(harness=True)),
            'positive_controls':['real-capture-semantic','actual-vector-compile','request-dispatch','real-C-host'],'cases':cases}
    verify_receipt(result);E.write(output/'results.json',result);return result

def compiled(output,cases):
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    sources=['scripts/tlb-shootdown-host.c','kernel/src/apic/lapic_delivery.c','kernel/src/memory/tlb_interval.c',
             'kernel/src/memory/tlb_deferred.c','kernel/src/smp/cpu_request_state.c']
    with tempfile.TemporaryDirectory(prefix='tlb-compiled-fixtures-') as temp:
        root=Path(temp)
        for path in [*(ROOT/'kernel').rglob('*.h'),*(ROOT/'shared').rglob('*.h')]:
            target=root/path.relative_to(ROOT);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
        for name in sources:
            target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target)
        argv=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread','-fno-pie','-no-pie',
              '-DHOBBYOS_SELFTEST=1',*sources,'-o','test']
        subprocess.run(argv,cwd=root,check=True,capture_output=True);subprocess.run([str(root/'test')],check=True,capture_output=True)
        for name,path,old,new in COMPILED:
            target=root/path;original=target.read_text();E.require(original.count(old)==1,'compiled mutation anchor: '+name)
            target.write_text(original.replace(old,new));destination=output/name;destination.mkdir()
            build=subprocess.run(argv,cwd=root,text=True,capture_output=True)
            (destination/'compile.log').write_text(build.stdout+build.stderr)
            E.require(build.returncode==0,'compiled fixture did not reach C oracle: '+name+build.stderr)
            result=subprocess.run([str(root/'test')],cwd=root,text=True,capture_output=True,timeout=30)
            (destination/'run.log').write_text(result.stdout+result.stderr)
            E.require(result.returncode!=0 and 'Assertion' in result.stderr,'actual C mutation accepted: '+name)
            cases.append({'name':name,'synthetic':True,'expected_exit':'nonzero','actual_exit':result.returncode,
                'expected_boundary':'Assertion','actual_reason':compiled_reason(name,result.stderr),
                'recipe':{'path':path,'old':old,'new':new,'base_sha256':E.sha(ROOT/path),
                          'materialized_sha256':E.sha(target)},'compile_argv':argv,'compile_exit':build.returncode})
            target.write_text(original)
        header=root/'kernel/src/core/interrupt_vectors.h';original=header.read_text()
        (root/'vector.c').write_text('#include "kernel/src/core/interrupt_vectors.h"\nint main(void){return 0;}\n')
        command=['gcc','-std=c11','-Werror','-c','vector.c','-o','vector.o']
        built=subprocess.run(command,cwd=root,text=True,capture_output=True)
        (output/'runtime-positive-compile.log').write_text(built.stdout+built.stderr)
        E.require(built.returncode==0,'actual runtime positive compile failed: '+built.stderr)
        for name,old,new in [('vector-collision','INT_VECTOR_CPU_REQUEST 0xE1','INT_VECTOR_CPU_REQUEST 64'),
                             ('changed-timer-assignment','INT_VECTOR_LAPIC_TIMER 34','INT_VECTOR_LAPIC_TIMER 36')]:
            header.write_text(original.replace(old,new));result=subprocess.run(command,cwd=root,text=True,capture_output=True)
            E.require(result.returncode!=0 and 'static assertion failed' in result.stderr,'compiled vector collision accepted')
            destination=output/name;destination.mkdir();(destination/'compile.log').write_text(result.stdout+result.stderr)
            cases.append({'name':name,'synthetic':True,'expected_exit':1,'actual_exit':result.returncode,
                'expected_boundary':'static assertion failed','actual_reason':result.stderr,
                'recipe':{'path':'kernel/src/core/interrupt_vectors.h','old':old,'new':new,
                          'base_sha256':E.identity(original),'materialized_sha256':E.sha(header)}})
        runtime_sources=['scripts/tlb-runtime-host.c','kernel/src/apic/lapic_delivery.c',
                         'kernel/src/memory/tlb_interval.c','kernel/src/smp/cpu_request_state.c','kernel/src/smp/cpu_request.c']
        for name in runtime_sources:
            target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target)
        header.write_text(original)
        command=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread','-fno-pie','-no-pie',
                 '-DHOBBYOS_SELFTEST=1','-DHOBBYOS_DEBUG_ASSERT=0',*runtime_sources,'-o','runtime']
        subprocess.run(command,cwd=root,check=True,capture_output=True)
        subprocess.run([str(root/'runtime')],cwd=root,check=True,capture_output=True,timeout=30)
        target=root/'kernel/src/smp/cpu_request.c';original=target.read_text()
        for name,old,new in RUNTIME_COMPILED:
            E.require(original.count(old)==1,'runtime mutation anchor: '+name)
            target.write_text(original.replace(old,new));destination=output/name;destination.mkdir()
            built=subprocess.run(command,cwd=root,text=True,capture_output=True)
            (destination/'compile.log').write_text(built.stdout+built.stderr)
            E.require(built.returncode==0,'runtime mutation did not compile: '+name+built.stderr)
            ran=subprocess.run([str(root/'runtime')],cwd=root,text=True,capture_output=True,timeout=30)
            (destination/'run.log').write_text(ran.stdout+ran.stderr)
            E.require(ran.returncode!=0 and 'RUNTIME_HOST assertion=' in ran.stderr,'runtime negative accepted: '+name)
            cases.append({'name':name,'synthetic':True,'expected_exit':1,'actual_exit':ran.returncode,
                'expected_boundary':'RUNTIME_HOST assertion=','actual_reason':ran.stderr.strip(),
                'recipe':{'path':'kernel/src/smp/cpu_request.c','old':old,'new':new,
                          'base_sha256':E.sha(ROOT/'kernel/src/smp/cpu_request.c'),'materialized_sha256':E.sha(target)},
                'compile_argv':command,'compile_exit':built.returncode})
            target.write_text(original)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    try:print(json.dumps({'status':'PASS','cases':len(run(args.evidence,args.output)['cases'])}))
    except Exception as error:sys.exit(str(error))
