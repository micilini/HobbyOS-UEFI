#!/usr/bin/env python3
"""Acknowledged invalidation acceptance using the maintained memory campaign."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import memory_evidence as E
import validation_stages as S
import validation_candidates as candidates
import tlb_current as T

ROOT=Path(__file__).resolve().parent.parent
os.environ.setdefault('HOBBYOS_VALIDATION_CONTRACT','tlb-coherency')
os.environ['PYTHONDONTWRITEBYTECODE']='1'

def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

G=module('memory-observation-gate')
C=module('cpu-control-state-gate')
V=module('verify-tlb-shootdown')
O=module('verify-memory-observation')
COMMANDS=['tlbtest','tlbtest','cpustate','cpustate','cpustate','ptinspect samples','memmap',
          'mem','cpu','irq check','irq controllers','irq routes','irq boot']

def test_plan():
    return {'schema':1,'build_input_sha256':E.identity(E.manifest()),
        'normal_profiles':{name:3 for name in O.MATRIX},
        'supplemental_profiles':{'q35-tcg-smp4-nx-disabled':3,'q35-kvm-smp4-xapic':3},
        'commands':COMMANDS,'invocations_per_boot':2,'sequential_replacements':256,
        'concurrent_replacements_per_thread':64,'concurrent_threads_on_smp':2,
        'deferred_replacements':1,'pages':[1,2,63,64,65],'threshold_pages':64,
        'service_deadline_ns':1000000000,'trace_schema':6 if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else 5,'trace_channel':'existing-debugcon-0x402',
        'trace_binding':'serial-invocation-records-bytes-fnv1a-and-per-record-crc32'}

def receipt(directory,name,result):
    E.write(directory/(name+'.json'),{'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'result':result})

def static(evidence):
    E.write(evidence/'test-plan.json',test_plan())
    scope=T.scope('acknowledged-invalidation');scope['instructions']=T.audit_sites()
    scope['preserved_controls']=C.control_semantics()
    scope['dispatch']=T.dispatch_semantics((ROOT/'kernel/src/core/interrupts.c').read_text())
    scope['send_callers']=T.caller_semantics()
    G.static(evidence)
    directory=evidence/'static-vectors';directory.mkdir()
    source=directory/'check.c';source.write_text('#include "kernel/src/core/interrupt_vectors.h"\nint main(void) { return INT_VECTOR_CPU_REQUEST != 225; }\n')
    built=G.run(evidence,'vector-catalog-build',['gcc','-std=c11','-Wall','-Wextra','-Werror','-I.',str(source),'-o',str(directory/'check')])
    ran=G.run(evidence,'vector-catalog',[str(directory/'check')])
    scope['compiled_vectors']=[str(p.relative_to(evidence)) for p in (built,ran)]
    receipt(evidence,'tlb-static',scope)

def host(evidence):
    directory=evidence/'tlb-host'/G.stamp();directory.mkdir(parents=True)
    result=[]
    subjects=[('primitives',['scripts/tlb-shootdown-host.c','kernel/src/apic/lapic_delivery.c',
        'kernel/src/memory/tlb_interval.c','kernel/src/memory/tlb_deferred.c','kernel/src/smp/cpu_request_state.c'],[]),
        ('runtime',['scripts/tlb-runtime-host.c','kernel/src/apic/lapic_delivery.c','kernel/src/memory/tlb_interval.c',
         'kernel/src/smp/cpu_request_state.c','kernel/src/smp/cpu_request.c'],['-DHOBBYOS_DEBUG_ASSERT=0']),
        ('deferred-worker',['scripts/tlb-deferred-worker-host.c','kernel/src/memory/kernel_tlb.c',
         'kernel/src/memory/tlb_deferred.c','kernel/src/memory/tlb_interval.c'],['-UHOBBYOS_SELFTEST','-DHOBBYOS_DEBUG_ASSERT=0'])]
    for name,flags in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        for subject,sources,extra in subjects:
            binary=directory/(subject+'-'+name)
            built=G.run(evidence,subject+'-'+name+'-build',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                '-pthread','-fno-pie','-no-pie','-DHOBBYOS_SELFTEST=1',*flags,*extra,*sources,'-o',str(binary)])
            ran=G.run(evidence,subject+'-'+name,[str(binary)],{'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1'})
            output=(ran/'stdout.log').read_text()
            E.require('status=PASS' in output and 'privileged_operations=SUBSTITUTED' in output,'actual host summary missing')
            result.append({'subject':subject,'mode':name,'compile':str(built.relative_to(evidence)),
                           'execute':str(ran.relative_to(evidence)),'stdout':output,'binary_sha256':E.sha(binary)})
    if (evidence/'control-host.json').exists():C.verify_host(evidence)
    else:C.host(evidence,inherited=False)
    inherited=directory/'inherited';inherited.mkdir();G.host(inherited)
    receipt(evidence,'tlb-host',{'cases':result,'inherited':str(inherited.relative_to(evidence))})

def build_facts(directory,production):
    symbols=(directory/'symbols.txt').read_text()
    if production:
        V.production_symbols(symbols)
        V.production_payload((directory/'compiled-strings.txt').read_text())
    for name in ('lapic_delivery_send','cpu_request_handle','cpu_request_irq','kernel_tlb_invalidate','kernel_tlb_defer'):
        E.require(re.search(r'\b'+name+r'$',symbols,re.M),'invalidation service missing: '+name)
    disassembly=subprocess.check_output(['objdump','-d','-Mintel',str(directory/'kernel.elf')],text=True)
    relocations=subprocess.check_output(['readelf','-Wr',str(directory/'kernel.elf')],text=True)
    E.require(not subprocess.check_output(['nm','-u',str(directory/'kernel.elf')],text=True).strip(),'undefined kernel helper')
    def function(name):
        match=re.search(r'^[0-9a-f]+ <'+name+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',disassembly,re.M|re.S)
        E.require(match is not None,'emitted function missing: '+name);return match[1]
    E.require(re.search(r'\binvlpg\b',function('invalidate_page')),'local page invalidation compiled out')
    E.require(re.search(r'\bmov\s+cr3,',function('reload_cr3')),'local full invalidation compiled out')
    publish=function('delivery_publish')
    E.require(publish.count('mfence')==publish.count('lfence')==1 and publish.index('mfence')<publish.index('lfence'),
              'x2 publication ordering compiled out')
    handler=function('cpu_request_irq')+function('cpu_request_handle')
    E.require(not re.search(r'<(?:kmalloc|pmm_alloc_frame|serial_write_all|schedule|sem_wait|spin_lock)',handler),
              'blocking/allocation handler dependency')
    E.require(not re.search(r'\b[xyz]mm\d+\b',handler+function('lapic_delivery_send')),'unapproved SIMD in request service')
    stub=function('irq_vector_225')
    E.require('push   0xe1' in stub and 'irq_external_common_entry' in stub,'dedicated emitted external stub absent')
    L=module('verify-section-layout');layout=L.validate_layout(L.ElfFile.from_path(directory/'kernel.elf'))
    E.require(layout['trampoline']['sha256']==T.contract()['trampoline_abi']['sha256'],'protected trampoline changed')
    (directory/'tlb-disassembly.txt').write_text(disassembly);(directory/'tlb-relocations.txt').write_text(relocations)
    result={'layout':layout,'disassembly_sha256':E.sha(directory/'tlb-disassembly.txt'),
            'relocations_sha256':E.sha(directory/'tlb-relocations.txt'),'elf_sha256':E.sha(directory/'kernel.elf')}
    E.write(directory/'tlb-codegen.json',result);return result

def build(evidence):
    G.build(evidence,run_host=False)
    facts={name:build_facts(evidence/'candidates'/name,name!='selftest') for name in ('debug','production','selftest')}
    receipt(evidence,'tlb-build',facts)

def runtime(evidence,validation=None):
    validation=validation or V.verify_run
    G.runtime(evidence,scope='tlb-matrix',commands=COMMANDS,validation=validation,serial_backend='file')
    for scope in ('nx-disabled','xapic'):
        directory=evidence/scope;directory.mkdir(exist_ok=True)
        G.runtime(directory,scope=scope,commands=COMMANDS,candidate=evidence/'candidates/selftest',validation=validation,serial_backend='file')
    if not (evidence/'tlb-failure-experiments.json').exists():failure_experiments(evidence)
    collect_baseline(evidence)
    receipt(evidence,'tlb-runtime',verify_runtime(evidence))

def collect_baseline(evidence):
    """Publish the replayed clean-boot samples before their fixture consumers."""
    series=O.verify_series(evidence)
    if not (evidence/'baseline.json').exists():E.write(evidence/'baseline.json',series)
    G.verify_baseline(evidence,series)
    return series

def failure_experiments(evidence):
    selected={}
    for kind in ('delayed-response','context-failure'):
        name='expected-failures/'+G.stamp()+'-'+kind;directory=evidence/name
        argv=['python3','-B','scripts/memory-qemu.py','--evidence',str(directory),
            '--candidate',str(evidence/'candidates/selftest'),'--machine','q35','--accel','kvm','--smp','4',
            '--serial-backend','file','--fault-trace','--command','tlbtest '+kind]
        try:G.run(evidence,kind,argv)
        except RuntimeError:
            if kind!='context-failure':raise
        O.verify_run(directory/'qemu-serial.log',failure_experiment=kind)
        selected[kind]=name
    E.write(evidence/'tlb-failure-experiments.json',selected)

def verify_runtime(evidence):
    result={};modes={}
    for scope,path in [('tlb-matrix',evidence),('nx-disabled',evidence/'nx-disabled'),('xapic',evidence/'xapic')]:
        summary=O.verify_series(path);E.require(summary['scope']==scope,'invalidation series scope differs')
        manifest=json.loads((path/'series.json').read_text());runs=[]
        for name in manifest['runs']+manifest['diagnostics']:
            row=V.verify_run(path/name);runs.append(row)
            if name in manifest['runs']:
                E.require(row['policy']['observation']['serial_backend']=='file',
                          'current invalidation series requires the declared serial file backend')
                modes.setdefault(row['policy']['profile'],set()).add(row['workloads'][0]['x2apic'])
        result[scope]={'series':summary,'runs':runs}
    E.require(modes['q35-tcg-smp4']=={False} and modes['q35-kvm-smp4']=={True} and
              modes['q35-kvm-smp4-xapic']=={False},'actual xAPIC/x2APIC workload coverage missing')
    failures=json.loads((evidence/'tlb-failure-experiments.json').read_text())
    E.require(set(failures)=={'context-failure','delayed-response'},'guest failure experiment omitted')
    result['expected_failures']={kind:O.verify_run(evidence/name/'qemu-serial.log',failure_experiment=kind)
                                for kind,name in failures.items()}
    return result

def production(evidence):
    candidate=evidence/'candidates/production';G.verify_local_candidate(candidate);runs=[]
    for accel,smp in [('tcg',1),('kvm',4)]:
        name=f'production/q35-{accel}-smp{smp}';directory=evidence/name
        argv=['python3','-B','scripts/memory-qemu.py','--evidence',str(directory),'--candidate',str(candidate),
              '--machine','q35','--accel',accel,'--smp',str(smp),'--serial-backend','file','--production']
        for command in ['ptinspect','cpustate','memmap','tlbtest','mem','cpu','irq check']:argv+=['--command',command]
        if not directory.exists():G.run(evidence,'production-'+accel,argv)
        V.verify_run(directory);runs.append(name)
    E.write(evidence/'production.json',{'status':'PASS','candidate_id':G.verify_local_candidate(candidate),'runs':runs})
    receipt(evidence,'tlb-production',[V.verify_run(evidence/name) for name in runs])

def fixtures(evidence):
    collect_baseline(evidence)
    inherited=G.fixtures(evidence)
    directory=evidence/'tlb-fixtures'/G.stamp()
    G.run(evidence,'tlb-fixtures',['python3','-B','scripts/tlb-shootdown-fixtures.py','--evidence',str(evidence),'--output',str(directory)])
    baseline=evidence.parent.parent/'before/observation-source'
    E.require((baseline/'scripts/verify-memory-observation.py').is_file(),'preserved original observation verifier missing')
    policy=module('cpu-control-fixtures').run(evidence,directory/'policy',baseline)
    host(directory)
    receipt(evidence,'tlb-fixtures',{'path':str(directory.relative_to(evidence)),
        'sha256':E.sha(directory/'results.json'),'policy_sha256':E.sha(directory/'policy/results.json'),
        'policy_cases':len(policy['cases']),'inherited':inherited})

def focused_fixtures(evidence):
    """Exercise available proofs without asserting the missing aggregate gates."""
    directory=evidence/'focused-fixtures'/G.stamp();directory.mkdir(parents=True)
    selected=sorted(evidence.glob('diagnostics/q35-kvm-smp4-readback-*/result.json'))
    E.require(selected,'independent current capture required for focused fixtures')
    capture=selected[-1].parent;V.verify_run(capture)
    F=module('tlb-shootdown-fixtures');F.run(evidence,directory/'invalidation')
    module('cpu-control-fixtures').run(evidence,directory/'policy',evidence.parent.parent/'before/observation-source')
    for name,script,option in [('decoder','memory-observation-fixtures.py','--evidence'),
                               ('interrupt','memory-interrupt-fixtures.py','--output'),
                               ('packaging','memory-packaging-fixtures.py','--output')]:
        G.run(directory,name,['python3','-B','scripts/'+script,option,str(directory/name)])
    G.run(directory,'capture-recipes',['python3','-B','scripts/memory-replay-fixtures.py',
        '--source',str(capture),'--output',str(directory/'capture-recipes')])
    receipt(evidence,'focused-fixtures',{'path':directory.relative_to(evidence).as_posix(),
        'capture':capture.relative_to(evidence).as_posix(),
        'scope':'Available focused positives/negatives only; no complete series or inherited ledger assertion',
        'invalidation_sha256':E.sha(directory/'invalidation/results.json'),
        'policy_sha256':E.sha(directory/'policy/results.json'),
        'capture_sha256':E.sha(directory/'capture-recipes/result.json')})

def verify_codegen(directory,production,transport=None):
    if transport is None: directory=candidates.resolve(directory)
    row=json.loads((directory/'tlb-codegen.json').read_text())
    candidate=json.loads((directory/'candidate.json').read_text())
    identity=E.verify_candidate(candidate,json.loads((directory/'build-receipt.json').read_text()))
    L=module('verify-section-layout')
    if transport is None:
        E.require(G.verify_local_candidate(directory)==identity,'local build identity differs')
        elf=L.ElfFile.from_path(directory/'kernel.elf')
        for tool,name in ((['nm','-an'],'symbols.txt'),(['objdump','-d','-Mintel'],'tlb-disassembly.txt'),
                          (['readelf','-Wr'],'tlb-relocations.txt')):
            E.require(subprocess.check_output([*tool,str(directory/'kernel.elf')])==(directory/name).read_bytes(),
                      'actual ELF tool facts differ: '+name)
    else:
        for name,facts in candidate['artifacts'].items():transport.binary(directory/name,facts['sha256'],facts['size'])
        elf=transport.elf(directory/'kernel.elf')
    layout=L.validate_layout(elf)
    E.require({k:v for k,v in layout.items() if k!='elf_sha256'}==
              {k:v for k,v in row['layout'].items() if k!='elf_sha256'} and
              row['elf_sha256']==row['layout']['elf_sha256']==candidate['artifacts']['kernel.elf']['sha256'],
              'invalidation layout/binary facts differ')
    for name,key in (('tlb-disassembly.txt','disassembly_sha256'),('tlb-relocations.txt','relocations_sha256')):
        E.require(E.sha(directory/name)==row[key],'invalidation emitted proof changed')
    symbols=(directory/'symbols.txt').read_text();C.verify_symbols(symbols,production)
    if production:
        V.production_symbols(symbols)
        V.production_payload((directory/'compiled-strings.txt').read_text())
    text=(directory/'tlb-disassembly.txt').read_text()
    def fn(name):
        value=re.search(r'^[0-9a-f]+ <'+name+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',text,re.M|re.S)
        E.require(value is not None,'invalidation emitted function missing: '+name);return value[1]
    E.require('invlpg' in fn('invalidate_page') and re.search(r'\bmov\s+cr3,',fn('reload_cr3')),
              'real local invalidation missing')
    publish=fn('delivery_publish')
    E.require(publish.count('mfence')==publish.count('lfence')==1 and publish.index('mfence')<publish.index('lfence'),
              'real x2 publication order missing')
    E.require('push   0xe1' in fn('irq_vector_225') and 'irq_external_common_entry' in fn('irq_vector_225'),
              'request stub delivery absent')
    E.require(not re.search(r'<(?:kmalloc|pmm_alloc_frame|serial_write_all|schedule|sem_wait|spin_lock)',
                           fn('cpu_request_irq')+fn('cpu_request_handle')),'request handler blocks or allocates')
    return row

def verify_host(evidence,current_harness=False):
    evidence=S.host_base(evidence)
    value=json.loads((evidence/'tlb-host.json').read_text());rows=value['result']['cases']
    E.require(value['build_input_sha256']==E.identity(E.manifest()),'host linked inputs changed')
    if current_harness:E.require(value['harness_sha256']==E.identity(E.manifest(harness=True)),'final host harness changed')
    subjects={
        'primitives':(['scripts/tlb-shootdown-host.c','kernel/src/apic/lapic_delivery.c','kernel/src/memory/tlb_interval.c',
                      'kernel/src/memory/tlb_deferred.c','kernel/src/smp/cpu_request_state.c'],[]),
        'runtime':(['scripts/tlb-runtime-host.c','kernel/src/apic/lapic_delivery.c','kernel/src/memory/tlb_interval.c',
                    'kernel/src/smp/cpu_request_state.c','kernel/src/smp/cpu_request.c'],['-DHOBBYOS_DEBUG_ASSERT=0']),
        'deferred-worker':(['scripts/tlb-deferred-worker-host.c','kernel/src/memory/kernel_tlb.c',
                            'kernel/src/memory/tlb_deferred.c','kernel/src/memory/tlb_interval.c'],
                           ['-UHOBBYOS_SELFTEST','-DHOBBYOS_DEBUG_ASSERT=0'])}
    expected={(mode,subject) for mode in ('normal','ubsan','asan') for subject in subjects}
    E.require(len(rows)==len(expected) and {(r['mode'],r['subject']) for r in rows}==expected,'host coverage missing/duplicate')
    for row in rows:
        built,_=C.verify_command(evidence,row['compile']);ran,output=C.verify_command(evidence,row['execute'])
        sources,extra=subjects[row['subject']]
        flags={'normal':[],'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
               'asan':['-fsanitize=address','-fno-omit-frame-pointer']}[row['mode']]
        argv=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread','-fno-pie','-no-pie',
              '-DHOBBYOS_SELFTEST=1',*flags,*extra,*sources,'-o']
        E.require(built['argv'][:-1]==argv and ran['argv']==[built['argv'][-1]] and
                  built['harness_sha256']==ran['harness_sha256']==value['harness_sha256'],
                  'actual invalidation C compile/execute differs')
        E.require(output==row['stdout'] and 'status=PASS' in output and 'privileged_operations=SUBSTITUTED' in output,
                  'host raw result differs')
    C.verify_host(evidence,current_harness)
    inherited=evidence/value['result']['inherited'];E.safe(value['result']['inherited'])
    retained=json.loads((inherited/'host.json').read_text())
    E.require(retained['source_sha256']==E.identity(E.manifest()) and
              retained['harness_sha256']==value['harness_sha256'] and retained['status']=='PASS' and
              retained['profiles']==['normal','ubsan','asan'] and
              retained['subjects']==['shared-decoder','bitmap-integrity','pmem-integrity','cpu-mailbox','allocator-reference'],
              'retained actual C host inventory differs')
    attempts=list((inherited/'attempts').glob('*/command.json'))
    E.require(len(attempts)==30,'retained actual C command coverage differs')
    for path in attempts:C.verify_command(inherited,path.parent.relative_to(inherited).as_posix())
    return {'profiles':['normal','ubsan','asan'],'subjects':list(subjects),'retained':retained['subjects']}

def compare_runtime(recorded,observed,transport=None):
    for scope in ('tlb-matrix','nx-disabled','xapic'):
        E.require({k:v for k,v in recorded[scope]['series'].items() if k!='replay_verifier_sha256'}==
                  {k:v for k,v in observed[scope]['series'].items() if k!='replay_verifier_sha256'} and
                  len(recorded[scope]['runs'])==len(observed[scope]['runs']),'runtime series replay differs')
        for old,new in zip(recorded[scope]['runs'],observed[scope]['runs']):
            C.verify_mode_run(old['policy'],new['policy'],transport)
            E.require({k:v for k,v in old.items() if k!='policy'}=={k:v for k,v in new.items() if k!='policy'},
                      'invalidation raw workload replay differs')
    E.require(recorded['expected_failures']==observed['expected_failures'],'isolated failure replay differs')

def verify(evidence,transport=None):
    E.require(json.loads((S.static_base(evidence)/'test-plan.json').read_text())==test_plan(),'frozen invalidation workload plan differs')
    if transport is None:T.scope('final-acceptance')
    T.audit_sites();T.caller_semantics();G.verify_static(evidence)
    receipts={name:json.loads(((S.static_base(evidence) if name=='tlb-static' else S.host_base(evidence) if name=='tlb-host' else evidence)/(name+'.json')).read_text()) for name in
              ('tlb-static','tlb-host','tlb-build','tlb-runtime','tlb-production','tlb-fixtures')}
    for name,row in receipts.items():
        E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'stale/missing mode input: '+name)
    for name in ('debug','production','selftest'):
        E.require(verify_codegen(evidence/'candidates'/name,name!='selftest',transport)==receipts['tlb-build']['result'][name],
                  'invalidation build receipt differs')
    verify_host(evidence)
    runtime=verify_runtime(evidence)
    compare_runtime(receipts['tlb-runtime']['result'],runtime,transport)
    production=[V.verify_run(evidence/name) for name in json.loads((evidence/'production.json').read_text())['runs']]
    E.require(len(production)==2 and {r['policy']['profile'] for r in production}=={'q35-tcg-smp1','q35-kvm-smp4'},
              'production profile coverage')
    for old,new in zip(receipts['tlb-production']['result'],production):
        C.verify_mode_run(old['policy'],new['policy'],transport)
        E.require(not new['workloads'],'production test workload leaked')
    fixture=receipts['tlb-fixtures'];E.require(fixture['harness_sha256']==E.identity(E.manifest(harness=True)),
        'final fixtures used a different verifier')
    directory=evidence/fixture['result']['path']
    E.require(E.sha(directory/'results.json')==fixture['result']['sha256'],'fixture results changed')
    module('tlb-shootdown-fixtures').verify_receipt(json.loads((directory/'results.json').read_text()))
    E.require(E.sha(directory/'policy/results.json')==fixture['result']['policy_sha256'],'policy fixture result changed')
    module('cpu-control-fixtures').verify_receipt(json.loads((directory/'policy/results.json').read_text()))
    final_host=verify_host(directory,True)
    regression=(module('memory-current-regression').verify_ledger(evidence,E.identity(E.manifest())) if transport is None
                else module('memory-portable-replay').regression_ledger(evidence,transport))
    regression['historical']=json.loads((evidence/'regression-ledger.json').read_text())['historical_unavailable']
    G.verify_baseline(evidence,O.verify_series(evidence))
    return {'status':'PASS','runtime':runtime,'production':production,'regression':regression,
            'host':final_host,'normal_boots':21,'nx_disabled_boots':3,'xapic_supplemental_boots':3,
            'hardware':'DEFERRED_TO_FINAL_CLOSURE','independent_review':'PENDING'}

def begin_harness_reverification(evidence):
    """Preserve a completed verdict before replaying unchanged captured inputs."""
    path=evidence/'tlb-acceptance.json'
    if not path.exists():return None
    prior=json.loads(path.read_text())
    E.require(prior['build_input_sha256']==E.identity(E.manifest()),
              'harness re-verification cannot reuse changed build inputs')
    current=E.identity(E.manifest(harness=True))
    if prior['harness_sha256']==current:return None
    directory=evidence/'replay-revisions'/(G.stamp()+'-invalidation-verifier');directory.mkdir(parents=True)
    names=[name for name in ('tlb-acceptance.json','tlb-fixtures.json','acceptance.json') if (evidence/name).exists()]
    rows=[{'path':name,'sha256':E.sha(evidence/name),'size':(evidence/name).stat().st_size} for name in names]
    E.write(directory/'start.json',{'schema':1,'status':'REPLAY_PENDING','build_input_sha256':prior['build_input_sha256'],
        'previous_harness_sha256':prior['harness_sha256'],'harness_sha256':current,'archived_receipts':rows,
        'raw_capture_policy':'Unchanged raw captures require independent replay; no new boot is claimed.'})
    for row in rows:
        original=evidence/row['path'];E.require(E.sha(original)==row['sha256'],'prior verdict changed during preservation')
        original.rename(directory/row['path'])
    return directory

def finalize(evidence):
    series=collect_baseline(evidence)
    derived=verify(evidence)
    fixtures=json.loads((evidence/'tlb-fixtures.json').read_text())['result']['inherited']
    # This receipt is produced by the same strict native contract used by the
    # earlier gate, after the additional coherence predicate has also replayed.
    generic={'status':'PASS','contract':'memory-current-regression-v1','series':series,
             'baseline_replay':G.verify_baseline(evidence,series),'fixtures':fixtures,
             'hardware':'DEFERRED_TO_FINAL_CLOSURE'}
    if (evidence/'acceptance.json').exists():
        E.require(json.loads((evidence/'acceptance.json').read_text())==generic,'existing generic acceptance differs')
    else:E.write(evidence/'acceptance.json',generic)
    G.verify_acceptance(evidence)
    if (evidence/'tlb-acceptance.json').exists():
        previous=json.loads((evidence/'tlb-acceptance.json').read_text())
        E.require(previous['status']=='PASS' and previous['result']==derived and
                  previous['build_input_sha256']==E.identity(E.manifest()) and
                  previous['harness_sha256']==E.identity(E.manifest(harness=True)),
                  'existing invalidation acceptance differs')
    else:receipt(evidence,'tlb-acceptance',derived)

def storage_admission(evidence):
    # The retained native campaign alone expands to multiple GiB. Keep room
    # for its real outputs; a storage failure must not truncate an attempt.
    usage=shutil.disk_usage(evidence);required=4*1024*1024*1024
    path=evidence/'resource-attempts'/G.stamp();path.mkdir(parents=True)
    result={'schema':1,'status':'AVAILABLE' if usage.free>=required else 'INSUFFICIENT',
            'path':str(evidence),'free_bytes':usage.free,'required_free_bytes':required,
            'total_bytes':usage.total,'build_input_sha256':E.identity(E.manifest()),
            'reason':'Complete current native campaigns and immutable new VM evidence require persistent storage.',
            'utc':G.stamp()}
    E.write(path/'storage.json',result)
    E.require(usage.free>=required,'insufficient persistent storage for complete current campaigns; see '+str(path))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    parser.add_argument('--evidence',type=Path,required=True);args=parser.parse_args();os.chdir(ROOT)
    evidence=args.evidence.resolve();evidence.relative_to(ROOT/'backups');evidence.mkdir(parents=True,exist_ok=True)
    if args.mode=='all':
        storage_admission(evidence)
        begin_harness_reverification(evidence)
        for name in ('static','host','build','runtime','production'):
            if not (evidence/('tlb-'+name+'.json')).exists():globals()[name](evidence)
        G.run(evidence,'current-inherited-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(evidence)])
        if not (evidence/'tlb-fixtures.json').exists():fixtures(evidence)
        finalize(evidence)
    elif args.mode=='verify':print(json.dumps(verify(evidence),sort_keys=True))
    else:globals()[args.mode](evidence)
    print(json.dumps({'mode':args.mode,'status':'PASS','evidence':str(evidence)}))

if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
