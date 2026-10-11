#!/usr/bin/env python3
"""Execute actual stack ownership, tables, runtime adapters and fault decisions."""
import argparse
import importlib.util
import json
from pathlib import Path
import memory_evidence as E

MEM='kernel/src/memory/'
SUBJECTS={
    'owners':['scripts/stack-guard-host.c',MEM+'stack_domain.c'],
    'tables':['scripts/stack-table-host.c',*[MEM+n+'.c' for n in
        ('stack_domain','stack_map','paging_entry','ram_map_plan','physical_contract',
         'paging_build','image_permissions','image_walk','mapping_core')]],
    'runtime':['scripts/stack-runtime-host.c',*[MEM+n+'.c' for n in
        ('stack_domain','stack_map','ram_map_plan','image_permissions','paging_entry')]],
    'inverse':['scripts/stack-inverse-host.c',*[MEM+n+'.c' for n in
        ('stack_domain','physical_translation','physical_contract','ram_map_plan')]],
    'service':['scripts/stack-service-host.c','kernel/src/smp/cpu_request_state.c',MEM+'tlb_interval.c'],
    'scheduler':['scripts/stack-scheduler-host.c'],
    'faults':['scripts/stack-guard-probe-host.c','scripts/stack-guard-probe-abi.S',
        MEM+'guard_probe_access.S',MEM+'guard_probe_match.c',MEM+'stack_fault.c']}
EXPECTED={
    'owners':'{"status":"PASS","checks":6792,"concurrent_creators":24,"admitted_owners":1,"hardware_faults":"NOT_HOSTED"}\n',
    'tables':'STACK_TABLE_HOST checks=3738 status=PASS actual_constructor=1 actual_walker=1 hardware_faults=NOT_HOSTED\n',
    'runtime':'STACK_RUNTIME_HOST checks=96476 status=PASS actual_adapter=1 actual_slot_mapper=1 peak_task_slots=1536 required_backlog=1353 privileged_boundaries=HOSTED\n',
    'inverse':'STACK_INVERSE_HOST status=PASS actual_stack_inverse=1 physical_zero=VALID broken_identity=REJECTED privileged_execution=NONE\nchecks=77\n',
    'service':'STACK_SERVICE_HOST actual_publish_handle_collect_retire=PASS hardware_execution=NONE\n',
    'scheduler':'STACK_SCHEDULER_HOST status=PASS source_adapters=2 cases=26 capacity=16384 canary=512 privileged_boundaries=HOSTED\n',
    'faults':'STACK_GUARD_PROBE_HOST checks=1095 status=PASS hardware_faults=NOT_HOSTED\n'}
MODES={'normal':[], 'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
       'asan':['-fsanitize=address','-fno-omit-frame-pointer']}

SCHEDULER_FUNCTIONS=('sched_copy_name','sched_make_default_name','task_id_allocate_locked',
    'scheduler_task_stack_guard_ok','scheduler_task_stack_rsp_in_bounds','task_exit_reason_valid',
    'task_reap_mask_locked','thread_create_ex_handle','scheduler_reap_zombies')

def scheduler_source():
    import stack_current
    path=E.ROOT/'kernel/src/core/scheduler.c';functions=stack_current.functions(path.read_text())
    source='\n\n'.join(functions[name] for name in SCHEDULER_FUNCTIONS)+'\n'
    return source,{'source_sha256':E.sha(path),'functions':{name:E.identity(functions[name])
        for name in SCHEDULER_FUNCTIONS},'extracted_sha256':E.identity(source)}

def paging_source():
    import stack_current
    path=E.ROOT/'kernel/src/memory/paging.c'
    source=stack_current.functions(path.read_text())['paging_stack_install']+'\n'
    return source,{'source_sha256':E.sha(path),'function':'paging_stack_install',
        'extracted_sha256':E.identity(source)}

