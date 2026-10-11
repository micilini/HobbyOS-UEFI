#!/usr/bin/env python3
"""Labeled immutable-base contradictions reaching the no-low semantic validators."""
import copy
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import memory_evidence as E
import low_address_evidence as V
import low_current as C

CASES={
    'missing-final-root':'missing/duplicate [LOW_ROOT] ACTIVE',
    'bsp-stack-low':'actual BSP stack is not the complete high 16-KiB stack',
    'bsp-descriptor-low':'BSP descriptor state is not high',
    'ap-root-still-private':'AP final CR3 precedes safe high stack/descriptor departure',
    'ap-online-without-readback':'local CPU readback coverage',
    'ap-missing-departure':'bootstrap lifetime records incomplete',
    'ap-stale-generation':'AP final CR3 precedes safe high stack/descriptor departure',
    'ap-physical-stack':'AP final CR3 precedes safe high stack/descriptor departure',
    'ap-low-gdt':'AP final CR3 precedes safe high stack/descriptor departure',
    'hidden-intended-ap':'private bootstrap root/claim/target set',
    'bootstrap-root-high':'private bootstrap root/claim/target set',
    'trampoline-zero':'private bootstrap root/claim/target set',
    'retirement-before-online':'AP root/departure/readiness/retirement chronology',
    'retirement-wrong-free-delta':'private retirement/free/observer accounting',
    'smp1-invented-retirement':'SMP1 allocated or fabricated bootstrap retirement',
    'fabricated-null-result':'NULL result lacks the exact non-present supervisor read fault',
    'wrong-null-site':'NULL result lacks the exact non-present supervisor read fault',
    'wrong-null-address':'NULL result lacks the exact non-present supervisor read fault',
    'wrong-null-error':'NULL result lacks the exact non-present supervisor read fault',
    'wrong-null-cpu':'NULL result lacks the exact non-present supervisor read fault',
    'inactive-null-generation':'NULL result lacks the exact non-present supervisor read fault',
    'post-switch-old-owner':'post-CR3 readback uses construction CPU pointers',
    'ap-main-before-descriptors':'AP main root precedes high descriptor readiness',
    'duplicate-trampoline-reservation':'trampoline reservation/root/stack ownership',
    'bsp-no-high-stack-switch':'BSP high stack missing before first C frame',
    'low-first-raw-entry':'reported empty lower half contradicted by actual PML4 bytes',
    'low-middle-raw-entry':'reported empty lower half contradicted by actual PML4 bytes',
    'low-last-nonpresent-entry':'reported empty lower half contradicted by actual PML4 bytes',
    'private-extra-low-page':'bootstrap low map is broader than the exact trampoline page',
    'private-shared-high-link':'bootstrap low map is broader than the exact trampoline page',
    'retired-observer-pointer':'retired table owners/observer/free accounting',
    'retired-frame-still-owned':'bootstrap PMM page not released exactly',
    'missing-prelaunch-bytes':'missing independent private physical table bytes',
    'other-vm-bootstrap-tail':'bootstrap diagnostic phase/candidate/root binding',
    'production-null-probe':'NULL production payload isolation: null_probe_fault',
    'production-null-destructive':'NULL production payload isolation: null_destructive_fault',
    'production-null-command-present':'production command is present or did not return unknown',
    'production-null-historical-scope':'production command is present or did not return unknown',
    'production-receipt-slot-identity':'low-domain semantic stage differs',
    'production-receipt-root-identity':'low-domain semantic stage differs',
    'portable-statistics-base-identity':'portable statistics linker successor differs',
    'portable-statistics-baseline-bytes':'portable statistics linker successor differs',
    'portable-statistics-current-bytes':'portable statistics linker successor differs',
    'portable-statistics-promotion':'portable statistics linker successor differs',
    'portable-statistics-historical-scope':'current statistics linker continuity',
}

def verify_receipt(row):
    E.require(row['schema']==1 and row['status']=='PASS' and
        len(row['cases'])==len(CASES) and {c['name'] for c in row['cases']}==set(CASES),'low-domain negative coverage')
    for case in row['cases']:
        E.require(case['synthetic'] is True and case['actual_exit']==1 and
            case['expected_boundary']==CASES[case['name']] and CASES[case['name']] in case['actual_reason'],
            'low-domain negative stopped at unrelated boundary')
    E.require(row['restored_positives']==['smp1','smp4','physical-pre-post','source-transition','production-codegen','production-absence','production-receipt-json','portable-statistics-linker','historical-statistics-linker'],
        'low-domain restored controls incomplete')

