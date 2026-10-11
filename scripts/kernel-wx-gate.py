#!/usr/bin/env python3
"""Complete kernel image protection gate layered on the existing native campaign."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')!='guarded-kernel-stacks':
    os.environ['HOBBYOS_VALIDATION_CONTRACT']='kernel-image-protection'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E
import validation_resources as R
import validation_stages as S
import kernel_image_current as CURRENT
import stack_current as STACK
if STACK.enabled(): CURRENT=STACK
import kernel_image_evidence as V

def module(name):
    spec=importlib.util.spec_from_file_location('image_gate_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
D=module('low-half-gate');G=D.G;T=D.T;DEVICE=D.D.D
G.SURFACE+=('cmd_wxtest','protection_probe','protection_text_target','protection_rodata_target','protection_data_target')
def read(path):return json.loads(path.read_text())
def receipt(evidence,mode,result):T.receipt(evidence,'image-'+mode,result)
def plan():
    return {'schema':1,'contract':CURRENT.CONTRACT,'base_commit':CURRENT.BASE,'base_tree':CURRENT.TREE,
        'build_input_sha256':E.identity(E.manifest()),'inherited':D.plan(),'complete_boots':39,
        'command_plan':DEVICE.V.COMMANDS,'targets_per_cpu':{'nx':6,'without_nx':4},
        'unsupported_without_nx':[4,5],'fault_errors':{'write':3,'fetch':17},
        'main_effective_rwx_bytes_with_nx':0,'permission_activation':'UNPUBLISHED_CONSTRUCTION',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','stack_guards':'OWNED_ABSENT_GUARDS' if STACK.enabled() else 'NOT_STARTED'}

def static(evidence):
    base=evidence
    if (evidence/'image-static.json').exists():
        base=evidence/'stage-revisions/current-static'/G.stamp();base.mkdir(parents=True)
    D.static(base);E.write(base/'image-test-plan.json',plan())
    receipt(base,'static',CURRENT.boundary_semantics())
    if base!=evidence:S.seal_static(evidence,base)

def host(evidence):
    D.host(evidence);base=D.D.host_base(evidence);path=base/'image-host'/G.stamp()
    module('kernel-wx-host').run(path)
    receipt(base,'host',{'path':path.relative_to(base).as_posix(),'sha256':E.sha(path/'image-host.json')})

def codegen(path,testing):
    plan=V.elf_plan(path);names=plan['symbols'];text=(path/'tlb-disassembly.txt').read_text()
    for symbol in ('image_permissions_from_linker','image_permissions_check_leaf','image_walk_main','paging_image_boot_check'):
        E.require(symbol in names,'production image enforcement absent: '+symbol)
    def body(name):
        m=re.search(r'^[0-9a-f]+ <'+name+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',text,re.M|re.S)
        E.require(m is not None,'image emitted symbol missing: '+name);return m[1]
    payload=('cmd_wxtest','protection_probe_fixup','protection_probe_execute','protection_store_site',
        'protection_store_recovery','protection_fetch_recovery','protection_text_target',
        'protection_rodata_target','protection_data_target')
    for name in payload:E.require((name in names)==testing,'image production test payload isolation: '+name)
    import memory_elf_proof as ELF
    proof=read(path/'image-elf-proof.json');elf=ELF.replay(proof)
    E.require(proof['elf_sha256']==read(path/'candidate.json')['artifacts']['kernel.elf']['sha256'] and
        proof['elf_size']==read(path/'candidate.json')['artifacts']['kernel.elf']['size'] and
        V.verify_native_image(elf)['sections']==plan['sections'] and
        bool(V.fixed_probe_ranges(elf))==testing,'image bounded ELF/candidate/role binding')
    if testing:
        store=body('protection_store_site')
        fetch=body('protection_probe_fetch')+body('protection_fetch_call')
        recover=body('protection_fetch_recovery')
        E.require(re.search(r'mov\s+BYTE PTR \[rdi\],sil',store) and 'ud2' not in store,'image assembler store missing')
        E.require(re.search(r'call\s+rdi',fetch) and re.search(r'sub\s+rsp,0x8',fetch) and
            re.search(r'add\s+rsp,0x8',fetch) and 'ret' in fetch and 'ret' in recover and
            re.search(r'mov\s+eax,0x1',recover),'image fetch recovery call/return ABI')
        E.require(names['protection_store_site']!=names['protection_store_recovery'] and
            names['protection_fetch_recovery']!=names['protection_data_target'],'image fault/recovery sites collapsed')
    return {'status':'PASS','elf_sha256':read(path/'candidate.json')['artifacts']['kernel.elf']['sha256'],
        'disassembly_sha256':E.sha(path/'tlb-disassembly.txt'),'sections':plan['sections'],
        'section_plan_sha256':E.identity(plan['sections']),'test_payload':testing,
        'bootstrap_blob':'ORIGINAL_RO_NX_PRIVATE_DESTINATION_LIFETIME_UNCHANGED'}

def build(evidence):
    D.build(evidence)
    capture_image_elf_proofs(evidence)
    receipt(evidence,'build',{role:codegen(evidence/'candidates'/role,role in ('selftest','framebuffer-reference'))
        for role in ('selftest','production','debug','framebuffer-reference')})

def capture_image_elf_proofs(evidence):
    """Read current canonical binaries; publish new bounded facts, never new boots."""
    import memory_elf_proof as ELF
    for role in ('selftest','production','debug','framebuffer-reference'):
        directory=evidence/'candidates'/role;G.verify_local_candidate(directory)
        proof=ELF.capture(directory/'kernel.elf');path=directory/'image-elf-proof.json'
        if path.exists():E.require(read(path)==proof,'image bounded ELF proof changed')
        else:E.write(path,proof)

def runtime_proof(evidence):
    rows={};roots=[evidence,evidence/'nx-disabled',evidence/'xapic',evidence/'hub-keyboard',evidence/'high-ram',evidence/'high-ram-fallback']
    for base in roots:
        series=read(base/'series.json')
        for name in series['runs']+series.get('diagnostics',[]):
            path=base/name;key=path.relative_to(evidence).as_posix();E.require(key not in rows,'image repeated selected run')
            result=DEVICE.V.verify_complete_run(path)
            E.require(result['kernel_image']['status']=='PASS','image current complete predicate absent')
            rows[key]=result['kernel_image']
    E.require(sum(not read((evidence/name)/'result.json')['readback'] for name in rows)==39,'image functional coverage')
    return {'status':'PASS','runs':rows,'faults':sum(r['faults'] for r in rows.values()),
        'unsupported':sum(r['unsupported'] for r in rows.values()),'readbacks':sum('independent' in r for r in rows.values())}

def runtime(evidence):
    D.runtime(evidence)
    if (evidence/'image-runtime.json').exists():
        S.revalidate_runtime(evidence,'image',lambda path:reusable(evidence,'runtime',receipt_override=path))
        return
    receipt(evidence,'runtime',runtime_proof(evidence))
def production(evidence):
    D.production(evidence)
    receipt(evidence,'production',[DEVICE.V.verify_complete_run(evidence/name) for name in read(evidence/'production.json')['runs']])
def fixtures(evidence):
    D.fixtures(evidence);path=evidence/'image-fixtures'/G.stamp()
    module('kernel-wx-fixtures').run(evidence,path)
    receipt(evidence,'fixtures',{'path':path.relative_to(evidence).as_posix(),'sha256':E.sha(path/'results.json')})
def verify_host(evidence):
    inherited=D.verify_host(evidence);base=D.D.host_base(evidence);row=read(base/'image-host.json');path=base/row['result']['path']
    E.require(E.sha(path/'image-host.json')==row['result']['sha256'],'image hosted receipt binding')
    return {'inherited':inherited,'image':module('kernel-wx-host').verify(path)}
def semantic(evidence,mode,*,receipt_row=None):
    base=D.D.host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    row=receipt_row or read(S.runtime_receipt(evidence,'image') if mode=='runtime' else base/('image-'+mode+'.json'))
    if mode=='runtime':S.runtime_predecessor(evidence,row)
    E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'image stage source/status')
    if mode=='static':expected=CURRENT.boundary_semantics();E.require(read(base/'image-test-plan.json')==plan(),'image frozen plan')
    elif mode=='host':verify_host(evidence);return row['result']
    elif mode=='build':expected={role:codegen(evidence/'candidates'/role,role in ('selftest','framebuffer-reference')) for role in ('selftest','production','debug','framebuffer-reference')}
    elif mode=='runtime':expected=runtime_proof(evidence)
    elif mode=='production':expected=[DEVICE.V.verify_complete_run(evidence/name) for name in read(evidence/'production.json')['runs']]
    elif mode=='fixtures':
        path=evidence/row['result']['path']/'results.json';E.require(E.sha(path)==row['result']['sha256'],'image fixture bytes')
        module('kernel-wx-fixtures').verify_receipt(read(path));return row['result']
    E.require(json.loads(E.canonical(expected))==row['result'],'image stage semantic result differs');return expected

def reusable(evidence,mode,*,receipt_override=None):
    base=D.D.host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    def validate(row):
        E.require(D.reusable(evidence,mode)['state']=='REUSABLE','image inherited stage incomplete')
        return semantic(evidence,mode,receipt_row=row)
    path=S.runtime_receipt(evidence,'image') if mode=='runtime' else base/('image-'+mode+'.json')
    return R.classify(receipt_override or path,validate,source=E.identity(E.manifest()),
        harness=None if mode in ('build','production') else E.identity(E.manifest(harness=True)))
def verify(evidence,transport=None):
    E.require(read(S.static_base(evidence)/'image-test-plan.json')==plan(),'image complete command/capability plan differs')
    inherited=D.verify(evidence,transport)
    return {'status':'PASS','inherited':inherited,'stages':{mode:semantic(evidence,mode) for mode in
        ('static','host','build','runtime','production','fixtures')},'independent_review':'PENDING',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','next_increment':'NOT_STARTED'}
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
        for gate,prefix in ((DEVICE.P,'physical'),(DEVICE,'device'),(D.D,'direct'),(D,'low')):
            if not (evidence/(prefix+'-acceptance.json')).exists():gate.receipt(evidence,'acceptance',gate.verify(evidence))
        receipt(evidence,'acceptance',verify(evidence))
    elif a.mode=='verify':print(json.dumps(verify(evidence),sort_keys=True))
    else:
        if a.mode in ('build','runtime','production','fixtures'):R.before_step(evidence,'runtime' if a.mode=='production' else a.mode)
        globals()[a.mode](evidence)
    print(json.dumps({'mode':a.mode,'status':'PASS','evidence':str(evidence)}))
if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
