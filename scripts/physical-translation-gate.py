#!/usr/bin/env python3
"""Checked translation acceptance over the existing acknowledged-memory campaign."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import memory_evidence as E
import physical_current as P
import validation_resources as R
import validation_stages as S
import stack_current as STACK

ROOT=Path(__file__).resolve().parent.parent
os.environ.setdefault('HOBBYOS_VALIDATION_CONTRACT','physical-translation')
os.environ['PYTHONDONTWRITEBYTECODE']='1'

def module(name):
    spec=importlib.util.spec_from_file_location('physical_gate_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

T=module('tlb-shootdown-gate');G=T.G;V=module('verify-physical-translation')
T.COMMANDS=['phystest',*T.COMMANDS,'phystest']
G.SURFACE+=('cmd_phystest','physical_fixture','physical_host_backing','g_fixture_owner')

def receipt(evidence,name,result):
    T.receipt(evidence,'physical-'+name,result)

def static(evidence):
    T.static(evidence)
    result=P.scope('physical-boundary');result['boundary']=P.boundary_semantics()
    result['conversion_inventory']=module('physical-source-inventory').verify()
    receipt(evidence,'static',result)

def host(evidence):
    if not (evidence/'tlb-host.json').exists():T.host(evidence)
    directory=evidence/'physical-host'/G.stamp();directory.mkdir(parents=True)
    subjects=[('boundary',['scripts/physical-translation-host.c','kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/utils/bitmap.c'],[]),
        ('boot',['scripts/physical-boot-host.c','kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/memory/physical_translation.c',*STACK.hosted_inverse_sources()],
         ['-mcmodel=large','-Wl,--defsym,_kernel_start=0xffffffff82000000,--defsym,_kernel_end=0xffffffff82400000',
          *(['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0'] if STACK.enabled() else [])]),
        ('pmm',['scripts/physical-pmm-host.c','kernel/src/utils/bitmap.c'],
         ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0'])]
    rows=[]
    for mode,flags in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        for subject,sources,extra in subjects:
            binary=directory/(subject+'-'+mode)
            compile=G.run(evidence,'physical-'+subject+'-'+mode+'-compile',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                '-fno-pie','-no-pie',*flags,*extra,*sources,'-o',str(binary)])
            execute=G.run(evidence,'physical-'+subject+'-'+mode,[str(binary)],{'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1'})
            output=(execute/'stdout.log').read_text()
            E.require('PASS' in output,'actual translation host summary missing')
            rows.append({'subject':subject,'mode':mode,'compile':compile.relative_to(evidence).as_posix(),
                         'execute':execute.relative_to(evidence).as_posix(),'output_sha256':E.sha(execute/'stdout.log'),
                         'binary_sha256':E.sha(binary),'binary':binary.relative_to(evidence).as_posix()})
    receipt(evidence,'host',rows)

def build(evidence):
    T.build(evidence);facts={}
    for name in ('selftest','debug','production'):
        directory=evidence/'candidates'/name
        symbols=(directory/'symbols.txt').read_text()
        strings=subprocess.check_output(['strings',str(directory/'kernel.elf')],text=True)
        if name!='selftest':V.production(symbols,strings)
        else:E.require('cmd_phystest' in symbols and 'physical_fixture_run' in symbols,'translation guest fixture missing')
        facts[name]={'elf_sha256':E.sha(directory/'kernel.elf'),'symbols_sha256':E.sha(directory/'symbols.txt'),
            'boundary_symbols':{symbol:symbol in symbols for symbol in ('physical_boot_adapt','physical_pointer_range',
                'physical_address_range','bitmap_find_zero_run_below','paging_kernel_root_physical','paging_query_range')}}
        E.require(all(facts[name]['boundary_symbols'].values()),'production translation implementation missing')
    receipt(evidence,'build',facts)

def selected_runs(evidence):
    result=[]
    for directory in (evidence,evidence/'nx-disabled',evidence/'xapic'):
        series=json.loads((directory/'series.json').read_text())
        result.extend(directory/name for name in series['runs']+series['diagnostics'])
    return result

def runtime(evidence,validation=None):
    T.runtime(evidence,validation=validation)
    rows=[{'path':path.relative_to(evidence).as_posix(),'result':V.verify_run(path)} for path in selected_runs(evidence)]
    receipt(evidence,'runtime',rows)

def production(evidence):
    candidate=evidence/'candidates/production';G.verify_local_candidate(candidate);runs=[]
    for accel,smp in [('tcg',1),('kvm',4)]:
        name=f'production/q35-{accel}-smp{smp}';directory=evidence/name
        argv=['python3','-B','scripts/memory-qemu.py','--evidence',str(directory),'--candidate',str(candidate),
              '--machine','q35','--accel',accel,'--smp',str(smp),'--serial-backend','file','--production']
        for command in ['ptinspect','cpustate','memmap','tlbtest','phystest','mem','cpu','irq check']:argv+=['--command',command]
        if not directory.exists():G.run(evidence,'production-'+accel,argv)
        V.verify_run(directory);runs.append(name)
    E.write(evidence/'production.json',{'status':'PASS','candidate_id':G.verify_local_candidate(candidate),'runs':runs})
    T.receipt(evidence,'tlb-production',[T.V.verify_run(evidence/name) for name in runs])
    receipt(evidence,'production',[V.verify_run(evidence/name) for name in runs])

def fixtures(evidence):
    if not (evidence/'tlb-fixtures.json').exists():T.fixtures(evidence)
    directory=evidence/'physical-fixtures'/G.stamp()
    G.run(evidence,'physical-fixtures',['python3','-B','scripts/physical-translation-fixtures.py','--evidence',str(evidence),'--output',str(directory)])
    host(directory)
    receipt(evidence,'fixtures',{'path':directory.relative_to(evidence).as_posix(),'sha256':E.sha(directory/'results.json'),
            'host_receipt_sha256':E.sha(directory/'physical-host.json')})

def verify_host(evidence,current_harness=False):
    evidence=S.host_base(evidence)
    row=json.loads((evidence/'physical-host.json').read_text());seen=set()
    E.require(row['build_input_sha256']==E.identity(E.manifest()) and row['status']=='PASS','stale translation host inputs')
    if current_harness:E.require(row['harness_sha256']==E.identity(E.manifest(harness=True)),'translation host verifier version stale')
    for item in row['result']:
        key=(item['subject'],item['mode']);E.require(key not in seen,'duplicate translation host case');seen.add(key)
        subjects={
            'boundary':(['scripts/physical-translation-host.c','kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/utils/bitmap.c'],[],
                'PHYSICAL_TRANSLATION_HOST status=PASS checks=313260 privileged_operations=NONE\n'),
            'boot':(['scripts/physical-boot-host.c','kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/memory/physical_translation.c',*STACK.hosted_inverse_sources()],
                ['-mcmodel=large','-Wl,--defsym,_kernel_start=0xffffffff82000000,--defsym,_kernel_end=0xffffffff82400000',
                 *(['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0'] if STACK.enabled() else [])],
                'status=PASS cases=14 scope=actual_boot_adapter physical_backing=SUBSTITUTED nonidentity_host_buffers=1\n'),
            'pmm':(['scripts/physical-pmm-host.c','kernel/src/utils/bitmap.c'],
                ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0'],
                'status=PASS checks=176 logical_frames=268435456 scope=actual_pmm_wrappers privileged_operations=SUBSTITUTED\n')}
        E.require(item['subject'] in subjects,'unknown actual C translation subject')
        sources,extra,summary=subjects[item['subject']]
        flags={'normal':[],'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
               'asan':['-fsanitize=address','-fno-omit-frame-pointer']}[item['mode']]
        built,compile_output=T.C.verify_command(evidence,item['compile']);ran,output=T.C.verify_command(evidence,item['execute'])
        expected=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',*flags,*extra,*sources,'-o']
        E.require(built['argv'][:-1]==expected and ran['argv']==[built['argv'][-1]] and
                  built['harness_sha256']==ran['harness_sha256']==row['harness_sha256'] and output==summary,
                  'actual translation C compile/execute/independent checks differ')
        for phase in ('compile','execute'):
            directory=evidence/item[phase];command=json.loads((directory/'command.json').read_text())
            E.require(command['exit_status']==0 and command['build_input_sha256']==row['build_input_sha256'] and
                      command['stdout_sha256']==E.sha(directory/'stdout.log'),'translation host raw command failed or changed')
        E.require(E.sha(evidence/item['execute']/'stdout.log')==item['output_sha256'],'translation host output differs')
    E.require(seen=={(subject,mode) for subject in ('boundary','boot','pmm') for mode in ('normal','ubsan','asan')},
              'translation host sanitizer/subject omitted')
    return {'cases':len(seen),'status':'PASS'}

def verify(evidence,transport=None):
    inherited=T.verify(evidence,transport);P.audit_sites();P.boundary_semantics()
    reference_root=(transport.root/'preservation/paging-baselines' if transport else
                    evidence.parent.parent/'before/paging-baselines')
    preserved_baselines=V.baseline_references(reference_root)
    module('physical-source-inventory').verify();host_result=verify_host(evidence)
    rows=[V.verify_run(path) for path in selected_runs(evidence)]
    smokes=[V.verify_run(evidence/name) for name in json.loads((evidence/'production.json').read_text())['runs']]
    receipts={}
    for name in ('static','build','runtime','production','fixtures'):
        row=json.loads(((S.static_base(evidence) if name=='static' else evidence)/('physical-'+name+'.json')).read_text())
        E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'missing/stale physical mode: '+name)
        receipts[name]=row
    recorded=receipts['runtime']['result']
    E.require([r['path'] for r in recorded]==[p.relative_to(evidence).as_posix() for p in selected_runs(evidence)],
              'physical selected run receipt omitted/reordered')
    def compare(old,new):
        T.C.verify_mode_run(old['invalidation']['policy'],new['invalidation']['policy'],transport)
        E.require(old==new,'physical semantic replay differs from collected result')
    for old,new in zip(recorded,rows):compare(old['result'],new)
    E.require(len(receipts['production']['result'])==len(smokes)==2,'physical production receipt coverage')
    for old,new in zip(receipts['production']['result'],smokes):compare(old,new)
    for name in ('selftest','debug','production'):
        directory=evidence/'candidates'/name;fact=receipts['build']['result'][name]
        candidate=json.loads((directory/'candidate.json').read_text())
        E.require(fact['elf_sha256']==candidate['artifacts']['kernel.elf']['sha256'] and
                  fact['symbols_sha256']==E.sha(directory/'symbols.txt'),'physical build symbol/binary binding')
        symbols=(directory/'symbols.txt').read_text()
        E.require(all(v and k in symbols for k,v in fact['boundary_symbols'].items()) and
                  set(fact['boundary_symbols'])=={'physical_boot_adapt','physical_pointer_range','physical_address_range',
                     'bitmap_find_zero_run_below','paging_kernel_root_physical','paging_query_range'},'physical build enforcement omitted')
        if name!='selftest':V.production(symbols,(directory/'compiled-strings.txt').read_text())
    fixture=json.loads((evidence/'physical-fixtures.json').read_text())
    E.require(fixture['harness_sha256']==E.identity(E.manifest(harness=True)),'translation fixtures used stale verifier')
    path=evidence/fixture['result']['path']/'results.json'
    E.require(E.sha(path)==fixture['result']['sha256'],'translation fixture receipt changed')
    module('physical-translation-fixtures').verify_receipt(json.loads(path.read_text()))
    E.require(E.sha(path.parent/'physical-host.json')==fixture['result']['host_receipt_sha256'],'final translation host receipt differs')
    verify_host(path.parent,True)
    return {'status':'PASS','host':host_result,'runtime':rows,'production':smokes,'inherited':inherited,
            'preserved_baselines':preserved_baselines,
            'hardware':'DEFERRED_TO_FINAL_CLOSURE','independent_review':'PENDING'}

def storage_admission(evidence,owner=None):
    owner=owner or sys.modules[__name__]
    prefix='device' if owner is not sys.modules[__name__] else 'physical'
    states={name:S.stage(owner,evidence,prefix,name) for name in ('build','runtime','fixtures')}
    states['native']=S.ledger(owner,evidence)
    # Native captures previously occupied 1.74 GB; retain a 2 GiB durable
    # allowance and the existing 4 GiB admission floor separately. The complete
    # device matrix needs more boots than the inherited translation matrix.
    durable={'build':512*R.MIB,'runtime':(3 if prefix=='device' else 2)*R.GIB,
             'native':2*R.GIB,'fixtures':512*R.MIB}
    pending=any(r['state']!='REUSABLE' for r in states.values())
    return R.admit(evidence,stages=states,durable=durable,
        temporary=256*R.MIB if pending else 0,scratch=4*R.GIB if pending else 0,
        packaging=512*R.MIB if pending else 0,
        minimum=R.NATIVE_FLOOR if states['native']['state']!='REUSABLE' else 0)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    p.add_argument('--evidence',type=Path,required=True);args=p.parse_args();os.chdir(ROOT)
    evidence=args.evidence.resolve();evidence.relative_to(ROOT/'backups');evidence.mkdir(parents=True,exist_ok=True)
    if args.mode=='all':
        storage_admission(evidence)
        if (evidence/'physical-acceptance.json').exists():
            result=verify(evidence)
            E.require(json.loads((evidence/'physical-acceptance.json').read_text())['result']==result,
                      'preserved acceptance differs; use a successor destination')
            print(json.dumps(result,sort_keys=True));return
        for name in ('static','host','build','runtime','production'):
            if not R.require_successor(S.stage(sys.modules[__name__],evidence,'physical',name)):
                if name in ('build','runtime'):R.before_step(evidence,name)
                globals()[name](evidence)
        if not R.require_successor(S.ledger(sys.modules[__name__],evidence)):
            G.run(evidence,'current-inherited-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(evidence)])
        if not R.require_successor(S.stage(sys.modules[__name__],evidence,'physical','fixtures')):
            R.before_step(evidence,'fixtures');fixtures(evidence)
        T.finalize(evidence);receipt(evidence,'acceptance',verify(evidence))
    elif args.mode=='verify':print(json.dumps(verify(evidence),sort_keys=True))
    else:
        if args.mode in ('build','runtime','production','fixtures'):
            R.before_step(evidence,'runtime' if args.mode=='production' else args.mode)
        globals()[args.mode](evidence)

if __name__=='__main__':main()
