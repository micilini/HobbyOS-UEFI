#!/usr/bin/env python3
"""Complete no-low root and bounded AP lifetime gate, using the retained campaign."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') not in ('kernel-image-protection','guarded-kernel-stacks'):
    os.environ['HOBBYOS_VALIDATION_CONTRACT']='low-address-isolation'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E
import validation_resources as R
import validation_stages as S
import low_current as CURRENT
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
    import kernel_image_current as CURRENT
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
    import stack_current as CURRENT
import low_address_evidence as V

def module(name):
    spec=importlib.util.spec_from_file_location('low_gate_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
D=module('direct-map-gate');G=D.G;T=D.T
G.SURFACE+=('cmd_lowmap','null_probe','null_destructive','g_bootstrap_snapshot')
def read(path):return json.loads(path.read_text())
def receipt(evidence,mode,result):T.receipt(evidence,'low-'+mode,result)
def plan():
    return {'schema':1,'contract':CURRENT.CONTRACT,'base_commit':CURRENT.BASE,'base_tree':CURRENT.TREE,
        'build_input_sha256':E.identity(E.manifest()),'inherited':D.plan(),'complete_boots':39,
        'main_low_pml4_slots':256,'bootstrap_low_pages':1,'private_tables':4,'bsp_stack_bytes':16384,
        'null_destructive_profiles':['q35-tcg-smp1','q35-kvm-smp4'],'diagnostic':'PHYSICAL_PRE_LAUNCH_AND_POST_RETIREMENT',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','image_permissions':'CONSTRUCTION_PROTECTED' if CURRENT.CONTRACT in ('kernel-image-protection','guarded-kernel-stacks') else 'UNCHANGED'}

def static(evidence):
    D.static(evidence);E.write(evidence/'low-test-plan.json',plan())
    receipt(evidence,'static',CURRENT.boundary_semantics())

def host(evidence):
    D.host(evidence)
    destination=D.host_base(evidence)
    path=destination/'low-host'/G.stamp();module('low-half-host').run(path)
    receipt(destination,'host',{'path':path.relative_to(destination).as_posix(),'sha256':E.sha(path/'low-host.json')})

def codegen(path,testing):
    text=(path/'tlb-disassembly.txt').read_text();names=V.symbols((path/'symbols.txt').read_text())
    def body(name):
        found=re.search(r'^[0-9a-f]+ <'+name+r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',text,re.M|re.S)
        E.require(found is not None,'low-root emitted symbol missing: '+name);return found[1]
    for name in ('paging_build_verify','paging_bootstrap_retire','bootstrap_tables_verify','bootstrap_lifetime_depart',
        'pmm_claim_bootstrap','low_fault_description','physical_translation_retire_identity'):
        E.require(name in names,'production no-low enforcement absent: '+name)
    E.require(names['bsp_stack_top']-names['bsp_stack_bottom']==16384 and
        names['bsp_stack_bottom']>=0xffffffff80000000 and names['bsp_stack_top']-0xffffffff80000000<=0x40000000,
        'BSP stack emitted backing/early coverage')
    entry_symbol='_start'
    if CURRENT.CONTRACT in ('kernel-image-protection','guarded-kernel-stacks'):
        E.require(names['_start']==names['_boot_text_start'],'BSP entry/linker boundary mismatch')
        if '<_start>:' not in text:entry_symbol='_boot_text_start'
    entry=text[text.index('<'+entry_symbol+'>:'):text.index('<'+entry_symbol+'>:')+20000]
    E.require(re.search(r'movabs\s+rsp,0x%x\b'%names['bsp_stack_top'],entry), 'BSP high stack installation absent from entry')
    ap=body('ap_kernel_entry')
    sequence=['<gdt_flush>','ltr','<idt_load>','<paging_bootstrap_handoff_ready>',
        '<paging_load_kernel_root>','<cpu_policy_confirm_local>','<paging_bootstrap_depart>','<smp_mark_cpu_online>']
    E.require([ap.index(x) for x in sequence]==sorted(ap.index(x) for x in sequence),'emitted AP descriptor/root/readiness order')
    for name in ('null_probe_fault','null_probe_recovery','null_destructive_fault','cmd_lowmap','g_bootstrap_snapshot'):
        E.require((name in names)==testing,'NULL production payload isolation: '+name)
    if testing:
        for name in ('null_probe_fault','null_destructive_fault'):
            actual=body(name)
            E.require(re.search(r'48 8b 00\s+mov\s+rax,QWORD PTR \[rax\]',actual) and 'ud2' not in actual,
                'NULL site is not the real assembler read')
        E.require(names['null_probe_recovery']!=names['null_probe_fault']!=names['null_destructive_fault'],'NULL site identities collapsed')
    return {'status':'PASS','elf_sha256':read(path/'candidate.json')['artifacts']['kernel.elf']['sha256'],
        'disassembly_sha256':E.sha(path/'tlb-disassembly.txt'),'bsp_stack':[names['bsp_stack_bottom'],names['bsp_stack_top']],
        'test_payload':testing,'trampoline':'UNCHANGED_ACCEPTED_BLOB'}

def build(evidence):
    D.build(evidence)
    receipt(evidence,'build',{role:codegen(evidence/'candidates'/role,role in ('selftest','framebuffer-reference'))
        for role in ('selftest','production','debug','framebuffer-reference')})

def destructive(evidence):
    selected={}
    for accel,smp in (('tcg',1),('kvm',4)):
        R.before_step(evidence,'runtime');name='null-faults/'+G.stamp()+f'-q35-{accel}-smp{smp}';path=evidence/name
        try:
            G.run(evidence,'null-fault-'+accel,['python3','-B','scripts/memory-qemu.py','--evidence',str(path),
                '--candidate',str(evidence/'candidates/selftest'),'--machine','q35','--accel',accel,'--smp',str(smp),
                '--serial-backend','file','--fault-trace','--command','lowmap crash'])
        except RuntimeError:pass
        actual=V.destructive(path)
        import validation_retention
        validation_retention.retire_run(path)
        selected[f'q35-{accel}-smp{smp}']={'path':name,'result_sha256':E.sha(path/'result.json'),'proof':actual}
    E.write(evidence/'low-destructive.json',selected)

def destructive_proof(evidence):
    rows=read(evidence/'low-destructive.json');E.require(set(rows)==set(plan()['null_destructive_profiles']),'NULL destructive profile coverage')
    result={};boots=set()
    for profile,row in rows.items():
        E.safe(row['path']);path=evidence/row['path'];raw=read(path/'result.json');actual=V.destructive(path)
        E.require(raw['profile']==profile and actual==row['proof'] and E.sha(path/'result.json')==row['result_sha256'],
            'NULL destructive result binding')
        E.require(raw['boot_id'] not in boots,'NULL destructive repeated boot');boots.add(raw['boot_id']);result[profile]=actual
    return result

def runtime(evidence):
    D.runtime(evidence)
    if (evidence/'low-runtime.json').exists():
        S.revalidate_runtime(evidence,'low',lambda path:reusable(evidence,'runtime',receipt_override=path))
        return
    if not (evidence/'low-destructive.json').exists():destructive(evidence)
    receipt(evidence,'runtime',{'bootstrap':V.readback(evidence/read(evidence/'direct-owner-readback.json')['path']),
        'destructive':destructive_proof(evidence)})

def production(evidence):
    D.production(evidence)
    receipt(evidence,'production',[V.verify_run(evidence/p) for p in read(evidence/'production.json')['runs']])

def fixtures(evidence):
    D.fixtures(evidence);path=evidence/'low-fixtures'/G.stamp()
    module('low-half-fixtures').run(evidence,path)
    receipt(evidence,'fixtures',{'path':path.relative_to(evidence).as_posix(),'sha256':E.sha(path/'results.json')})

def verify_host(evidence):
    inherited=D.verify_host(evidence);base=D.host_base(evidence);row=read(base/'low-host.json')
    path=base/row['result']['path'];E.require(E.sha(path/'low-host.json')==row['result']['sha256'],'NULL host receipt binding')
    return {'inherited':inherited,'low':module('low-half-host').verify(path)}

def semantic(evidence,mode,*,receipt_row=None):
    base=D.host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    row=receipt_row or read(S.runtime_receipt(evidence,'low') if mode=='runtime' else base/('low-'+mode+'.json'))
    if mode=='runtime':S.runtime_predecessor(evidence,row)
    E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'low-domain stage source/status')
    if mode=='static':expected=CURRENT.boundary_semantics();E.require(read(S.static_base(evidence)/'low-test-plan.json')==plan(),'low-domain frozen plan')
    elif mode=='host':verify_host(evidence);return row['result']
    elif mode=='build':expected={role:codegen(evidence/'candidates'/role,role in ('selftest','framebuffer-reference'))
        for role in ('selftest','production','debug','framebuffer-reference')}
    elif mode=='runtime':expected={'bootstrap':V.readback(evidence/read(evidence/'direct-owner-readback.json')['path']),
        'destructive':destructive_proof(evidence)}
    elif mode=='production':expected=[V.verify_run(evidence/p) for p in read(evidence/'production.json')['runs']]
    elif mode=='fixtures':
        path=evidence/row['result']['path']/'results.json';E.require(E.sha(path)==row['result']['sha256'],'low-domain fixture byte binding')
        module('low-half-fixtures').verify_receipt(read(path));return row['result']
    # JSON receipts represent CPU-slot dictionary keys as strings.
    expected=json.loads(E.canonical(expected))
    E.require(expected==row['result'],'low-domain semantic stage differs');return expected

def reusable(evidence,mode,*,receipt_override=None):
    def validate(row):
        E.require(D.reusable(evidence,mode)['state']=='REUSABLE','inherited stage not reusable')
        return semantic(evidence,mode,receipt_row=row)
    base=D.host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    path=S.runtime_receipt(evidence,'low') if mode=='runtime' else base/('low-'+mode+'.json')
    return R.classify(receipt_override or path,validate,source=E.identity(E.manifest()),
        harness=None if mode in ('build','production') else E.identity(E.manifest(harness=True)))

def verify(evidence,transport=None):
    E.require(read(S.static_base(evidence)/'low-test-plan.json')==plan(),'low-domain current command plan differs')
    inherited=D.verify(evidence,transport)
    stages={mode:semantic(evidence,mode) for mode in ('static','host','build','runtime','production','fixtures')}
    return {'status':'PASS','inherited':inherited,'stages':stages,'independent_review':'PENDING',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','next_increment':'NOT_STARTED'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    p.add_argument('--evidence',required=True,type=Path);p.add_argument('--scratch-parent',type=Path);p.add_argument('--observation-baseline',type=Path)
    a=p.parse_args();os.chdir(E.ROOT);evidence=a.evidence.resolve();evidence.relative_to(E.ROOT/'backups');evidence.mkdir(parents=True,exist_ok=True)
    if a.scratch_parent:R.configure_scratch(a.scratch_parent)
    if a.observation_baseline:D.D.adopt_observation(evidence,a.observation_baseline.resolve())
    if a.mode=='all':
        D.D.P.storage_admission(evidence,D.D)
        for mode in ('static','host','build','runtime','production'):
            if not R.require_successor(reusable(evidence,mode)):
                if mode in ('build','runtime','production'):R.before_step(evidence,'runtime' if mode=='production' else mode)
                globals()[mode](evidence)
        if not R.require_successor(S.ledger(D.D,evidence)):
            G.run(evidence,'current-native-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(evidence)])
        if not R.require_successor(reusable(evidence,'fixtures')):R.before_step(evidence,'fixtures');fixtures(evidence)
        T.finalize(evidence)
        for gate,prefix in ((D.D.P,'physical'),(D.D,'device'),(D,'direct')):
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
