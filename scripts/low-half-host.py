#!/usr/bin/env python3
"""Actual no-low builder, bootstrap ownership, reservation and fault decisions."""
import argparse
import importlib.util
import json
from pathlib import Path
import memory_evidence as E
import kernel_image_current as WX
import stack_current as STACK

def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

SHARED=['kernel/src/memory/'+n+'.c' for n in ('ram_map_plan','physical_contract','paging_entry','paging_build','image_permissions','mapping_core')]+STACK.hosted_stack_sources()
SUBJECTS={
    'root':['scripts/low-address-host.c',*SHARED],
    'bootstrap':['scripts/bootstrap-host.c',*SHARED,*['kernel/src/memory/'+n+'.c' for n in
        ('bootstrap_claim','bootstrap_tables','bootstrap_lifetime','null_probe_match','low_fault')]],
    'reservation':['scripts/bootstrap-pmm-host.c',*['kernel/src/memory/'+n+'.c' for n in
        ('ram_map_plan','physical_contract','physical_translation','bootstrap_claim')],'kernel/src/utils/bitmap.c',*STACK.hosted_inverse_sources()]}
EXPECTED={
    'root':'LOW_ADDRESS_HOST checks=1853 status=PASS\n',
    'bootstrap':'BOOTSTRAP_HOST checks=2345 status=PASS\n',
    'reservation':'status=PASS cases=12 scope=actual_bitmap_pointer_and_coverage_publication original_storage_unchanged=1 logical_frames=268435456 privileged_locks=HOSTED\nBOOTSTRAP_PMM_HOST status=PASS cases=15 full_frames=268435456 reservation=ADOPTED free_count=UNCHANGED if=RESTORED\n'}

if WX.enabled() or STACK.enabled():
    EXPECTED['root']='LOW_ADDRESS_HOST checks=1854 status=PASS\n'
    EXPECTED['bootstrap']='BOOTSTRAP_HOST checks=2346 status=PASS\n'
if STACK.enabled():
    EXPECTED['root']='LOW_ADDRESS_HOST checks=2016 status=PASS\n'
    EXPECTED['bootstrap']='BOOTSTRAP_HOST checks=2562 status=PASS\n'

def run(output):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    gate=module('memory-observation-gate');rows=[]
    for mode,extra in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        for subject,sources in SUBJECTS.items():
            binary=output/(subject+'-'+mode)
            flags=['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
                   '-ffunction-sections','-fdata-sections','-DHOBBYOS_DEBUG_ASSERT=0',*extra]
            compiled=gate.run(output,binary.name+'-build',['gcc',*flags,'-Wl,--gc-sections',*sources,'-o',str(binary)])
            executed=gate.run(output,binary.name,[str(binary)],
                {'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            summary=(executed/'stdout.log').read_text()
            E.require(summary==EXPECTED[subject],'low-domain hosted case inventory/result differs')
            rows.append({'subject':subject,'mode':mode,'compile':compiled.relative_to(output).as_posix(),
                'execute':executed.relative_to(output).as_posix(),'summary':summary,
                'binary_sha256':E.sha(binary),'stdout_sha256':E.sha(executed/'stdout.log')})
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'subjects':rows,
        'authority':'Actual hosted production decisions; privileged operations substituted only in host fixtures'}
    E.write(output/'low-host.json',result);return result

def verify(directory):
    gate=module('cpu-control-state-gate');row=json.loads((directory/'low-host.json').read_text());seen=set()
    E.require(row['schema']==1 and row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),
        'low-domain hosted source/schema')
    for case in row['subjects']:
        pair=(case['subject'],case['mode']);E.require(pair not in seen,'duplicate low-domain hosted subject');seen.add(pair)
        compiled,_=gate.verify_command(directory,case['compile']);executed,output=gate.verify_command(directory,case['execute'])
        argv=compiled['argv'];subject,mode=pair
        E.require(argv[0]=='gcc' and executed['argv']==[argv[-1]] and
            compiled['build_input_sha256']==executed['build_input_sha256']==row['build_input_sha256'] and
            all(p in argv for p in SUBJECTS[subject]) and output==case['summary']==EXPECTED[subject] and
            E.sha(directory/case['execute']/'stdout.log')==case['stdout_sha256'] and
            ('-fsanitize=address' in argv)==(mode=='asan') and
            ('-fsanitize=undefined' in argv)==(mode=='ubsan') and '-O2' in argv and '-Werror' in argv,
            'low-domain actual compile/run/flags/expected results')
    E.require(seen=={(s,m) for s in SUBJECTS for m in ('normal','ubsan','asan')},'missing low-domain hosted suite')
    return {'status':'PASS','executions':len(seen),'subjects':sorted(SUBJECTS)}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True,type=Path)
    print(E.canonical(run(parser.parse_args().output)).decode())