def module(name):
    spec=importlib.util.spec_from_file_location('stack_host_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def assertion_layout():
    import os
    from unittest.mock import patch
    symbols={'g_assertion_test_state':{'address':0xffffffff80001000},
        'g_assertion_test_fixture':{'address':0xffffffff80002000,'size':96},
        'g_scheduler_cpus':{'address':0xffffffff80003000,'size':32*152},
        'g_cpus':{'address':0xffffffff80005000,'size':32*48}}
    rows=[]
    for contract,offset in (('guarded-kernel-stacks',184),('kernel-image-protection',168),('',168)):
        with patch.dict(os.environ,HOBBYOS_VALIDATION_CONTRACT=contract):
            reader=module('assert-qemu')
            E.require(reader.TASK_ID_OFFSET==8 and reader.TASK_GENERATION_OFFSET==offset,
                'assertion current/historical task layout differs')
            commands=(reader.observer_commands(symbols,Path('fixture-before.bin'),False),
                reader.context_observer_commands(symbols))
            for command in commands:
                fields=[line for line in command if 'SCHEDULER_TASK_GENERATION=' in line]
                E.require(len(fields)==1 and fields[0].endswith(f'($current + {offset})'),
                    'assertion observer reads the wrong lifecycle field')
            rows.append({'contract':contract,'task_id':8,'lifecycle_generation':offset,'observers':2})
    return rows

def run(output):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    gate=module('memory-observation-gate');rows=[]
    extracted,scheduler=scheduler_source();(output/'stack-scheduler-extracted.h').write_text(extracted)
    extracted,paging=paging_source();(output/'stack-paging-extracted.h').write_text(extracted)
    for mode,extra in MODES.items():
        for subject,sources in SUBJECTS.items():
            binary=output/(subject+'-'+mode)
            flags=['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
                '-ffunction-sections','-fdata-sections',*(['-pthread'] if subject=='owners' else []),
                *(['-DHOBBYOS_SELFTEST=1'] if subject=='service' else []),
                *(['-I',str(output)] if subject in ('scheduler','runtime') else []),*extra]
            compiled=gate.run(output,binary.name+'-compile',
                ['gcc',*flags,*sources,'-Wl,--gc-sections','-o',str(binary)])
            arguments=[str(binary)]
            if subject=='service':
                data=output/(binary.name+'-data');data.mkdir();arguments.append(str(data))
            executed=gate.run(output,binary.name,arguments,
                {'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            summary=(executed/'stdout.log').read_text()
            E.require(summary==EXPECTED[subject],'stack actual hosted coverage differs: '+subject+': '+summary.strip())
            decisions={}
            if subject=='scheduler':decisions['scheduler_source']=scheduler
            if subject=='runtime':decisions['paging_source']=paging
            if subject=='service':
                import stack_native_audit
                decisions['service_decisions']=stack_native_audit.host_cases(data)
            rows.append({'subject':subject,'mode':mode,**decisions,'compile':compiled.relative_to(output).as_posix(),
                'execute':executed.relative_to(output).as_posix(),'summary':summary,
                'stdout_sha256':E.sha(executed/'stdout.log'),'binary_sha256':E.sha(binary)})
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'executions':rows,
        'assertion_task_layout':assertion_layout(),
        'authority':'Actual hosted C/assembly. PMM/IRQ/TSS/request privileged boundaries and fault frames are hosted substitutes; no hardware-fault claim.'}
    E.write(output/'stack-host.json',result);return result

def verify(directory):
    gate=module('cpu-control-state-gate');record=json.loads((directory/'stack-host.json').read_text());seen=set()
    E.require(record['schema']==1 and record['status']=='PASS' and record['build_input_sha256']==E.identity(E.manifest()),
        'stack hosted schema/source')
    E.require(record['assertion_task_layout']==assertion_layout(),'assertion observer task layout coverage differs')
    for row in record['executions']:
        subject,mode=row['subject'],row['mode'];pair=subject,mode
        E.require(pair not in seen and subject in SUBJECTS and mode in MODES,'stack hosted duplicate/unknown execution');seen.add(pair)
        compiled,_=gate.verify_command(directory,row['compile']);executed,output=gate.verify_command(directory,row['execute'])
        argv=compiled['argv']
        arguments=[argv[-1],argv[-1]+'-data'] if subject=='service' else [argv[-1]]
        E.require(argv[0]=='gcc' and executed['argv']==arguments and all(p in argv for p in SUBJECTS[subject]) and
            compiled['build_input_sha256']==executed['build_input_sha256']==record['build_input_sha256'] and
            output==row['summary']==EXPECTED[subject] and E.sha(directory/row['execute']/'stdout.log')==row['stdout_sha256'] and
            ('-fsanitize=address' in argv)==(mode=='asan') and ('-fsanitize=undefined' in argv)==(mode=='ubsan') and
            ('-pthread' in argv)==(subject=='owners') and '-O2' in argv and '-Werror' in argv,
            'stack actual compile/execution/sanitizer mismatch')
        if subject=='service':
            import stack_native_audit
            E.require('-DHOBBYOS_SELFTEST=1' in argv,'stack service translation units disagree on telemetry layout')
            E.require(row['service_decisions']==stack_native_audit.host_cases(directory/(Path(argv[-1]).name+'-data')),
                'stack actual native service decoder coverage differs')
        if subject=='scheduler':
            extracted,binding=scheduler_source()
            E.require(row['scheduler_source']==binding and
                (directory/'stack-scheduler-extracted.h').read_text()==extracted and '-I' in argv,
                'stack hosted scheduler adapters differ from actual source')
        if subject=='runtime':
            extracted,binding=paging_source()
            E.require(row['paging_source']==binding and
                (directory/'stack-paging-extracted.h').read_text()==extracted and '-I' in argv,
                'stack hosted mapping adapter differs from actual source')
    E.require(seen=={(s,m) for s in SUBJECTS for m in MODES},'stack missing actual hosted suite')
    return {'status':'PASS','executions':len(seen),'subjects':sorted(SUBJECTS),'hardware_faults':'NOT_HOSTED'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    print(E.canonical(run(p.parse_args().output)).decode())
