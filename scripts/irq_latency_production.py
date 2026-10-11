"""Portable paired compilation, separate from original-toolchain reproduction."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import memory_evidence as E

BASE = 'd68a3b4a2c97f55dfdcc445bfe42317e09e1c55e'
TREE = '3ce6be4fcae7c23fd41adb439e081af72ea4717c'
UNITS = ('apic/lapic.c', 'core/idt.c', 'core/interrupt_stubs.S', 'core/spinlock.c',
         'core/switch.S', 'memory/dma_buffer.c', 'memory/heap.c', 'memory/kernel_stack.c',
         'memory/kernel_tlb.c', 'memory/mapping_core.c', 'memory/owned_mapping.c',
         'memory/paging.c', 'memory/pmem.c', 'shell/commands/registry.c', 'smp/cpu_request.c')
COMMON = ['-ffreestanding','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel','-fno-pic','-fno-pie']
C_FLAGS = ['-O2','-fno-strict-aliasing','-fno-stack-protector','-fno-delete-null-pointer-checks',
    '-fno-omit-frame-pointer','-std=gnu11','-MMD','-MP','-Wall','-Wextra','-Wundef',
    '-Wmissing-prototypes','-Wvla','-Wframe-larger-than=2048','-Werror',
    '-Werror=implicit-function-declaration','-Werror=incompatible-pointer-types','-Werror=int-conversion']
DEFINES = ['-DHOBBYOS_DEBUG_ASSERT=0','-DHOBBYOS_IRQ_LATENCY=0']
CHANGE_SUBJECTS = {
    'kernel/src/core/spinlock.c': ('spinlock-release',),
    'kernel/src/core/clock.c': ('clock-body','request-clock','request-pvclock','clocksource','pvclock'),
    'kernel/src/core/kernel_init.c': ('clocksource',),
    'kernel/src/smp/smp_boot.c': ('clocksource',),
    'kernel/src/smp/cpu_request.c': ('request-cycle-contract','request-service','request-clock'),
    'kernel/src/memory/kernel_tlb.c': ('request-cycle-contract','request-service'),
    'kernel/src/smp/cpu_request_state.c': ('request-cycle-contract','request-service'),
    'kernel/src/memory/heap.c': ('heap-preparation',),
    'kernel/src/memory/pmem.c': ('pmm-observation','pmm-concurrent'),
    'kernel/src/utils/bitmap.c': ('pmm-observation','bitmap-load'),
    'kernel/src/shell/diagnostic_result.c': ('stack-report','stack-emitter','warning-report','warning-emitter'),
    'kernel/src/core/panic.c': ('warning-report','warning-emitter'),
    'kernel/src/core/interrupts.c': ('interior-controller',),
    'kernel/src/cpu/mmio.c': ('mmio-identity', 'writeback-clock-integration'),
}
NEW_UNITS = {
    'kernel/src/core/clocksource.c': ('clocksource','request-pvclock'),
    'kernel/src/core/pvclock.c': ('pvclock','clocksource'),
}

def unit_kind(name):
    if name in ('kernel/src/core/clock.c','kernel/src/core/kernel_init.c','kernel/src/smp/smp_boot.c'):
        return 'qualified-clocksource-integration'
    return 'measured-local-correction' if name in ('kernel/src/memory/pmem.c','kernel/src/utils/bitmap.c',
        'kernel/src/memory/heap.c','kernel/src/smp/cpu_request.c','kernel/src/memory/kernel_tlb.c',
        'kernel/src/smp/cpu_request_state.c','kernel/src/core/spinlock.c') else 'instrumentation-only'


def expected_argv(source, additional=False):
    E.require(source in {'kernel/src/'+s for s in UNITS} or additional and source in (CHANGE_SUBJECTS | NEW_UNITS),
              'production comparison unknown unit')
    flags = COMMON + (C_FLAGS if source.endswith('.c') else []) + DEFINES
    if source.endswith('.c'): flags += ['-DHOBBYOS_KERNEL_SERIAL_DEV_PORTS=1']
    return ['gcc',*flags,'-c',source,'-o',str(Path(source).with_suffix('.o'))]


def validate_recorded(record):
    E.require(record['schema']==1 and record['status']=='PASS' and record['baseline_commit']==BASE,
              'production recorded identity')
    names = [r['source'] for r in record['rows']]
    E.require(len(names)==len(set(names))==len(UNITS) and set(names)=={'kernel/src/'+s for s in UNITS},
              'production comparison unit inventory')
    for row in record['rows']:
        E.require(row['argv']==expected_argv(row['source']), 'production recorded compiler flags/output')
        E.require(row['exit']==0 and row['exact_object_equal'] is True and
                  row['before_object_sha256']==row['after_object_sha256'], 'production recorded failed/different object')
        for key in ('before_object_sha256','after_object_sha256','baseline_source_sha256','current_source_sha256'):
            E.require(len(row[key])==64 and all(c in '0123456789abcdef' for c in row[key]),
                      'production recorded digest')


def source_snapshot(root):
    rows=[]
    for p in sorted(root.rglob('*')):
        E.require(not p.is_symlink(), 'production source symlink')
        if p.is_file():
            data=p.read_bytes()
            rows.append({'path':p.relative_to(root).as_posix(),'size':len(data),'sha256':E.sha(p),
                'mode':'100755' if p.stat().st_mode & 0o111 else '100644',
                'current_blob':hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()})
    return rows


def approved_baseline(root):
    rows=source_snapshot(root)
    spec=importlib.util.spec_from_file_location('latency_source_tree',E.ROOT/'scripts/package-memory-review.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    E.require(len(rows)==759 and module.source_tree(rows)==TREE, 'production approved baseline tree')
    return rows


def toolchain():
    compiler=shutil.which('gcc')
    E.require(compiler is not None, 'production local compiler unavailable')
    facts={'driver':str(Path(compiler).resolve()),'driver_sha256':E.sha(Path(compiler).resolve())}
    for name,args in [('version',['--version']),('target',['-dumpmachine']),
                      ('full_version',['-dumpfullversion']),('specs',['-dumpspecs'])]:
        p=subprocess.run([compiler,*args],capture_output=True,env={**os.environ,'LC_ALL':'C'})
        E.require(p.returncode==0, 'production toolchain query failed')
        facts[name+'_sha256']=hashlib.sha256(p.stdout).hexdigest()
        if name!='specs': facts[name]=p.stdout.decode().strip()
    for name in ('cc1','as'):
        p=subprocess.run([compiler,'-print-prog-name='+name],capture_output=True,text=True,check=True)
        path=Path(p.stdout.strip())
        if not path.is_absolute(): path=Path(shutil.which(str(path)) or '')
        E.require(path.is_file(), 'production compiler component unavailable: '+name)
        facts[name]={'path':str(path.resolve()),'sha256':E.sha(path.resolve())}
    return facts


def compile_pair(baseline, current, source, output, compiler, change=None, *, ordinary_debug=False):
    """Only output/dependency relocation differs from the validated flags."""
    E.require(not ordinary_debug or source=='kernel/src/cpu/mmio.c', 'ordinary debug comparison unit')
    result={'source':source,'roles':{}}
    if ordinary_debug:result['profile']='ordinary-debug-no-observer'
    for role,root in [('before',baseline),('after',current)]:
        directory=output/role;directory.mkdir(parents=True,exist_ok=True)
        obj=directory/Path(source).with_suffix('.o');obj.parent.mkdir(parents=True,exist_ok=True)
        E.require(obj.resolve().is_relative_to(output.resolve()) and not obj.exists(), 'production writable output path')
        argv=expected_argv(source,additional=change is not None);argv[0]=compiler;argv[-1]=str(obj.resolve())
        if ordinary_debug:argv[argv.index('-DHOBBYOS_DEBUG_ASSERT=0')]='-DHOBBYOS_DEBUG_ASSERT=1'
        if source.endswith('.c'):
            argv+=['-MF',str(obj.with_suffix('.d').resolve()),'-MT',str(Path(source).with_suffix('.o'))]
        p=subprocess.run(argv,cwd=root,capture_output=True,env={**os.environ,'LC_ALL':'C'})
        stem=source.replace('/','_')+'-'+role
        (output/(stem+'.stdout')).write_bytes(p.stdout);(output/(stem+'.stderr')).write_bytes(p.stderr)
        row={'argv':argv,'cwd':str(root),'exit':p.returncode,'source_sha256':E.sha(root/source),
             'stdout_sha256':hashlib.sha256(p.stdout).hexdigest(),'stderr_sha256':hashlib.sha256(p.stderr).hexdigest(),
             'object_sha256':E.sha(obj) if p.returncode==0 and obj.is_file() else None}
        E.write(output/(stem+'.json'),row)
        E.require(p.returncode==0 and row['object_sha256'] is not None, 'production local compilation failed')
        result['roles'][role]=row
    equal=result['roles']['before']['object_sha256']==result['roles']['after']['object_sha256']
    if change is None or change['kind']=='instrumentation-only':
        E.require(equal,'production local paired objects differ')
    else:E.require(not equal,'intentional production difference hidden')
    if change is not None:result.update(change=change,object_equal=equal)
    return result


def compile_new(current,source,output,compiler,change):
    E.require(source in NEW_UNITS and change['kind']=='qualified-clocksource', 'new production unit role')
    obj=output/'new'/Path(source).with_suffix('.o');obj.parent.mkdir(parents=True,exist_ok=True)
    E.require(not obj.exists(),'new production output already exists')
    argv=expected_argv(source,additional=True);argv[0]=compiler;argv[-1]=str(obj)
    argv+=['-MF',str(obj.with_suffix('.d')),'-MT',str(Path(source).with_suffix('.o'))]
    p=subprocess.run(argv,cwd=current,capture_output=True,env={**os.environ,'LC_ALL':'C'})
    stem=source.replace('/','_')
    (output/(stem+'.compile.log')).write_bytes(p.stdout+p.stderr)
    E.require(p.returncode==0 and obj.is_file(),'new production unit compilation failed')
    dis=subprocess.check_output(['objdump','-dr',str(obj)],text=True)
    symbols=subprocess.check_output(['nm','-S',str(obj)],text=True)
    (output/(stem+'.disassembly')).write_text(dis);(output/(stem+'.symbols')).write_text(symbols)
    E.require('g_source_terminal' not in symbols and 'irq_latency_tick' not in symbols,
              'optional observer payload in new production unit')
    if source.endswith('/pvclock.c'):
        E.require('rdtsc' in dis and 'lfence' in dis and 'hpet_' not in dis and 'cpuid' not in dis,
                  'pvclock local snapshot production instructions')
    else:
        E.require('cpu_write_msr_safe' in dis and 'cpu_read_msr_safe' in dis and 'hpet_read_clock_sample' in dis,
                  'clocksource registration/reference operations absent')
    return dict(source=source,change=change,argv=argv,exit=p.returncode,source_sha256=E.sha(current/source),
        object_sha256=E.sha(obj),compile_sha256=E.sha(output/(stem+'.compile.log')),
        disassembly_sha256=E.sha(output/(stem+'.disassembly')),symbols_sha256=E.sha(output/(stem+'.symbols')),
        baseline_object='NOT_APPLICABLE_NEW_UNIT')


def compare(baseline, current, record, output, original_toolchain=None, change_contract=None):
    baseline,current,output=(Path(p).resolve() for p in (baseline,current,output))
    E.require(baseline!=current and not baseline.is_relative_to(current) and not current.is_relative_to(baseline),
              'production current source used as baseline')
    E.require(not output.exists() and not output.is_relative_to(baseline) and not output.is_relative_to(current),
              'production writable output path')
    validate_recorded(record)
    before=approved_baseline(baseline);after=source_snapshot(current)
    changes={};new_units={}
    if change_contract is not None:
        E.require(change_contract['schema']==2 and change_contract['baseline_commit']==BASE and
            set(change_contract['units'])==set(CHANGE_SUBJECTS) and
            set(change_contract.get('new_units',{}))==set(NEW_UNITS),'production change contract inventory')
        changes=change_contract['units']
        for name,row in changes.items():
            E.require(E.sha(baseline/name)==row['baseline_sha256'] and E.sha(current/name)==row['current_sha256'],
                      'production intentional source binding')
            E.require(row['subjects']==list(CHANGE_SUBJECTS[name]) and row['kind']==unit_kind(name),
                'production change semantic contract')
        new_units=change_contract['new_units']
        for name,row in new_units.items():
            E.require(not (baseline/name).exists() and row['baseline_absent'] is True and
                E.sha(current/name)==row['current_sha256'],'new production source binding')
            E.require(row['kind']=='qualified-clocksource' and row['subjects']==list(NEW_UNITS[name]),
                      'new production semantic contract')
    E.require(shutil.disk_usage(output.parent).free>=4*1024**3+64*1024**2,
              'BLOCKED_STORAGE production compilation reserve')
    for name in ('CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','GCC_EXEC_PREFIX','COMPILER_PATH','LIBRARY_PATH'):
        E.require(not os.environ.get(name), 'production uncontrolled compiler environment: '+name)
    local=toolchain();output.mkdir()
    exact=original_toolchain is not None and original_toolchain==local
    rows=[];debug_mmio=None;new_rows=[]
    try:
        for historical in record['rows']:
            source=historical['source']
            E.require(E.sha(baseline/source)==historical['baseline_source_sha256'], 'production baseline source binding')
            row=compile_pair(baseline,current,source,output,local['driver'],changes.get(source))
            row['recorded_object_sha256']=historical['before_object_sha256']
            if exact:
                E.require(row['roles']['after']['source_sha256']==historical['current_source_sha256'],
                          'production exact reproduction source differs')
                E.require(all(r['object_sha256']==historical['before_object_sha256'] for r in row['roles'].values()),
                          'production exact toolchain reproduction differs')
            rows.append(row)
        for source in sorted(set(changes)-{'kernel/src/'+s for s in UNITS}):
            rows.append(compile_pair(baseline,current,source,output,local['driver'],changes[source]))
        for source,change in new_units.items():
            new_rows.append(compile_new(current,source,output,local['driver'],change))
        if 'kernel/src/cpu/mmio.c' in changes:
            debug_mmio=compile_pair(baseline,current,'kernel/src/cpu/mmio.c',output/'ordinary-debug-mmio',
                local['driver'],changes['kernel/src/cpu/mmio.c'],ordinary_debug=True)
        if changes:
            spec=importlib.util.spec_from_file_location('changed_unit_host',current/'scripts/irq-latency-host.py')
            host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
            subjects=sorted({s for row in (*changes.values(),*new_units.values()) for s in row['subjects']})
            if 'request-service' in subjects:
                spec=importlib.util.spec_from_file_location('changed_request_host',current/'scripts/request-telemetry-host.py')
                service=importlib.util.module_from_spec(spec);spec.loader.exec_module(service)
                service.run(output/'request-service',current)
                subjects.remove('request-service')
            if 'request-clock' in subjects:
                spec=importlib.util.spec_from_file_location('checked_request_host',current/'scripts/request-telemetry-host.py')
                service=importlib.util.module_from_spec(spec);spec.loader.exec_module(service)
                service.run(output/'request-clock',current,actual_clock=True)
                subjects.remove('request-clock')
            if 'request-pvclock' in subjects:
                spec=importlib.util.spec_from_file_location('checked_pvclock_request_host',current/'scripts/request-telemetry-host.py')
                service=importlib.util.module_from_spec(spec);spec.loader.exec_module(service)
                service.run(output/'request-pvclock',current,actual_pvclock=True)
                subjects.remove('request-pvclock')
            for mode,extra in host.MODES.items():
                for subject in subjects:
                    binary=output/(subject+'-'+mode)
                    argv=[local['driver'],'-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
                        *extra,*host.subject_flags(subject),*host.SUBJECTS[subject],'-o',str(binary)]
                    proc=subprocess.run(argv,cwd=current,capture_output=True)
                    (output/(binary.name+'-compile.log')).write_bytes(proc.stdout+proc.stderr)
                    E.require(proc.returncode==0,'production correction actual semantic compile')
                    proc=subprocess.run([str(binary)],cwd=output,capture_output=True,
                        env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
                    (output/(binary.name+'-run.log')).write_bytes(proc.stdout+proc.stderr)
                    E.require(proc.returncode==0 and proc.stdout.decode()==host.EXPECTED[subject],
                              'production correction actual semantic test')
    finally:
        E.require(source_snapshot(baseline)==before and source_snapshot(current)==after,
                  'production immutable source changed')
    result={'schema':2,'status':'PASS','mode':'LOCAL_PRODUCTION_EQUIVALENCE_AND_MEASURED_CHANGES' if changes else 'REVIEWER_LOCAL_PRODUCTION_EQUIVALENCE',
        'baseline_commit':BASE,'baseline_tree':TREE,'local_toolchain':local,
        'recorded_build':{'status':'IDENTITY_FACTS_VERIFIED','receipt_sha256':E.identity(record),
            'exact_reproduction':'PASS' if exact else 'UNAVAILABLE_TOOLCHAIN_IDENTITY',
            'original_toolchain_supplied':original_toolchain is not None},
        'source_immutable':True,'rows':rows,'new_units':new_rows,'whole_production_acceptance':False}
    if changes:result['change_contract']=change_contract
    if debug_mmio is not None:result['ordinary_debug_mmio']=debug_mmio
    E.write(output/'result.json',result)
    return result
