#!/usr/bin/env python3
"""Real-C mutations and semantic address contradictions over genuine captures."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import tempfile
import memory_evidence as E
import physical_current as P
import direct_map_evidence as RAM

ROOT=Path(__file__).resolve().parent.parent
def module(name):
    spec=importlib.util.spec_from_file_location('physical_fixture_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def selector_module():
    # The historical selector owns its CLI contract, not its caller's replay.
    key='HOBBYOS_VALIDATION_CONTRACT';contract=os.environ.get(key)
    try:return module('physical-translation-review')
    finally:
        if contract is None:os.environ.pop(key,None)
        else:os.environ[key]=contract
V=module('verify-physical-translation')
COMPILED=[
 ('known-but-unmapped','kernel/src/memory/physical_contract.c',' || end > c->identity_limit','', 'boundary'),
 ('heap-image-offset','kernel/src/memory/physical_contract.c','*virtual_address = physical;','*virtual_address = physical & ~UINT64_C(0xffffffff80000000);','boundary'),
 ('missing-glyph-conversion','kernel/src/memory/boot_address.c','if (!boot_pointer(g_objects.glyphs, g_objects.glyph_bytes, &g_font.glyph_buffer)) return false;',
  'if (!g_font.glyph_buffer) return false;','boot'),
 ('bootstrap-reopened','kernel/src/memory/boot_address.c','g_physical_contract.ready || !out','!out','boot'),
 ('stale-borrow','kernel/src/memory/physical_contract.c','borrow->epoch == c->epoch &&','1 &&','boundary'),
 ('bounded-search-over-ceiling','kernel/src/utils/bitmap.c','const Bitmap view = {bitmap->buffer, bounded, exclusive_limit};',
  'const Bitmap view = {bitmap->buffer, bitmap->size, bitmap->total_bits};','boundary'),
 ('pmm-ignores-ceiling','kernel/src/memory/pmem.c','(size_t)g_alloc_limit_frames, 1,','(size_t)g_total_frames, 1,','pmm'),
 ('pmm-contiguous-ignores-ceiling','kernel/src/memory/pmem.c','(size_t)g_alloc_limit_frames, count,','(size_t)g_total_frames, count,','pmm'),
 ('mmio-mislabeled-ram','kernel/src/memory/physical_contract.c','(kind && !(r->kind & kind))','false','boundary'),
 ('high-corruption-hidden','kernel/src/utils/bitmap.c','word < words; word++','word < words && word < 16384; word++','boundary'),
]

if RAM.enabled():
    COMPILED[0]=('known-but-unmapped','kernel/src/memory/physical_contract.c',
        'if (end > c->identity_limit) return false;', '', 'boundary')

def run(evidence,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=False);cases=[]
    def reject(name,reason,action,recipe):
        try:action()
        except (ValueError,OSError) as error:
            actual=str(error);E.require(reason in actual,'translation fixture wrong boundary: '+name+': '+actual)
        else:raise ValueError('translation fixture accepted: '+name)
        cases.append({'name':name,'synthetic':True,'expected_boundary':reason,'actual_reason':actual,
                      'expected_exit':1,'actual_exit':1,'recipe':recipe})
    series=json.loads((evidence/'series.json').read_text())
    base=evidence/next(n for n in series['runs'] if '/q35-kvm-smp4-' in n)
    lines=(base/'qemu-serial.log').read_text().splitlines()
    commands=[json.loads(s) for s in (base/'framed.jsonl').read_text().splitlines()]
    row=next(r for r in commands if r['payload']=='phystest')
    O=module('verify-memory-observation');segment=O.FOUNDATION.transaction_segment(lines,row)
    import re
    topology={int(m[1]):(int(m[2]),m[3]) for line in lines
              if (m:=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line))}
    candidate=json.loads((base/'candidate.json').read_text())
    root=V.number(V.records(segment,'RESULT')[0],'root');image=candidate['symbols']['_kernel_start']
    ram=RAM.activation(lines) if RAM.enabled() else None
    positive=V.fixture(segment,topology,root,image,ram=ram)
    def mutate_record(kind,index,changes):
        changed=list(segment);matches=[i for i,line in enumerate(changed) if line.startswith('[PHYSICAL]['+kind+'] ')]
        at=matches[index];fields=V.records([changed[at]],kind)[0];fields.update(changes)
        changed[at]='[PHYSICAL]['+kind+'] '+' '.join(k+'='+str(v) for k,v in fields.items());return changed
    ceiling=V.number(positive['accounting'],'ceiling')
    for name,kind,index,changes,reason in [
        ('wrong-physical-root','RESULT',0,{'root':'0xffffffff80100000'},'physical root/owned-frame'),
        ('image-offset-on-heap','POINTER',2,{'virtual':'0xffffffff80400000'},'heap/identity image-offset'),
        ('at-exclusive-ceiling','POINTER',3,{'virtual':hex(ceiling),'physical':hex(ceiling),'walked':hex(ceiling)},'heap/identity image-offset'),
        ('contiguous-over-ceiling','POINTER',4,{'virtual':hex(ceiling-4096),'physical':hex(ceiling-4096),'walked':hex(ceiling-4096)},'heap/identity image-offset'),
        ('forged-eligible-accounting','RESULT',0,{'eligible_free_frames':str(V.number(positive['accounting'],'free_frames')+1)},'physical free/eligible accounting'),
        ('search-returns-high-frame','SEARCH',1,{'found':'1','index':'65'},'physical bounded search result'),
        ('search-mutates-input','SEARCH',0,{'unchanged':'0'},'physical bounded search result'),
        ('one-past-stack-reversed','CPU',0,{'stack_top':'0x1234'},'physical AP stack extent'),
        ('stale-pointer-walker','POINTER',2,{'walked':'0x1000'},'physical pointer/walker contradiction'),
        ('unexpected-boundary-rejection','RESULT',0,{'rejected':'1'},'physical boundary violation'),
    ]:
        changed=mutate_record(kind,index,changes)
        reject(name,reason,lambda c=changed:V.fixture(c,topology,root,image,ram=ram),
            {'scope':'semantic record copy; actual production validator, no transport claim','kind':kind,'index':index,'changes':changes})
    diagnostic=evidence/series['diagnostics'][0];real=V.verify_run(diagnostic)
    with tempfile.TemporaryDirectory(prefix='physical-readback-negative-') as temporary:
        dest=Path(temporary)
        for name in ('physical-readback.json','table-snapshot.json','table-frames.bin','page-readback.stdout.log','qemu-serial.log'):
            shutil.copyfile(diagnostic/name,dest/name)
        proof=json.loads((dest/'physical-readback.json').read_text());old=copy.deepcopy(proof)
        raw=bytearray.fromhex(proof['retained']['g_font']);raw[:8]=bytes.fromhex(proof['originals']['font'])[:8]
        proof['retained']['g_font']=raw.hex()
        text=(dest/'page-readback.stdout.log').read_text()
        text=text.replace('PHYSICAL_STATE_GDB '+json.dumps(old,sort_keys=True,separators=(',',':')),
                          'PHYSICAL_STATE_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))
        (dest/'page-readback.stdout.log').write_text(text);(dest/'physical-readback.json').write_text(json.dumps(proof))
        reject('nested-font-pointer-not-adapted','physical nested glyph/header conversion missing',
            lambda:V.readback(dest,json.loads((diagnostic/'result.json').read_text()),real['fixtures']),
            {'scope':'synthetic debugger bytes and outer stdout consistently changed','base_sha256':E.sha(diagnostic/'physical-readback.json')})
    symbols=(evidence/'candidates/production/symbols.txt').read_text();strings=(evidence/'candidates/production/compiled-strings.txt').read_text()
    V.production(symbols,strings)
    reject('production-enforcement-removed','physical production enforcement compiled out',
        lambda:V.production(re.sub(r'^.*\bphysical_pointer_range$','',symbols,flags=re.M),strings),{'remove':'physical_pointer_range'})
    reject('production-test-leak','physical production fixture payload present',
        lambda:V.production(symbols+'\n00000001 T physical_fixture_run\n',strings),{'append':'physical_fixture_run'})
    reference=evidence.parent.parent/'before/paging-baselines'
    if not reference.is_dir():reference=evidence.parent.parent/'preservation/paging-baselines'
    baseline=json.loads((reference/'acknowledged-invalidation/baseline.json').read_text())['baseline']
    V.preserved_baseline_summary('acknowledged-invalidation',baseline)
    baseline=copy.deepcopy(baseline);baseline['q35-tcg-smp4']['median_ns']+=1
    reject('preserved-baseline-altered','preserved paging baseline changed',
        lambda:V.preserved_baseline_summary('acknowledged-invalidation',baseline),
        {'scope':'synthetic copy of original reviewed timing receipt','change':'increment TCG median by one nanosecond'})
    for name,path,old,new,subject in COMPILED:
        directory=output/name;directory.mkdir()
        with tempfile.TemporaryDirectory(prefix='physical-compiled-negative-') as temp:
            scratch=Path(temp)
            for prefix in ('kernel','shared'):
                shutil.copytree(ROOT/prefix,scratch/prefix,ignore=shutil.ignore_patterns('*.o','*.d','*.su'))
            (scratch/'scripts').mkdir()
            host={'boundary':'physical-translation-host.c','boot':'physical-boot-host.c','pmm':'physical-pmm-host.c'}[subject]
            shutil.copyfile(ROOT/'scripts'/host,scratch/'scripts'/host)
            changed=scratch/path;original=changed.read_text();E.require(old in original,'physical compiled mutation anchor missing: '+name)
            changed.write_text(original.replace(old,new,1))
            sources={'boundary':['kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/utils/bitmap.c'],
                'boot':['kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/memory/physical_translation.c'],
                'pmm':['kernel/src/utils/bitmap.c']}[subject]
            extra={'boundary':[],'boot':['-mcmodel=large','-Wl,--defsym,_kernel_start=0xffffffff82000000,--defsym,_kernel_end=0xffffffff82400000'],
                   'pmm':['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0']}[subject]
            if subject=='boot' and os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
                import stack_current
                sources+=stack_current.hosted_inverse_sources()
                extra+=['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0']
            argv=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',*extra,'scripts/'+host,*sources,'-o','fixture']
            compiled=subprocess.run(argv,cwd=scratch,text=True,capture_output=True)
            (directory/'compile.log').write_text(compiled.stdout+compiled.stderr)
            E.require(compiled.returncode==0,'physical negative did not compile: '+name)
            def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
            executed=subprocess.run(['./fixture'],cwd=scratch,text=True,capture_output=True,preexec_fn=no_core)
            output_text=executed.stdout+executed.stderr;(directory/'execute.log').write_text(output_text)
            E.require(executed.returncode!=0 and ('Assertion' in output_text or 'check failed' in output_text),
                      'physical actual C mutation not rejected: '+name)
            cases.append({'name':name,'synthetic':True,'expected_boundary':'actual C independent oracle',
                'actual_reason':'actual C independent oracle: '+output_text.strip(),'expected_exit':executed.returncode,
                'actual_exit':executed.returncode,'recipe':{'path':path,'old':old,'new':new,'count':1,
                    'base_sha256':E.sha(ROOT/path),'result_sha256':E.sha(changed),'compile':argv},
                'compile_sha256':E.sha(directory/'compile.log'),'execute_sha256':E.sha(directory/'execute.log')})
    with tempfile.TemporaryDirectory(prefix='physical-conversion-negative-') as temporary:
        scratch=Path(temporary)
        for prefix in ('kernel','bootloader','shared'):
            shutil.copytree(ROOT/prefix,scratch/prefix,ignore=shutil.ignore_patterns('*.o','*.d','*.su'))
        (scratch/'scripts').mkdir()
        shutil.copyfile(ROOT/'scripts/physical-conversion-sites.json',scratch/'scripts/physical-conversion-sites.json')
        owned=os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks')
        if owned:
            for name in (('device-conversion-sites.json','direct-map-conversion-sites.json','memory-source-inventory.py')+
                (('stack-guard-conversion-sites.json',) if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks' else ('kernel-image-conversion-sites.json',) if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='kernel-image-protection' else ('low-address-conversion-sites.json',) if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='low-address-isolation' else ())):
                shutil.copyfile(ROOT/'scripts'/name,scratch/'scripts'/name)
        inventory=module('physical-source-inventory');inventory.verify(scratch)
        changed=scratch/'kernel/src/memory/physical_translation.c'
        changed.write_text(changed.read_text()+'\nvoid *synthetic_raw_conversion(unsigned long physical) { return (void *)physical; }\n')
        reject('unknown-raw-conversion-site','unknown or altered '+('device' if owned else 'raw')+' conversion site',lambda:inventory.verify(scratch),
            {'scope':'actual complete source scanner on isolated synthetic source copy','path':'kernel/src/memory/physical_translation.c'})
    review=selector_module()
    E.require(review.requirement_source(evidence)==ROOT/'scripts/memory-current-requirements.json',
              'selected original requirement inventory positive')
    E.require(review.reference_path('0'*1024) is None and
              review.reference_path('candidates/selftest/candidate.json')==Path('candidates/selftest/candidate.json'),
              'observation data was interpreted as a dependency path')
    review.audit_admission(True,None,None)
    with tempfile.TemporaryDirectory(prefix='physical-selection-negative-') as temporary:
        scratch=Path(temporary)
        E.write(scratch/'regression-ledger.json',{'inventory_sha256':'0'*64})
        reject('selected-inventory-forged','selected requirement inventory differs from collected ledger',
            lambda:review.requirement_source(scratch),{'scope':'actual selector predicate; synthetic ledger digest'})
    reject('selection-audit-archive','selection audit cannot create a delivery archive',
        lambda:review.audit_admission(True,Path('synthetic.zip'),Path('synthetic-extraction')),
        {'scope':'actual selection-only admission predicate; no archive or extraction created'})
    transport=module('review-transport-fixtures').run(output/'transport')
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
            'harness_sha256':E.identity(E.manifest(harness=True)),'base_serial_sha256':E.sha(base/'qemu-serial.log'),
            'positive':['actual-captured-pointer-and-ceiling','independent-debugger-nested-objects','production-enforcement',
                        'original-requirement-inventory','nonpath-observation-excluded','selection-only-admission'],
            'cases':cases,'transport':transport}
    verify_receipt(result);E.write(output/'results.json',result);return result

def verify_receipt(value):
    names={'wrong-physical-root','image-offset-on-heap','at-exclusive-ceiling','contiguous-over-ceiling',
        'forged-eligible-accounting','search-returns-high-frame','search-mutates-input','one-past-stack-reversed',
        'stale-pointer-walker','unexpected-boundary-rejection','nested-font-pointer-not-adapted',
        'production-enforcement-removed','production-test-leak','unknown-raw-conversion-site',
        'preserved-baseline-altered','selected-inventory-forged','selection-audit-archive'}|{r[0] for r in COMPILED}
    E.require(value['status']=='PASS' and len(value['cases'])==len(names) and {r['name'] for r in value['cases']}==names,
              'physical fixture coverage omitted/duplicated')
    for row in value['cases']:
        E.require(row['synthetic'] and row['expected_exit']==row['actual_exit']!=0 and row['expected_boundary'] in row['actual_reason'],
                  'physical fixture unrelated rejecting boundary')
    module('review-transport-fixtures').verify(value['transport'])

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();print(json.dumps(run(args.evidence,args.output),sort_keys=True))
