#!/usr/bin/env python3
"""Actual RAM construction, feature admission and pointer-publication tests."""
import argparse
import importlib.util
from pathlib import Path
import memory_evidence as E
import low_current
import kernel_image_current as WX
import stack_current as STACK

def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def run(output):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    gate=module('memory-observation-gate');rows=[]
    shared=['kernel/src/memory/ram_map_plan.c','kernel/src/memory/physical_contract.c']
    for mode,extra in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        flags=['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
               '-ffunction-sections','-fdata-sections',*extra]
        policy=output/('policy-'+mode+'.o')
        gate.run(output,'policy-'+mode,['gcc',*flags,'-DHOBBYOS_CONTROL_POLICY_HOST_TEST','-c',
            'kernel/src/cpu/control_policy.c','-o',str(policy)])
        subjects=[('construction',['scripts/direct-map-host.c',*shared,'kernel/src/memory/paging_entry.c',
            'kernel/src/memory/paging_build.c','kernel/src/memory/image_permissions.c','kernel/src/memory/mapping_core.c',*STACK.hosted_stack_sources()]),
            ('feature',['scripts/direct-map-boot-host.c','kernel/src/memory/ram_map_boot.c',*shared,
                'kernel/src/memory/cache_attributes.c',str(policy)]),
            ('transition',['scripts/direct-map-transition-host.c',*shared,'kernel/src/memory/physical_translation.c',*STACK.hosted_inverse_sources()])]
        for subject,sources in subjects:
            binary=output/(subject+'-'+mode)
            built=gate.run(output,binary.name+'-build',['gcc',*flags,'-DHOBBYOS_DEBUG_ASSERT=0',
                '-Wl,--gc-sections',*sources,'-o',str(binary)])
            executed=gate.run(output,binary.name,[str(binary)],{'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1',
                'UBSAN_OPTIONS':'halt_on_error=1'})
            summary=(executed/'stdout.log').read_text()
            E.require('status=PASS' in summary,'RAM actual C summary absent')
            rows.append({'subject':subject,'mode':mode,'compile':built.relative_to(output).as_posix(),
                'execute':executed.relative_to(output).as_posix(),'stdout_sha256':E.sha(executed/'stdout.log'),
                'binary_sha256':E.sha(binary),'summary':summary})
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'subjects':rows,'authority':'Actual hosted C; privileged seams only substituted'}
    E.write(output/'direct-host.json',result);return result

def verify(directory):
    import json
    gate=module('cpu-control-state-gate')
    record=json.loads((directory/'direct-host.json').read_text())
    E.require(record['schema']==1 and record['status']=='PASS' and
        record['build_input_sha256']==E.identity(E.manifest()),'RAM hosted source/schema')
    expected={'construction':'DIRECT_MAP_HOST checks=470364 status=PASS\n',
        'feature':'status=PASS cases=16 scope=actual_boot_feature_cache_selection privileged_reads=HOSTED\n',
        'transition':'status=PASS cases=12 scope=actual_bitmap_pointer_and_coverage_publication original_storage_unchanged=1 logical_frames=268435456 privileged_locks=HOSTED\n'}
    if low_current.enabled(): expected['construction']='DIRECT_MAP_HOST checks=414994 status=PASS\n'
    if WX.enabled(): expected['construction']='DIRECT_MAP_HOST checks=414996 status=PASS\n'
    if STACK.enabled(): expected['construction']='DIRECT_MAP_HOST checks=504555 status=PASS\n'
    subjects={'construction':['scripts/direct-map-host.c','kernel/src/memory/ram_map_plan.c',
        'kernel/src/memory/physical_contract.c','kernel/src/memory/paging_entry.c',
        'kernel/src/memory/paging_build.c','kernel/src/memory/image_permissions.c','kernel/src/memory/mapping_core.c',*STACK.hosted_stack_sources()],
        'feature':['scripts/direct-map-boot-host.c','kernel/src/memory/ram_map_boot.c',
        'kernel/src/memory/ram_map_plan.c','kernel/src/memory/physical_contract.c','kernel/src/memory/cache_attributes.c'],
        'transition':['scripts/direct-map-transition-host.c','kernel/src/memory/ram_map_plan.c',
        'kernel/src/memory/physical_contract.c','kernel/src/memory/physical_translation.c',*STACK.hosted_inverse_sources()]}
    seen=set()
    for row in record['subjects']:
        pair=(row['subject'],row['mode']);E.require(pair not in seen,'RAM duplicate hosted case');seen.add(pair)
        compiled,_=gate.verify_command(directory,row['compile']);executed,output=gate.verify_command(directory,row['execute'])
        argv=compiled['argv'];mode=row['mode']
        E.require(argv[0]=='gcc' and executed['argv']==[argv[-1]] and
            compiled['build_input_sha256']==executed['build_input_sha256']==record['build_input_sha256'] and
            output==row['summary']==expected[row['subject']] and
            all(name in argv for name in subjects[row['subject']]) and
            ('-fsanitize=address' in argv)==(mode=='asan') and
            ('-fsanitize=undefined' in argv)==(mode=='ubsan') and '-O2' in argv and '-Werror' in argv,
            'RAM actual C compiler/execution/expected case result')
        E.require(E.sha(directory/row['execute']/'stdout.log')==row['stdout_sha256'],'RAM hosted raw hash')
    E.require(seen=={(s,m) for s in expected for m in ('normal','ubsan','asan')},'RAM missing hosted subject or sanitizer')
    return {'status':'PASS','executions':len(seen),'subjects':sorted(expected)}

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path)
    print(E.canonical(run(p.parse_args().output)).decode())
