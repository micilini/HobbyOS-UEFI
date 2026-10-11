#!/usr/bin/env python3
"""Synthetic contradictions against the actual direct-map semantic predicates."""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zlib
import memory_evidence as E
import direct_map_evidence as R
import device_mapping_evidence as M

if not R.LOW.enabled():os.environ['HOBBYOS_VALIDATION_CONTRACT']=R.CONTRACT
CASES={
 'missing-ram-tail':'RAM missing/extra interval coverage',
 'reserved-page-admitted':'RAM extra Reserved/device/unreported page',
 'framebuffer-wb-alias':'RAM extra Reserved/device/unreported page',
 'unsupported-one-gib':'RAM one-GiB leaf lacks actual capability',
 'cache-boundary-straddled':'RAM large leaf crosses cache/provenance boundary',
 'large-low-prefix':'RAM prohibited prefix/image large leaf',
 'large-image-alias':'RAM prohibited prefix/image large leaf',
 'wrong-pat-ps':'RAM PS/PAT/NX/permission',
 'missing-nx':'RAM PS/PAT/NX/permission',
 'nx-without-support':'RAM PS/PAT/NX/permission',
 'old-bitmap-member':'RAM bitmap owner/member old epoch',
 'old-table-pointer':'RAM table pointer or raw address capability',
 'old-nested-pointer':'RAM nested boot pointer stale',
 'image-copy-rebased':'RAM image-owned boot copy was rebased',
 'arbitrary-high-pointer':'RAM heap actual access/domain',
 'missing-terminal':'RAM trace terminal boundary',
 'copied-builder-count':'RAM leaf coverage/count/ceiling reconciliation',
 'high-physical-used-as-cpu':'RAM real high frame/table access or retirement',
 'high-release-missing':'RAM real high frame/table access or retirement',
 'high-table-not-accessible':'RAM real high frame/table access or retirement',
 'undefined-mtrr-overlap':'RAM undefined MTRR overlap',
 'wc-mtrr-without-capability':'RAM malformed MTRR range',
 'activation-before-verification':'RAM activation ordering or incomplete transition',
 'bootstrap-root-truncation':'RAM active root/domain/epoch',
 'ap-incompatible-width':'RAM participating CPU cannot load declared page format',
 'ap-incompatible-one-gib':'RAM participating CPU cannot load declared page format',
 'physical-table-wrong-page':'RAM snapshot wrong physical leaf',
 'physical-table-missing-tail':'RAM independent physical tables differ from full descriptor coverage',
 'physical-table-cycle':'RAM snapshot link/cycle',
 'physical-table-nx-omitted':'RAM snapshot cache/NX/alignment',
 'physical-table-source-substitution':'RAM physical snapshot missing or source/root binding',
 'owner-window-old-pointer':'RAM owner table pointer old epoch',
 'owner-stack-physical':'RAM owner stale pointer/extent',
 'owner-bitmap-old-pointer':'RAM owner bitmap/member mismatch',
 'owner-bootstrap-root-truncated':'RAM physical bootstrap root or CPU pointer',
 'owner-gdt-physical-tss':'RAM GDT TSS CPU pointer',
 'owner-other-vm':'RAM owner snapshot identity/source',
}

if R.LOW.enabled():CASES['owner-bootstrap-root-truncated']='retired bootstrap observer pointer remains live'
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
    CASES['owner-stack-physical']='RAM owner guarded stack CPU/IST extent mismatch'
    CASES['owner-obsolete-build-layout']='RAM owner layout differs'

