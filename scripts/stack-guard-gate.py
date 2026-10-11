#!/usr/bin/env python3
"""Guarded stack acceptance for the maintainer's own HobbyOS kernel in QEMU.

Every stage extends the existing semantic/native gate and its source-bound
receipts. Destructive guest experiments are separate from successful boots.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E
import stack_current as CURRENT
import stack_guard_evidence as V
import validation_resources as R
import validation_stages as S
import validation_retention as RETENTION

def module(name):
    spec=importlib.util.spec_from_file_location('stack_gate_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
D=module('kernel-wx-gate');G=D.G;T=D.T;DEVICE=D.DEVICE
G.SURFACE+=('cmd_stacktest','guard_probe','guard_overflow','stack_test_')
def read(path):return json.loads(path.read_text())
def receipt(evidence,mode,result):T.receipt(evidence,'stack-'+mode,result)
def plan():
    return {'schema':1,'contract':CURRENT.CONTRACT,'base_commit':CURRENT.BASE,'base_tree':CURRENT.TREE,
        'build_input_sha256':E.identity(E.manifest()),'inherited':D.plan(),'commands':DEVICE.V.COMMANDS,
        'region':[V.BASE,V.END],'slots':V.SLOTS,'cpu_reserved_slots':V.CPU_SLOTS,'task_slots':1536,
        'pitch':V.PITCH,'guard_bytes':4096,'payload_bytes':[16384,32768],'task_canary_bytes':512,
        'prepared_tasks':8,'task_cycles_per_boot':256,'warmup_cycles':8,'concurrent_creators':2,
        'taskman_peak_commands':V.taskman_peak_commands(DEVICE.V.COMMANDS),
        'taskman_peak_profiles':['q35-tcg-smp4','q35-kvm-smp4'],
        'destructive_profiles':{'stack-overflow':['q35-tcg-smp1','q35-kvm-smp4'],
            'stack-timeout':['q35-kvm-smp4']},'df_rip_authority':'ABORT_UNDEFINED',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','next_increment':'NOT_STARTED'}
def static(evidence):
    base=evidence
    if (evidence/'image-static.json').exists():
        base=evidence/'stage-revisions/current-static'/G.stamp();base.mkdir(parents=True)
    D.static(base)
    E.write(base/'stack-test-plan.json',plan())
    receipt(base,'static',CURRENT.boundary_semantics())
    if base!=evidence:S.seal_static(evidence,base)
def host(evidence):
    D.host(evidence);base=S.host_base(evidence);path=base/'stack-host'/G.stamp()
    module('stack-guard-host').run(path)
    receipt(base,'host',{'path':path.relative_to(base).as_posix(),'sha256':E.sha(path/'stack-host.json')})
def codegen(path,testing):
    inherited=D.codegen(path,testing);proof=read(path/'image-elf-proof.json')
    import memory_elf_proof as ELF
    elf=ELF.replay(proof);names={r['name'] for r in elf.symbols()}
    for name in ('kernel_stack_acquire','kernel_stack_retire','stack_owner_guard_lookup','stack_fault_classify',
        'paging_stack_install','tss_install_guarded_ist'):
        E.require(name in names,'stack production enforcement absent: '+name)
    for name in ('cmd_stacktest','guard_probe_fixup','guard_probe_read','guard_overflow_enter','stack_test_run'):
        E.require((name in names)==testing,'stack test payload isolation: '+name)
    E.require(bool(V.fixed_probe_ranges(elf))==testing,'stack actual assembler proof role')
    return {'status':'PASS','inherited':inherited,'test_payload':testing,
        'assembler':'FIXED_LOAD_RECOVERY_AND_64_BYTE_CURRENT_STACK_STEPS' if testing else 'ABSENT'}
def build(evidence):
    D.build(evidence)
    receipt(evidence,'build',{role:codegen(evidence/'candidates'/role,role in ('selftest','framebuffer-reference'))
        for role in ('selftest','production','debug','framebuffer-reference')})
def experiment_proof(evidence):
    index=read(evidence/'stack-experiments.json');expected=plan()['destructive_profiles'];result=[];seen=set()
    for row in index['runs']:
        E.safe(row['path']);path=evidence/row['path'];run=read(path/'result.json');kind=row['experiment']
        key=kind,run['profile'];E.require(key not in seen and kind in expected and run['profile'] in expected[kind],
            'stack destructive missing/duplicate profile');seen.add(key)
        proof=module('verify-memory-observation').verify_run(path/'qemu-serial.log',failure_experiment=kind)
        E.require(proof['status']=='EXPECTED_FAILURE' and run['candidate_id']==
            E.identity(read(evidence/'candidates/selftest/candidate.json')),'stack destructive candidate differs')
        result.append({'path':row['path'],'experiment':kind,'proof':proof})
    E.require(seen=={(kind,p) for kind,profiles in expected.items() for p in profiles},'stack destructive coverage incomplete')
    return result
def experiments(evidence):
    if (evidence/'stack-experiments.json').is_file():return experiment_proof(evidence)
    rows=[]
    for kind,profiles in plan()['destructive_profiles'].items():
        for profile in profiles:
            _,accel,cpu=profile.split('-');directory=evidence/'stack-experiments'/(kind+'-'+profile+'-'+G.stamp())
            R.before_step(evidence,'runtime')
            argv=['python3','-B','scripts/memory-qemu.py','--candidate',str(evidence/'candidates/selftest'),
                '--evidence',str(directory),'--machine','q35','--accel',accel,'--smp',cpu[3:],
                '--serial-backend','file','--fault-trace','--command',
                'stacktest '+('overflow' if kind=='stack-overflow' else 'timeout')]
            # The collector must retain a failed, nonreturning guest command.
            try:G.run(evidence,kind+'-'+profile,argv)
            except RuntimeError:
                E.require((directory/'result.json').is_file(),'stack collector failed without a raw attempt')
            proof=module('verify-memory-observation').verify_run(directory/'qemu-serial.log',failure_experiment=kind)
            E.require(proof['status']=='EXPECTED_FAILURE','stack destructive oracle failed')
            RETENTION.retire_run(directory)
            rows.append({'path':directory.relative_to(evidence).as_posix(),'experiment':kind})
    E.write(evidence/'stack-experiments.json',{'schema':1,'runs':rows})
    return experiment_proof(evidence)
def taskman_peak_proof(evidence):
    rows=read(evidence/'stack-taskman-peak.json')['runs'];result=[];seen=set()
    for name in rows:
        E.safe(name);directory=evidence/name;raw=read(directory/'result.json')
        E.require(raw['profile'] in plan()['taskman_peak_profiles'] and raw['profile'] not in seen and
            not raw['production'] and not raw['readback'] and raw['candidate_id']==
            E.identity(read(evidence/'candidates/selftest/candidate.json')),'stack TASKMAN peak profile/candidate')
        seen.add(raw['profile']);result.append(DEVICE.V.verify_complete_run(directory,stack_peak=True))
    E.require(seen==set(plan()['taskman_peak_profiles']),'stack real TASKMAN peak coverage incomplete')
    return result
def taskman_peak(evidence):
    if (evidence/'stack-taskman-peak.json').is_file():return taskman_peak_proof(evidence)
    rows=[]
    for profile in plan()['taskman_peak_profiles']:
        selected=None
        for previous in sorted((evidence/'stack-taskman-peak').glob(profile+'-*')):
            if not (previous/'result.json').is_file():continue
            try:DEVICE.V.verify_complete_run(previous,stack_peak=True)
            except (ValueError,KeyError,AssertionError,OSError) as error:
                rejection=evidence/'stack-taskman-peak-rejections'/(previous.name+'.json')
                if not rejection.exists():
                    rejection.parent.mkdir(exist_ok=True)
                    E.write(rejection,{'path':previous.relative_to(evidence).as_posix(),'status':'REJECTED',
                        'result_sha256':E.sha(previous/'result.json'),'reason':str(error),
                        'verifier_sha256':E.identity(E.manifest(harness=True))})
            else:selected=previous
        if selected is not None:
            rows.append(selected.relative_to(evidence).as_posix());continue
        _,accel,cpu=profile.split('-');directory=evidence/'stack-taskman-peak'/(profile+'-'+G.stamp())
        R.before_step(evidence,'runtime')
        argv=['python3','-B','scripts/memory-qemu.py','--candidate',str(evidence/'candidates/selftest'),
            '--evidence',str(directory),'--machine','q35','--accel',accel,'--smp',cpu[3:],'--serial-backend','file']
        for command in plan()['taskman_peak_commands']:argv+=['--command',command]
        try:
            G.run(evidence,'stack-taskman-peak-'+profile,argv)
            DEVICE.V.verify_complete_run(directory,stack_peak=True)
        finally:
            if (directory/'result.json').is_file() and read(directory/'result.json')['cleanup']:
                RETENTION.retire_run(directory)
        rows.append(directory.relative_to(evidence).as_posix())
    E.write(evidence/'stack-taskman-peak.json',{'schema':1,'runs':rows})
    return taskman_peak_proof(evidence)
def runtime_proof(evidence):
    rows={}
    for base in (evidence,*(evidence/name for name in ('nx-disabled','xapic','hub-keyboard','high-ram','high-ram-fallback'))):
        series=read(base/'series.json')
        for name in series['runs']+series.get('diagnostics',[]):
            path=base/name;key=path.relative_to(evidence).as_posix()
            E.require(key not in rows,'stack repeated boot identity')
            result=DEVICE.V.verify_complete_run(path)
            E.require(result['guarded_stacks']['status']=='PASS','stack complete current predicate absent')
            rows[key]=result['guarded_stacks']
    E.require(sum(not read(evidence/name/'result.json')['readback'] for name in rows)==39,'stack functional boot coverage')
    return {'status':'PASS','runs':rows,'experiments':experiment_proof(evidence),
        'taskman_peak':taskman_peak_proof(evidence)}
def runtime(evidence):
    D.runtime(evidence);experiments(evidence);taskman_peak(evidence)
    if (evidence/'stack-runtime.json').exists():
        S.revalidate_runtime(evidence,'stack',lambda path:reusable(evidence,'runtime',receipt_override=path));return
    receipt(evidence,'runtime',runtime_proof(evidence))
def production(evidence):
    D.production(evidence)
    receipt(evidence,'production',[DEVICE.V.verify_complete_run(evidence/name) for name in read(evidence/'production.json')['runs']])
def fixtures(evidence):
    D.fixtures(evidence);path=evidence/'stack-fixtures'/G.stamp()
    module('stack-guard-fixtures').run(evidence,path)
    receipt(evidence,'fixtures',{'path':path.relative_to(evidence).as_posix(),'sha256':E.sha(path/'results.json')})
def verify_host(evidence):
    inherited=D.verify_host(evidence);base=S.host_base(evidence);row=read(base/'stack-host.json');path=base/row['result']['path']
    E.require(E.sha(path/'stack-host.json')==row['result']['sha256'],'stack hosted receipt binding')
    return {'inherited':inherited,'stack':module('stack-guard-host').verify(path)}
def compare_runtime(recorded,observed,transport=None):
    """Bind the two peak serial provenance labels, then compare every fact."""
    E.require(isinstance(recorded,dict) and isinstance(observed,dict) and
        isinstance(recorded.get('taskman_peak'),list) and isinstance(observed.get('taskman_peak'),list) and
        len(recorded['taskman_peak'])==len(observed['taskman_peak'])==2,'stack runtime peak coverage')
    for old,new in zip(recorded['taskman_peak'],observed['taskman_peak']):
        T.C.verify_mode_run(old['inherited']['invalidation']['policy'],
            new['inherited']['invalidation']['policy'],transport)
    E.require(json.loads(E.canonical(observed))==recorded,'stack stage semantic result differs: runtime')

def semantic(evidence,mode,*,receipt_row=None,transport=None):
    base=S.host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    row=receipt_row or read(S.runtime_receipt(evidence,'stack') if mode=='runtime' else base/('stack-'+mode+'.json'))
    if mode=='runtime':S.runtime_predecessor(evidence,row)
    E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'stack stage source/status')
    if mode=='static':expected=CURRENT.boundary_semantics();E.require(read(base/'stack-test-plan.json')==plan(),'stack frozen plan')
    elif mode=='host':verify_host(evidence);return row['result']
    elif mode=='build':expected={role:codegen(evidence/'candidates'/role,role in ('selftest','framebuffer-reference')) for role in ('selftest','production','debug','framebuffer-reference')}
    elif mode=='runtime':
        expected=runtime_proof(evidence);compare_runtime(row['result'],expected,transport);return expected
    elif mode=='production':expected=[DEVICE.V.verify_complete_run(evidence/name) for name in read(evidence/'production.json')['runs']]
    elif mode=='fixtures':
        path=evidence/row['result']['path']/'results.json';E.require(E.sha(path)==row['result']['sha256'],'stack fixture bytes')
        module('stack-guard-fixtures').verify_receipt(read(path));return row['result']
    E.require(json.loads(E.canonical(expected))==row['result'],'stack stage semantic result differs: '+mode);return expected
def reusable(evidence,mode,*,receipt_override=None):
    base=S.host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    def validate(row):
        E.require(D.reusable(evidence,mode)['state']=='REUSABLE','stack inherited stage incomplete')
        return semantic(evidence,mode,receipt_row=row)
    path=S.runtime_receipt(evidence,'stack') if mode=='runtime' else base/('stack-'+mode+'.json')
    return R.classify(receipt_override or path,validate,source=E.identity(E.manifest()),
        harness=None if mode in ('build','production') else E.identity(E.manifest(harness=True)))
def verify(evidence,transport=None):
    E.require(read(S.static_base(evidence)/'stack-test-plan.json')==plan(),'stack current command/geometry plan differs')
    return {'status':'PASS','inherited':D.verify(evidence,transport),'stages':{mode:semantic(evidence,mode,transport=transport)
        for mode in ('static','host','build','runtime','production','fixtures')},
        'independent_review':'PENDING','hardware':'DEFERRED_TO_FINAL_CLOSURE','next_increment':'NOT_STARTED'}
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    p.add_argument('--evidence',required=True,type=Path);p.add_argument('--scratch-parent',type=Path);p.add_argument('--observation-baseline',type=Path)
    a=p.parse_args();os.chdir(E.ROOT);evidence=a.evidence.resolve();evidence.relative_to(E.ROOT/'backups');evidence.mkdir(parents=True,exist_ok=True)
    if a.scratch_parent:R.configure_scratch(a.scratch_parent)
    if a.observation_baseline:DEVICE.adopt_observation(evidence,a.observation_baseline.resolve())
    if a.mode=='all':
        DEVICE.P.storage_admission(evidence,DEVICE)
        for mode in ('static','host','build','runtime','production'):
            if not R.require_successor(reusable(evidence,mode)):
                if mode in ('build','runtime','production'):R.before_step(evidence,'runtime' if mode=='production' else mode)
                globals()[mode](evidence)
        if not R.require_successor(S.ledger(DEVICE,evidence)):
            G.run(evidence,'current-native-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(evidence)])
        if not R.require_successor(reusable(evidence,'fixtures')):R.before_step(evidence,'fixtures');fixtures(evidence)
        T.finalize(evidence)
        receipt(evidence,'acceptance',verify(evidence))
    elif a.mode=='verify':print(json.dumps(verify(evidence),sort_keys=True))
    else:
        if a.mode in ('build','runtime','production','fixtures'):R.before_step(evidence,'runtime' if a.mode=='production' else a.mode)
        globals()[a.mode](evidence)
    print(json.dumps({'mode':a.mode,'status':'PASS','evidence':str(evidence)}))
if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
