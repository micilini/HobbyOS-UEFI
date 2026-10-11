#!/usr/bin/env python3
"""Compile and execute the actual image policy, walker and exact probe decisions."""
import argparse
import json
import importlib.util
from pathlib import Path
import memory_evidence as E
import stack_current as STACK

EXPECTED={'permissions':'IMAGE_PERMISSION_HOST checks=271093 status=PASS\n',
          'probes':'PROTECTION_PROBE_HOST checks=4762 status=PASS concurrent_targets=24 schedules=2 hardware_faults=NOT_HOSTED\n',
          'production':'PROTECTION_PRODUCTION_HOST checks=766 status=PASS test_operations=REJECTED\n'}
if STACK.enabled(): EXPECTED['permissions']='IMAGE_PERMISSION_HOST checks=299065 status=PASS\n'

SUBJECTS={
    'permissions':['scripts/kernel-wx-host.c','kernel/src/memory/ram_map_plan.c',
        'kernel/src/memory/physical_contract.c','kernel/src/memory/paging_entry.c',
        'kernel/src/memory/paging_build.c','kernel/src/memory/image_permissions.c',
        'kernel/src/memory/image_walk.c','kernel/src/memory/mapping_core.c',*STACK.hosted_stack_sources()],
    'probes':['scripts/kernel-wx-probe-host.c','scripts/kernel-wx-probe-abi.S','kernel/src/memory/protection_probe_match.c',
        'kernel/src/memory/protection_probe_access.S','kernel/src/smp/cpu_request_state.c',
        'kernel/src/memory/tlb_interval.c'],
    'production':['scripts/kernel-wx-production-host.c','kernel/src/smp/cpu_request_state.c',
        'kernel/src/memory/tlb_interval.c']}

def run(output):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('image_host_gate',E.ROOT/'scripts/memory-observation-gate.py')
    gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
    rows=[]
    for mode,extra in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        for subject,sources in SUBJECTS.items():
            binary=output/(subject+'-'+mode)
            argv=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
                  *(['-pthread'] if subject=='probes' else []),
                  *([] if subject=='production' else ['-DHOBBYOS_SELFTEST=1']),*extra,*sources,'-o',str(binary)]
            compiled=gate.run(output,binary.name+'-compile',argv)
            executed=gate.run(output,binary.name,[str(binary)],{
                'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            E.require((executed/'stdout.log').read_text()==EXPECTED[subject],'image actual C coverage/result differs')
            rows.append({'subject':subject,'mode':mode,'compile':compiled.relative_to(output).as_posix(),
                'execute':executed.relative_to(output).as_posix(),'stdout_sha256':E.sha(executed/'stdout.log'),
                'binary_sha256':E.sha(binary),'summary':(executed/'stdout.log').read_text()})
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'executions':rows,
        'authority':'Actual hosted C and assembly; matcher fault frames synthetic; no guest hardware-fault claim.'}
    E.write(output/'image-host.json',result);return result

def verify(directory):
    spec=importlib.util.spec_from_file_location('image_host_verify',E.ROOT/'scripts/cpu-control-state-gate.py')
    gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
    row=json.loads((directory/'image-host.json').read_text());seen=set()
    E.require(row['schema']==1 and row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'image host input/schema')
    for case in row['executions']:
        pair=case['subject'],case['mode'];E.require(pair not in seen,'image duplicate hosted execution');seen.add(pair)
        subject,mode=pair;compiled,_=gate.verify_command(directory,case['compile']);executed,output=gate.verify_command(directory,case['execute'])
        argv=compiled['argv']
        E.require(argv[0]=='gcc' and executed['argv']==[argv[-1]] and all(p in argv for p in SUBJECTS[subject]) and
            compiled['build_input_sha256']==executed['build_input_sha256']==row['build_input_sha256'] and
            output==case['summary']==EXPECTED[subject] and E.sha(directory/case['execute']/'stdout.log')==case['stdout_sha256'] and
            ('-fsanitize=address' in argv)==(mode=='asan') and ('-fsanitize=undefined' in argv)==(mode=='ubsan') and
            '-O2' in argv and '-Werror' in argv and ('-DHOBBYOS_SELFTEST=1' in argv)==(subject!='production') and
            ('-pthread' in argv)==(subject=='probes'),
            'image actual compile/execution/sanitizer coverage')
    E.require(seen=={(s,m) for s in SUBJECTS for m in ('normal','ubsan','asan')},'image missing hosted suite')
    return {'status':'PASS','executions':len(seen),'subjects':sorted(SUBJECTS),'hardware_faults':'NOT_HOSTED'}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    print(E.canonical(run(parser.parse_args().output)).decode())