def module(name):
    spec=importlib.util.spec_from_file_location('ram_negative_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def decode(raw):
    result=[]
    for line in raw.decode().splitlines():
        body=line.rsplit(' crc=',1)[0];prefix,fields=body.split(' ',1)
        result.append((prefix,R.fields(fields)))
    return result

def seal(rows,lines):
    raw=b''
    for prefix,row in rows:
        body=(prefix+' '+' '.join(k+'='+v for k,v in row.items())).encode()
        raw+=body+f' crc={zlib.crc32(body):08x}\n'.encode()
    digest=14695981039346656037
    for byte in raw:digest=((digest^byte)*1099511628211)&((1<<64)-1)
    lines=list(lines);prefix='[RAM_MAP_TRACE][COMPLETE] '
    at=next(i for i,l in enumerate(lines) if l.startswith(prefix));receipt=R.fields(lines[at][len(prefix):])
    receipt.update(bytes=str(len(raw)),records=str(raw.count(b'\n')),fnv64=hex(digest))
    lines[at]=prefix+' '.join(k+'='+v for k,v in receipt.items())
    return raw,lines

def verify_receipt(record):
    names=[r['name'] for r in record['cases']]
    E.require(record['schema']==1 and record['status']=='PASS' and len(names)==len(set(names)) and
        set(names)==set(CASES),'RAM negative inventory incomplete/duplicate')
    E.require(record['positive_controls']==['ordinary-complete','high-one-gib-complete','high-fallback-complete',
        'physical-table-readback','pointer-owner-readback','receipt-roundtrip','restored-after-mutations'],'RAM negative restored positives absent')
    for row in record['cases']:
        E.require(row['synthetic'] and row['actual_exit']==1 and row['expected_boundary']==CASES[row['name']] and
            row['expected_boundary'] in row['actual_reason'],'RAM mutation unrelated rejection: '+row['name'])

def run(evidence,output):
    output.mkdir(parents=True,exist_ok=False);V=module('verify-device-mapping');cases=[]
    main=json.loads((evidence/'series.json').read_text())
    base=evidence/next(n for n in main['runs'] if n.startswith('profiles/q35-kvm-smp4-'))
    diagnostic=evidence/next(n for n in main['diagnostics'] if (evidence/n/'table-frames.bin').is_file())
    high=evidence/'high-ram';high=high/json.loads((high/'series.json').read_text())['runs'][0]
    fallback=evidence/'high-ram-fallback';fallback=fallback/json.loads((fallback/'series.json').read_text())['runs'][0]
    for path in (base,high,fallback,diagnostic):
        actual=V.verify_complete_run(path)
        E.require(actual==json.loads(E.canonical(actual)),
            'RAM semantic receipt changes after JSON serialization')
    def reject(name,action,recipe):
        try:action()
        except (ValueError,KeyError,AssertionError) as error:
            reason=str(error);E.require(CASES[name] in reason,'RAM wrong negative boundary '+name+': '+reason)
        else:raise ValueError('RAM contradiction accepted: '+name)
        cases.append({'name':name,'synthetic':True,'actual_exit':1,'expected_boundary':CASES[name],
            'actual_reason':reason,'recipe':recipe})
    def capture(path):
        lines=(path/'qemu-serial.log').read_text().splitlines()
        raw=M.split_traces((path/'qemu-debugcon.log').read_bytes())['RAM_MAP'][0]
        return raw,lines,R.activation(lines)
    raw,lines,active=capture(base);original=decode(raw)
    for name in list(CASES)[:22]:
        chosen=high if name in ('unsupported-one-gib','high-physical-used-as-cpu','high-release-missing','high-table-not-accessible') else base
        b,l,a=capture(chosen);rows=decode(b)
        def one(kind):return next(r for p,r in rows if p=='[RAM_MAP]['+kind+']')
        def leaf(predicate):return next(r for p,r in rows if p=='[RAM_MAP][LEAVES]' and predicate(r))
        def replace_leaf(r,pa,size,shift):r.update(physical=hex(pa),va=hex(R.BASE+pa),bytes=str(size),shift=str(shift),flags=hex(3|(128 if shift>12 else 0)|((1<<63) if a['nx'] else 0)))
        if name=='missing-ram-tail':
            row=[r for p,r in rows if p=='[RAM_MAP][LEAVES]'][-1]
            if R.number(row,'bytes')>1<<R.number(row,'shift'):row['bytes']=str(R.number(row,'bytes')-(1<<R.number(row,'shift')))
            else:rows.remove(('[RAM_MAP][LEAVES]',row))
        elif name in ('reserved-page-admitted','framebuffer-wb-alias'):
            pa=R.number(one('EXCLUSION'),'framebuffer_first') if name.startswith('framebuffer') else R.number(next(r for p,r in rows if p=='[RAM_MAP][DESCRIPTOR]' and r['type']=='0'),'first')
            row=leaf(lambda r:True);replace_leaf(row,pa,4096,12)
        elif name=='unsupported-one-gib':
            state=one('STATE');state['extended_edx']=hex(R.number(state,'extended_edx')&~(1<<26));state['page1g']='0'
        elif name=='cache-boundary-straddled':
            row=leaf(lambda r:R.number(r,'shift')==21 and R.number(r,'bytes')>=0x200000)
            boundary=R.number(row,'physical')+4096;variable=one('VARIABLE');bits=(1<<a['physical_bits'])-1
            variable.update(base=hex(boundary|6),mask=hex((bits&~4095)|0x800))
        elif name in ('undefined-mtrr-overlap','wc-mtrr-without-capability'):
            variables=[r for p,r in rows if p=='[RAM_MAP][VARIABLE]']
            E.require(len(variables)>=2,'RAM cache mutation requires two actual register slots')
            for variable in variables:variable['mask']='0x0'
            pa=R.number(leaf(lambda r:R.number(r,'shift')==21),'physical')
            mask=(((1<<a['physical_bits'])-1)&~0x1fffff)|0x800
            variables[0].update(base=hex(pa|6),mask=hex(mask))
            variables[1].update(base=hex(pa|1),mask=hex(mask))
            cache=one('MTRR');cap=R.number(cache,'capability')
            cache['capability']=hex(cap|0x400 if name=='undefined-mtrr-overlap' else cap&~0x400)
        elif name in ('large-low-prefix','large-image-alias'):
            pa=0 if name=='large-low-prefix' else R.number(one('EXCLUSION'),'image_first')&~0x1fffff
            if name=='large-image-alias' and os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
                plan=__import__('kernel_image_evidence').elf_plan(chosen)
                mutable=next(s for s in plan['sections'] if s['name']=='bss')
                pa=(mutable['page_first']+0x1fffff)&~0x1fffff
                E.require(pa+0x200000<=mutable['page_end'],'RAM image geometry fixture needs a complete mutable large-page span')
            replace_leaf(leaf(lambda r:True),pa,0x200000,21)
        elif name=='wrong-pat-ps':
            row=leaf(lambda r:R.number(r,'shift')==21);row['flags']=hex(R.number(row,'flags')^128)
        elif name=='missing-nx':
            row=leaf(lambda r:True);row['flags']=hex(R.number(row,'flags')&~(1<<63))
        elif name=='nx-without-support':
            a=dict(a,nx=False);state=one('STATE');state['nx']='0';state['extended_edx']=hex(R.number(state,'extended_edx')&~(1<<20))
        elif name=='old-bitmap-member':one('BITMAP')['member']=one('BITMAP')['physical']
        elif name=='old-table-pointer':one('STATE')['table_cpu']=one('STATE')['root']
        elif name=='old-nested-pointer':one('BOOT')['glyphs']=one('BOOT')['glyph_physical']
        elif name=='image-copy-rebased':one('BOOT')['view']=hex(R.BASE+0x200000)
        elif name=='arbitrary-high-pointer':one('HEAP')['cpu']=hex(R.BASE+R.SPAN+0x1000)
        elif name=='missing-terminal':rows=[r for r in rows if r[0]!='[RAM_MAP][END]']
        elif name=='copied-builder-count':one('STATE')['bytes']=str(R.number(one('STATE'),'bytes')+4096)
        elif name=='high-physical-used-as-cpu':one('HIGH')['cpu']=one('HIGH')['physical']
        elif name=='high-release-missing':one('HIGH')['released']='0'
        elif name=='high-table-not-accessible':one('HIGH')['table_cpu']=one('HIGH')['table']
        b,l=seal(rows,l)
        reject(name,lambda:R.trace(b,l,a,high=chosen==high,image_plan=__import__('kernel_image_evidence').elf_plan(chosen) if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks') else None),{'base':chosen.relative_to(evidence).as_posix(),
            'original_trace_sha256':E.sha(chosen/'qemu-debugcon.log'),'operation':name,
            'result_sha256':hashlib.sha256(b).hexdigest(),'outer_metadata':'Explicit synthetic CRC and FNV only'})
        R.trace(*capture(chosen)[:2],capture(chosen)[2],high=chosen==high,image_plan=__import__('kernel_image_evidence').elf_plan(chosen) if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks') else None)
    for name in ('activation-before-verification','bootstrap-root-truncation'):
        changed=list(lines)
        at=next(i for i,l in enumerate(changed) if l.startswith('[RAM_MAP] PRELOAD '))
        if name=='activation-before-verification':changed[at]=changed[at].replace('verified=1','verified=0')
        else:
            prefix='[RAM_MAP] PRELOAD ';row=R.fields(changed[at][len(prefix):]);row['root']='0000000100100000'
            changed[at]=prefix+' '.join(k+'='+v for k,v in row.items())
        reject(name,lambda:R.activation(changed),{'base':base.relative_to(evidence).as_posix(),'operation':name})
    hb,hl,ha=capture(high)
    for name in ('ap-incompatible-width','ap-incompatible-one-gib'):
        changed=list(hl);prefix='[CPU_CONTROL][CPU] '
        at=next(i for i,l in enumerate(changed) if l.startswith(prefix) and 'slot=1 ' in l);row=R.fields(changed[at][len(prefix):])
        if name.endswith('width'):row['physical_bits']='35';row['address_eax']=hex((R.number(row,'address_eax')&~255)|35)
        else:row['extended_edx']=hex(R.number(row,'extended_edx')&~(1<<26))
        changed[at]=prefix+' '.join(k+'='+v for k,v in row.items())
        reject(name,lambda:R.cpu_formats(changed,ha),{'base':high.relative_to(evidence).as_posix(),'operation':name})
    db,dl,da=capture(diagnostic);observed=R.trace(db,dl,da,image_plan=__import__('kernel_image_evidence').elf_plan(diagnostic) if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks') else None)
    with tempfile.TemporaryDirectory(prefix='ram-table-negative-',dir=os.environ.get('HOBBYOS_REVIEW_SCRATCH')) as temp:
        scratch=Path(temp)
        for name in ('table-snapshot.json','table-frames.bin','result.json'):shutil.copyfile(diagnostic/name,scratch/name)
        source=(scratch/'table-frames.bin').read_bytes();meta=json.loads((scratch/'table-snapshot.json').read_text());addresses=meta['frame_addresses']
        offsets={p:i*4096 for i,p in enumerate(addresses)}
        table=da['root'];slot=offsets[table]+((R.BASE>>39)&511)*8
        root_link=struct.unpack_from('<Q',source,slot)[0]
        table=root_link&0x000ffffffffff000
        leaf_slot=None
        def find(pa,level):
            nonlocal leaf_slot
            for i in range(512):
                off=offsets[pa]+8*i;e=struct.unpack_from('<Q',source,off)[0]
                if not e&1:continue
                if level==1 or e&128:leaf_slot=off;return
                find(e&0x000ffffffffff000,level-1)
                if leaf_slot is not None:return
        find(table,3);E.require(leaf_slot is not None,'RAM fixture actual leaf missing')
        for name in [n for n in CASES if n.startswith('physical-table-')]:
            data=bytearray(source);m=copy.deepcopy(meta);off=leaf_slot;old=struct.unpack_from('<Q',data,off)[0]
            if name=='physical-table-wrong-page':struct.pack_into('<Q',data,off,old^4096)
            elif name=='physical-table-missing-tail':struct.pack_into('<Q',data,off,0)
            elif name=='physical-table-cycle':struct.pack_into('<Q',data,slot,(root_link&~0x000ffffffffff000)|da['root'])
            elif name=='physical-table-nx-omitted':
                # Remove NX from every reachable direct-map entry so inherited NX cannot mask the mutation.
                pending=[(da['root'],4)];seen=set()
                while pending:
                    pa,level=pending.pop()
                    if pa in seen:continue
                    seen.add(pa)
                    for i in range(512):
                        if level==4 and not ((R.BASE>>39)&511)<=i<=(((R.BASE+R.SPAN-1)>>39)&511):continue
                        pos=offsets[pa]+8*i;e=struct.unpack_from('<Q',data,pos)[0]
                        if not e&1:continue
                        struct.pack_into('<Q',data,pos,e&~(1<<63))
                        if level>1 and not(level in (2,3) and e&128):pending.append((e&0x000ffffffffff000,level-1))
            else:m['candidate_id']='0'*64
            (scratch/'table-frames.bin').write_bytes(data);m['frames_sha256']=E.sha(scratch/'table-frames.bin')
            (scratch/'table-snapshot.json').write_bytes(E.canonical(m))
            reject(name,lambda:R.physical_tables(scratch,observed,da),{'base':diagnostic.relative_to(evidence).as_posix(),
                'operation':name,'original_sha256':hashlib.sha256(source).hexdigest(),
                'synthetic_sha256':m['frames_sha256'],'outer_metadata':'Synthetic snapshot hash recomputed'})
        (scratch/'table-frames.bin').write_bytes(source);(scratch/'table-snapshot.json').write_bytes(E.canonical(meta))
        R.physical_tables(scratch,observed,da)
    selected=json.loads((evidence/'direct-owner-readback.json').read_text())
    owner=evidence/selected['path'];R.owner_readback(owner)
    with tempfile.TemporaryDirectory(prefix='ram-owner-negative-',dir=os.environ.get('HOBBYOS_REVIEW_SCRATCH')) as temp:
        scratch=Path(temp)
        names=['result.json','table-snapshot.json','capture-config.json','qemu-serial.log','qemu-debugcon.log','ram-capture.py','ram-layout.stdout.log',
            'ram-owner-readback.json']+[p.name for p in owner.glob('ram-owner-*.bin')]
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):names+=['symbols.txt','elf.txt']
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            stack_proof=json.loads((owner/'stack-owner-readback.json').read_text())
            names+=['table-frames.bin','stack-owner-readback.json','stack-layout.stdout.log',
                'stack-capture.py','stack-descriptors.log','vm-identity.jsonl']+[
                'stack-owner-'+name+'.bin' for name in stack_proof['objects']]
        for name in names:shutil.copyfile(owner/name,scratch/name)
        originals={name:(scratch/name).read_bytes() for name in names}
        for name in [n for n in CASES if n.startswith('owner-')]:
            proof=json.loads(originals['ram-owner-readback.json'])
            def change(symbol,offset,value):
                path=scratch/('ram-owner-'+symbol+'.bin');data=bytearray(path.read_bytes())
                struct.pack_into('<Q',data,offset,value);path.write_bytes(data);proof['objects'][symbol]['sha256']=E.sha(path)
            if name=='owner-window-old-pointer':
                symbol='g_owned_window';raw=(scratch/('ram-owner-'+symbol+'.bin')).read_bytes()
                pa=struct.unpack_from('<Q',raw,8)[0];change(symbol,0,pa)
                change('g_boot_build',proof['layout']['build']['window'],pa)
            elif name=='owner-stack-physical':
                child=proof['children'][0];child['stack_top']-=R.BASE
                change('g_cpus',child['slot']*48+24,child['stack_top'])
            elif name=='owner-bitmap-old-pointer':
                raw=(scratch/'ram-owner-g_bitmap.bin').read_bytes();change('g_bitmap',0,struct.unpack_from('<Q',raw)[0]-R.BASE)
            elif name=='owner-bootstrap-root-truncated':
                proof['bootstrap_root']+=2**32;proof['bootstrap_pointer']+=2**32
            elif name=='owner-gdt-physical-tss':
                child=proof['children'][0];data=bytearray.fromhex(child['gdt_hex']);struct.pack_into('<I',data,48,0)
                child['gdt_hex']=data.hex()
            elif name=='owner-obsolete-build-layout':
                # Rebind every outer layout copy so the independent current
                # expectation, not disagreement between copies, rejects it.
                old=dict(proof['layout'])
                old['build']=dict(old['build'],count=270856,verified=270865,activated=270866,size=270872)
                proof['layout']=old
                config=json.loads(originals['capture-config.json']);config['ram_layout']=old
                (scratch/'capture-config.json').write_bytes(E.canonical(config))
                (scratch/'ram-layout.stdout.log').write_bytes(E.canonical(old))
            else:proof['vm_id']='synthetic-different-vm'
            (scratch/'ram-owner-readback.json').write_bytes(E.canonical(proof))
            reject(name,lambda:R.owner_readback(scratch),{'base':selected['path'],'operation':name,
                'original_sha256':E.sha(owner/'ram-owner-readback.json'),
                'synthetic_sha256':E.sha(scratch/'ram-owner-readback.json'),'outer_metadata':'Synthetic raw object hashes recomputed'})
            for filename,data in originals.items():(scratch/filename).write_bytes(data)
            R.owner_readback(scratch)
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'cases':cases,
        'positive_controls':['ordinary-complete','high-one-gib-complete','high-fallback-complete',
            'physical-table-readback','pointer-owner-readback','receipt-roundtrip','restored-after-mutations']}
    verify_receipt(result);E.write(output/'results.json',result);return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();print(json.dumps(run(a.evidence,a.output),sort_keys=True))