def run(evidence,output):
    output.mkdir(parents=True,exist_ok=False);cases=[]
    selected=json.loads((evidence/'direct-owner-readback.json').read_text())
    diagnostic=evidence/selected['path'];series=json.loads((evidence/'series.json').read_text())
    single=evidence/next(p for p in series['runs'] if p.startswith('profiles/q35-tcg-smp1-'))
    lines=(diagnostic/'qemu-serial.log').read_text().splitlines();elf=(diagnostic/'symbols.txt').read_text()
    base=V.verify_run(diagnostic);V.readback(diagnostic);V.verify_run(single)
    hashes={p.relative_to(diagnostic).as_posix():E.sha(p) for p in diagnostic.rglob('*') if p.is_file()}
    def reject(name,action,recipe):
        try:action()
        except (ValueError,KeyError,AssertionError) as error:
            reason=str(error);E.require(CASES[name] in reason,'low-domain unrelated rejection '+name+': '+reason)
        else:raise ValueError('low-domain contradiction accepted: '+name)
        cases.append({'name':name,'synthetic':True,'actual_exit':1,'expected_boundary':CASES[name],
            'actual_reason':reason,'recipe':recipe})
    def change_line(data,prefix,key,value):
        at=next(i for i,l in enumerate(data) if l.startswith(prefix));row=V.fields(data[at],prefix);row[key]=value
        data[at]=prefix+' '.join(k+'='+v for k,v in row.items())
    recipes={
        'bsp-stack-low':('[LOW_ROOT] ACTIVE ','rsp','10000'),
        'bsp-descriptor-low':('[LOW_ROOT] PRELOAD ','gdtr','10000'),
        'ap-root-still-private':('[AP_ROOT] DEPART ','root',f'{base["temporary_root"]:016x}'),
        'ap-stale-generation':('[AP_ROOT] DEPART ','generation','0'),
        'ap-physical-stack':('[AP_ROOT] DEPART ','rsp','300000'),
        'ap-low-gdt':('[AP_ROOT] DEPART ','gdtr','10000'),
        'hidden-intended-ap':('[AP_ROOT] CREATED ','intended','2'),
        'bootstrap-root-high':('[AP_ROOT] CREATED ','root','100100000'),
        'trampoline-zero':('[AP_ROOT] CREATED ','trampoline','0'),
        'retirement-wrong-free-delta':('[AP_ROOT] RETIRED ','freed','3')}
    for name in list(CASES)[:15]:
        changed=list(lines);topology=4;names=elf
        if name in recipes:change_line(changed,*recipes[name])
        elif name=='missing-final-root':changed=[l for l in changed if not l.startswith('[LOW_ROOT] ACTIVE ')]
        elif name in ('ap-online-without-readback','ap-missing-departure'):
            prefix='[CPU_STATE][READY] ' if name=='ap-online-without-readback' else '[AP_ROOT] DEPART '
            at=next(i for i,l in enumerate(changed) if l.startswith(prefix) and 'slot=1 ' in l);changed.pop(at)
        elif name=='retirement-before-online':
            at=next(i for i,l in enumerate(changed) if l.startswith('[AP_ROOT] RETIRED '));row=changed.pop(at)
            at=next(i for i,l in enumerate(changed) if l.startswith('[AP_ROOT] DEPART '));changed.insert(at,row)
        elif name=='smp1-invented-retirement':
            changed=(single/'qemu-serial.log').read_text().splitlines();names=(single/'symbols.txt').read_text();topology=1
            changed.append(next(l for l in lines if l.startswith('[AP_ROOT] RETIRED ')))
        reject(name,lambda:V.boot(changed,names,topology),{'base':selected['path'],'operation':name,'metadata':'SYNTHETIC_SERIAL_ONLY'})
        V.boot(lines,elf,4)
    null=V.records(lines,'[LOW_ROOT][NULL] ');E.require(len(null)==1,'NULL immutable positive')
    for name,key,value in [('fabricated-null-result','fault','0'),('wrong-null-site','rip','ffffffff82000000'),
        ('wrong-null-address','cr2','10000'),('wrong-null-error','error','1'),('wrong-null-cpu','slot','32'),
        ('inactive-null-generation','generation','0')]:
        changed=list(lines);change_line(changed,'[LOW_ROOT][NULL] ',key,value)
        reject(name,lambda:V.probe(changed,elf,base['topology']),{'base':selected['path'],'field':key,'value':value})
        V.probe(lines,elf,base['topology'])
    sources=[(E.ROOT/p).read_text() for p in ('kernel/src/memory/paging.c','kernel/src/memory/paging_build.c',
        'kernel/src/smp/smp_boot.c','kernel/src/core/entry.S')]
    for name in ('post-switch-old-owner','ap-main-before-descriptors','duplicate-trampoline-reservation','bsp-no-high-stack-switch'):
        changed=list(sources)
        if name=='post-switch-old-owner':changed[0]=changed[0].replace('paging_table_hash((const PageTable *)(uintptr_t)pointer)','paging_table_hash(table->cpu->entries)',1)
        elif name=='ap-main-before-descriptors':changed[2]=changed[2].replace('    paging_load_kernel_root();','',1).replace('    gdt_flush(', '    paging_load_kernel_root();\n    gdt_flush(',1)
        elif name=='duplicate-trampoline-reservation':changed[2]=changed[2].replace('    uint64_t boot_cr3;','    uint64_t boot_cr3;\n    pmm_mark_frame_used((void *)0x10000);',1)
        else:changed[3]=changed[3].replace('movabs rsp, offset bsp_stack_top','movabs rax, offset bsp_stack_top',1)
        reject(name,lambda:C.transition_semantics(*changed),{'source_sha256':[E.identity(v) for v in sources],
            'mutation':name,'source_binding':'Explicit synthetic inputs to the actual static semantic helper'})
        C.transition_semantics(*sources)
    # One disposable materialization at a time; originals remain immutable.
    with tempfile.TemporaryDirectory(prefix='low-root-negative-') as temporary:
        work=Path(temporary)/'capture';shutil.copytree(diagnostic,work)
        def restore(name):
            target=work/name;target.write_bytes((diagnostic/name).read_bytes())
        for name in list(CASES)[25:]:
            if name.startswith(('production-','portable-statistics-')):continue
            modified=set()
            def save_json(path,row):
                (work/path).write_bytes(E.canonical(row));modified.add(path)
            def save_bytes(path,data):
                (work/path).write_bytes(data);modified.add(path)
            if name.startswith('low-'):
                meta=json.loads((work/'table-snapshot.json').read_text());data=bytearray((work/'table-frames.bin').read_bytes())
                slot={'low-first-raw-entry':0,'low-middle-raw-entry':127,'low-last-nonpresent-entry':255}[name]
                at=meta['frame_addresses'].index(meta['active_root'])*4096+slot*8;struct.pack_into('<Q',data,at,2)
                save_bytes('table-frames.bin',data);meta['frames_sha256']=E.sha(work/'table-frames.bin');save_json('table-snapshot.json',meta)
                action=lambda:V.verify_run(work)
            elif name in ('private-extra-low-page','private-shared-high-link','missing-prelaunch-bytes'):
                data=bytearray((work/'bootstrap-before-private.bin').read_bytes());row=json.loads((work/'bootstrap-before.json').read_text())
                if name=='private-extra-low-page':struct.pack_into('<Q',data,3*4096,3)
                elif name=='private-shared-high-link':struct.pack_into('<Q',data,256*8,struct.unpack_from('<Q',data,256*8)[0]^4096)
                else:data=data[:-4096]
                save_bytes('bootstrap-before-private.bin',data);row['private_sha256']=E.sha(work/'bootstrap-before-private.bin');save_json('bootstrap-before.json',row);action=lambda:V.readback(work)
            elif name=='retired-observer-pointer':
                data=(base['temporary_root']+V.BASE).to_bytes(8,'little');path='bootstrap-retired-g_observer_boot_root.bin';save_bytes(path,data)
                row=json.loads((work/'bootstrap-retired.json').read_text());row['objects']['g_observer_boot_root']['sha256']=E.sha(work/path);save_json('bootstrap-retired.json',row);action=lambda:V.readback(work)
            elif name=='retired-frame-still-owned':
                row=json.loads((work/'bootstrap-retired.json').read_text());item=row['allocation_bits'][0];fact=item['bits']['g_bitmap_buffer']
                fact['byte']|=1<<((item['physical']//4096)%8);fact['value']=1;save_json('bootstrap-retired.json',row);action=lambda:V.readback(work)
            else:
                row=json.loads((work/'bootstrap-retired.json').read_text());row['vm_id']='synthetic-different-vm';save_json('bootstrap-retired.json',row);action=lambda:V.readback(work)
            # Rebind only the explicitly synthetic raw debugger result, preserving command and source facts.
            for phase in ('bootstrap-before','bootstrap-retired'):
                if phase+'.json' not in modified:continue
                stdout='bootstrap-before.stdout.log';text=(work/stdout).read_text();original=json.loads((diagnostic/(phase+'.json')).read_text());new=json.loads((work/(phase+'.json')).read_text())
                text=text.replace('LOW_ROOT_GDB '+json.dumps(original,sort_keys=True,separators=(',',':')),
                    'LOW_ROOT_GDB '+json.dumps(new,sort_keys=True,separators=(',',':')));save_bytes(stdout,text.encode())
            reject(name,action,{'base':selected['path'],'files':sorted(modified),'outer_metadata':'Explicit synthetic raw checksum/record rebinding'})
            for path in modified:restore(path)
            V.readback(work)
        production=Path(temporary)/'production';production.mkdir()
        origin=evidence/'candidates/production'
        for name in ('symbols.txt','tlb-disassembly.txt','candidate.json'):
            shutil.copyfile(origin/name,production/name)
        gate=V.module('low-half-gate');original=(production/'symbols.txt').read_bytes()
        positive=gate.codegen(production,False)
        for name,symbol in (('production-null-probe','null_probe_fault'),
                            ('production-null-destructive','null_destructive_fault')):
            (production/'symbols.txt').write_bytes(original+b'\nffffffff82001000 T '+symbol.encode()+b'\n')
            reject(name,lambda:gate.codegen(production,False),{'base':'candidates/production',
                'symbols_sha256':E.sha(origin/'symbols.txt'),'added_symbol':symbol,'binding':'SYNTHETIC_CODEGEN_INPUT'})
            (production/'symbols.txt').write_bytes(original)
            E.require(gate.codegen(production,False)==positive,'production codegen positive not restored')
    selected_production=json.loads((evidence/'production.json').read_text())['runs'][0]
    E.safe(selected_production);production=evidence/selected_production
    observed=json.loads((production/'result.json').read_text())
    commands=[json.loads(line) for line in (production/'framed.jsonl').read_text().splitlines()]
    absent=[row for row in commands if row['payload'] in ('lowmap','lowmap crash')]
    E.require(len(absent)==2 and {r['payload'] for r in absent}=={'lowmap','lowmap crash'},'production NULL absence controls missing')
    transcript=[];ends=[];offset=0
    for line in (production/'qemu-serial.log').read_bytes().splitlines(keepends=True):
        offset+=len(line);transcript.append(line.rstrip(b'\r\n').decode('ascii'));ends.append(offset)
    observer=V.module('verify-memory-observation')
    profile={'name':observed['profile'],'vm_id':observed['vm_id']}
    def absence(row):
        observer.production_absence_transaction(row,production,transcript,ends,profile,observed['candidate_elf'])
    for row in absent:absence(row)
    changed=dict(absent[0],handler_status=0)
    reject('production-null-command-present',lambda:absence(changed),{
        'base':selected_production,'record_sha256':E.identity(absent[0]),
        'field':'handler_status','value':0,'binding':'SYNTHETIC_SEMANTIC_INPUT'})
    for row in absent:absence(row)
    contract=os.environ.get('HOBBYOS_VALIDATION_CONTRACT')
    try:
        os.environ['HOBBYOS_VALIDATION_CONTRACT']='verified-ram-direct-map'
        reject('production-null-historical-scope',lambda:absence(absent[1]),{
            'base':selected_production,'record_sha256':E.identity(absent[1]),
            'contract':'verified-ram-direct-map','binding':'SYNTHETIC_CONTRACT_SELECTION'})
    finally:
        if contract is None:os.environ.pop('HOBBYOS_VALIDATION_CONTRACT',None)
        else:os.environ['HOBBYOS_VALIDATION_CONTRACT']=contract
    for row in absent:absence(row)
    receipt=json.loads((evidence/'low-production.json').read_text())
    positive=gate.semantic(evidence,'production',receipt_row=receipt)
    E.require(positive==json.loads(E.canonical(positive))==receipt['result'],
        'production receipt is not stable across JSON serialization')
    target=next(i for i,r in enumerate(receipt['result']) if r['cpus']==4)
    for name in ('production-receipt-slot-identity','production-receipt-root-identity'):
        synthetic=copy.deepcopy(receipt)
        if name.endswith('slot-identity'):synthetic['result'][target]['topology']['1']^=1
        else:synthetic['result'][target]['root']^=4096
        reject(name,lambda:gate.semantic(evidence,'production',receipt_row=synthetic),{
            'base':'low-production.json','sha256':E.sha(evidence/'low-production.json'),
            'mutation':name,'binding':'SYNTHETIC_RECEIPT_ONLY'})
        E.require(gate.semantic(evidence,'production',receipt_row=receipt)==positive,
            'production receipt positive not restored')
    selected_statistics=json.loads((evidence/'regression-selection.json').read_text())['taskman-statistics']
    E.safe(selected_statistics);statistics=evidence/selected_statistics/'evidence'
    portable=V.module('memory-portable-replay');base_commit=json.loads((statistics/'result.json').read_text())['implementation_base_commit']
    names=('source/kernel/link.ld','static/base-link.ld','static/linker-successor.json')
    originals={name:(statistics/name).read_bytes() for name in names}
    portable.statistics_linker(statistics,base_commit)
    with tempfile.TemporaryDirectory(prefix='low-statistics-linker-') as temporary:
        work=Path(temporary)
        for name,data in originals.items():
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        for name,target in [('portable-statistics-base-identity',None),
                            ('portable-statistics-baseline-bytes','static/base-link.ld'),
                            ('portable-statistics-current-bytes','source/kernel/link.ld'),
                            ('portable-statistics-promotion','static/linker-successor.json')]:
            expected_base=base_commit if target else '0'*40
            if target=='static/linker-successor.json':
                changed=json.loads(originals[target]);changed['baseline_sha256']='0'*64
                (work/target).write_bytes(E.canonical(changed))
            elif target:(work/target).write_bytes(originals[target]+b'\n/* synthetic contradiction */\n')
            reject(name,lambda:portable.statistics_linker(work,expected_base),{
                'base':selected_statistics,'mutation':name,'binding':'SYNTHETIC_LINKER_INPUT',
                'source_sha256':{n:E.sha(statistics/n) for n in names}})
            if target:(work/target).write_bytes(originals[target])
            portable.statistics_linker(work,base_commit)
        saved_contract=os.environ.get('HOBBYOS_VALIDATION_CONTRACT')
        try:
            os.environ['HOBBYOS_VALIDATION_CONTRACT']='verified-ram-direct-map'
            reject('portable-statistics-historical-scope',lambda:portable.statistics_linker(work,base_commit),{
                'base':selected_statistics,'contract':'verified-ram-direct-map','binding':'SYNTHETIC_CONTRACT_SELECTION'})
            (work/'static/base-link.ld').write_bytes(originals['source/kernel/link.ld'])
            portable.statistics_linker(work,base_commit)
        finally:
            if saved_contract is None:os.environ.pop('HOBBYOS_VALIDATION_CONTRACT',None)
            else:os.environ['HOBBYOS_VALIDATION_CONTRACT']=saved_contract
        (work/'static/base-link.ld').write_bytes(originals['static/base-link.ld'])
        portable.statistics_linker(work,base_commit)
    E.require(originals=={name:(statistics/name).read_bytes() for name in names},'original statistics linker evidence mutated')
    E.require(hashes=={p.relative_to(diagnostic).as_posix():E.sha(p) for p in diagnostic.rglob('*') if p.is_file()},'original bootstrap evidence mutated')
    V.verify_run(single);V.verify_run(diagnostic);V.readback(diagnostic);C.transition_semantics(*sources)
    result={'schema':1,'status':'PASS','cases':cases,'restored_positives':['smp1','smp4','physical-pre-post','source-transition','production-codegen','production-absence','production-receipt-json','portable-statistics-linker','historical-statistics-linker'],
        'build_input_sha256':E.identity(E.manifest()),'harness_sha256':E.identity(E.manifest(harness=True)),
        'base':selected['path'],'base_result_sha256':E.sha(diagnostic/'result.json')}
    verify_receipt(result);E.write(output/'results.json',result);return result
