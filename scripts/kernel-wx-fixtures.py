#!/usr/bin/env python3
"""Immutable-capture permission contradictions reaching the real current predicates."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import zlib
import memory_evidence as E
import kernel_image_evidence as V
import device_mapping_evidence as D
import stack_current as STACK

CASES={
 'linked-text-writable':'image effective section/alias permission',
 'direct-text-writable':'image effective section/alias permission',
 'third-writable-image-alias':'image unexpected third alias or unknown virtual region',
 'linked-data-executable':'image effective section/alias permission',
 'direct-data-executable':'image effective section/alias permission',
 'managed-data-executable':'image non-image data RWX/cache policy',
 'text-ancestor-nx':'image effective section/alias permission',
 'data-ancestor-readonly':'image effective section/alias permission',
 'original-trampoline-writable':'image effective section/alias permission',
 'original-trampoline-executable':'image effective section/alias permission',
 'boot-text-executable':'image effective section/alias permission',
 'wrong-protected-physical-frame':'image/direct physical/cache relationship',
 'low-middle-entry':'image raw main PML4 lower half is nonzero',
 'missing-physical-table-chunk':'image independent table bytes/binding missing',
 'mixed-table-candidate':'image independent table bytes/binding missing',
 'missing-physical-access-mode':'image independent table bytes/binding missing',
 'missing-rodata-plan':'image unknown/missing allocated ELF section',
 'missing-fixup-plan':'image unknown/missing allocated ELF section',
 'image-boundary-widened':'image linker/section extent differs',
 'unknown-allocated-section':'image unknown/missing allocated ELF section',
 'one-cpu-wp-clear':'image local CPU controls/identity/completion',
 'one-cpu-missing':'image missing/duplicate CPU result',
 'one-target-missing':'image all-CPU fault coverage',
 'fault-nonpresent':'image exact protection fault/target/CPU/root/generation',
 'write-reported-as-fetch':'image exact protection fault/target/CPU/root/generation',
 'null-reported-as-protection':'image exact protection fault/target/CPU/root/generation',
 'wrong-target-rip':'image exact protection fault/target/CPU/root/generation',
 'wrong-target-cr2':'image exact protection fault/target/CPU/root/generation',
 'wrong-main-root':'image exact protection fault/target/CPU/root/generation',
 'wrong-local-cpu':'image exact protection fault/target/CPU/root/generation',
 'wrong-local-apic':'image exact protection fault/target/CPU/root/generation',
 'stale-fault-generation':'image exact protection fault/target/CPU/root/generation',
 'inactive-fault-collection':'image exact protection fault/target/CPU/root/generation',
 'unexpected-access-success':'image exact protection fault/target/CPU/root/generation',
 'same-cpu-relabeled-results':'image duplicate/missing attempt',
 'ack-before-disarm':'image local CPU controls/identity/completion',
 'missing-cpu-ack':'image service missing target/ACK/completion',
 'hidden-probe-timeout':'image service missing target/ACK/completion',
 'nx-enabled-unsupported':'image local CPU controls/identity/completion',
 'live-scratch-rwx':'image non-image data RWX/cache policy',
 'reported-rights-disagree':'image actual scan summary contradicts leaves',
 'nx-off-reserved-bit':'image reserved NX in unsupported profile',
 'nx-off-zero-rwx-claim':'image actual scan summary contradicts leaves',
 'missing-terminal-record':'image missing/contradictory terminal checksum',
 'production-probe-symbol':'image production test payload isolation',
 'production-probe-command':'production command is present or did not return unknown',
 'fetch-nonpresent':'image exact protection fault/target/CPU/root/generation',
 'service-before-final-facts':'image service receipt precedes final disarmed CPU facts',
 'probe-notifications-unbounded':'image service missing target/ACK/completion',
 'probe-notifications-without-send':'image protection notification/audit binding',
 'probe-audit-notification-mismatch':'image protection notification/audit binding',
 'probe-audit-foreign-target':'image protection notification/audit binding',
 'probe-audit-generation-missing':'image protection service audit coverage',
 'live-topology-missing':'image online CPU topology missing or duplicated',
 'live-topology-duplicated':'image online CPU topology missing or duplicated',
 'retained-entry-table-relabel':'image retained entry-table identity or lifetime',
 'unclassified-unreachable-table':'image independent table inventory includes unreachable storage',
 'fixed-data-target-bytes':'image fixed target/helper bytes',
 'fixed-store-opcode':'image fixed target/helper bytes',
 'fixed-fetch-recovery-opcode':'image fixed target/helper bytes',
 'fixed-target-symbol-size':'image fixed probe symbol size/section',
 'fixed-target-wrong-section':'image fixed probe symbol size/section',
 'fixed-helper-missing-chunk':'ELF required fragment coverage',
 'fixed-probe-candidate-binding':'image bounded ELF/candidate/role binding',
 'legacy-layout-missing-current-boundary':'image required current linker symbol',
}
if STACK.enabled():
    # The same ancestor also controls the real linked BSP payload. Its exact
    # required write right is checked before the whole image leaf inventory.
    CASES['data-ancestor-readonly']='stack independent payload/backing/effective rights'

def copy_table_dependencies(source,destination):
    names=['symbols.txt','elf.txt','qemu-serial.log','result.json','table-snapshot.json','table-frames.bin']
    if STACK.enabled():
        proof=json.loads((source/'stack-owner-readback.json').read_text())
        names+=['stack-owner-readback.json','capture-config.json','stack-layout.stdout.log','stack-capture.py',
            'stack-descriptors.log','vm-identity.jsonl']+['stack-owner-'+name+'.bin' for name in proof['objects']]
    for name in names:shutil.copyfile(source/name,destination/name)

def synthetic_tables(directory,metadata,data):
    """Rebind labeled synthetic envelopes, never modify an original capture."""
    (directory/'table-frames.bin').write_bytes(data)
    metadata=copy.deepcopy(metadata);metadata['frames_sha256']=E.sha(directory/'table-frames.bin')
    (directory/'table-snapshot.json').write_bytes(E.canonical(metadata))
    if STACK.enabled():
        path=directory/'stack-owner-readback.json';owner=json.loads(path.read_text())
        owner['frames_sha256']=metadata['frames_sha256'];path.write_bytes(E.canonical(owner))
    return metadata

def verify_receipt(row):
    E.require(row['schema']==1 and row['status']=='PASS' and
        len(row['cases'])==len(CASES) and {r['name'] for r in row['cases']}==set(CASES),'image fixture coverage')
    for r in row['cases']:
        E.require(r['synthetic'] and r['actual_exit']==1 and r['expected_boundary']==CASES[r['name']] and
            r['expected_boundary'] in r['actual_reason'],'image fixture reached unrelated boundary')
    E.require(row['restored_positives']==['physical-main-root','linked-section-plan','all-cpu-faults',
        'live-scratch-rights','nx-absent-write-protection','production-codegen','production-command-absence',
        'migration-service-binding','topology-derived-live-scans','fixed-ELF-targets'],
        'image fixture restored positive coverage')

def parse(raw):
    out=[]
    for line in raw.splitlines():
        body=line.rsplit(b' crc=',1)[0];m=re.match(rb'^\[IMAGE_PROTECTION\]\[(\w+)\] (.*)$',body)
        E.require(m is not None,'image fixture immutable record');out.append((m[1].decode(),V.fields(m[2].decode())))
    return out

def encode(rows):
    """Rebind only labeled synthetic CRC/length/FNV metadata to reach semantics."""
    out=b'';digest=14695981039346656037
    for index,(kind,r) in enumerate(rows):
        row=dict(r)
        if kind=='END':row.update(records=str(index),bytes=str(len(out)),digest=str(digest))
        body=('[IMAGE_PROTECTION]['+kind+'] '+' '.join(k+'='+v for k,v in row.items())).encode()
        line=body+f' crc={zlib.crc32(body):08x}\n'.encode();out+=line
        if kind!='END':
            for byte in line:digest=((digest^byte)*1099511628211)&((1<<64)-1)
    return out

def inputs(path):
    lines=(path/'qemu-serial.log').read_text().splitlines();active=V.RAM.activation(lines);plan=V.elf_plan(path)
    split=D.split_traces((path/'qemu-debugcon.log').read_bytes())
    ram=V.RAM.trace(split['RAM_MAP'][0],lines,active,image_plan=plan)
    topology={V.LOW.d(r,'slot'):V.LOW.d(r,'apic') for r in V.LOW.records(lines,'[CPU_STATE][READY] ')}
    return plan,active,ram['coverage'],topology,split['IMAGE_PROTECTION'][0]

def service_and_live_fixtures(directory,reject):
    """Exercise current predicates on labeled copies of an actual complete boot."""
    plan,active,coverage,topology,_=inputs(directory)
    T=V.LOW.module('verify-tlb-shootdown');audits=[T.rows(b.decode('ascii').splitlines()) for b in T.trace_blocks(directory)]
    raw=D.split_traces((directory/'qemu-debugcon.log').read_bytes())['IMAGE_PROTECTION']
    proofs=[V.trace(b,plan,active,coverage,topology) for b in raw]
    V.service_binding(proofs,audits,active['root'],topology)
    # A migration between pinned sends can make every target remote. This is
    # a hosted semantic fixture, not a second hardware execution.
    changed=copy.deepcopy(audits);observed=copy.deepcopy(proofs)
    first=next(r for data in changed for r in data['SERVICE'] if V.n(r,'operation')==4)
    first['sent']=str(sum(1<<slot for slot in topology));first['notifications']=str(len(topology))
    observed[0]['service']['notifications']=str(len(topology))
    V.service_binding(observed,changed,active['root'],topology)
    for name in ('probe-notifications-without-send','probe-audit-notification-mismatch',
                 'probe-audit-foreign-target','probe-audit-generation-missing'):
        bad=copy.deepcopy(changed);row=next(r for data in bad for r in data['SERVICE'] if V.n(r,'operation')==4)
        if name=='probe-notifications-without-send':row['sent']=str(V.n(row,'sent')&~1)
        elif name=='probe-audit-notification-mismatch':row['notifications']=str(len(topology)-1)
        elif name=='probe-audit-foreign-target':row['sent']=str(V.n(row,'sent')|(1<<len(topology)))
        else:next(data['SERVICE'] for data in bad if row in data['SERVICE']).remove(row)
        reject(name,lambda:V.service_binding(observed,bad,active['root'],topology),
            {'base':E.sha(directory/'qemu-debugcon.log'),'mutation':name,'scope':'HOSTED_SYNTHETIC_AUDIT'})
        V.service_binding(observed,changed,active['root'],topology)
    with tempfile.TemporaryDirectory(prefix='image-live-topology-') as temporary:
        work=Path(temporary)
        for name in ('symbols.txt','elf.txt','qemu-serial.log'):shutil.copyfile(directory/name,work/name)
        expected=V.live_scans(directory,{'coverage':coverage},audits)
        for suffix in ('nx-disabled','xapic','hub-keyboard','high-ram','high-ram-fallback'):
            (work/'result.json').write_bytes(E.canonical({'synthetic':True,'profile':f'q35-tcg-smp{len(topology)}-'+suffix}))
            E.require(V.live_scans(work,{'coverage':coverage},audits)==expected,'image profile label changed live topology')
        serial=(work/'qemu-serial.log').read_text();ready=next(l for l in serial.splitlines() if l.startswith('[CPU_STATE][READY] '))
        for name in ('live-topology-missing','live-topology-duplicated'):
            changed_serial='\n'.join(l for l in serial.splitlines() if not l.startswith('[CPU_STATE][READY] '))+'\n' if name.endswith('missing') else serial+ready+'\n'
            (work/'qemu-serial.log').write_text(changed_serial)
            reject(name,lambda:V.live_scans(work,{'coverage':coverage},audits),
                {'base':E.sha(directory/'qemu-serial.log'),'mutation':name,'scope':'HOSTED_SYNTHETIC_TOPOLOGY'})
            (work/'qemu-serial.log').write_text(serial);E.require(V.live_scans(work,{'coverage':coverage},audits)==expected,'image live positive changed')

def fixed_elf_fixtures(candidate,reject):
    import memory_elf_proof as ELF
    proof=json.loads((candidate/'image-elf-proof.json').read_text());elf=ELF.replay(proof)
    ranges=dict(zip(V.PROBE_BYTES,V.fixed_probe_ranges(elf)));base=E.identity(proof)
    def patch(changed,offset,value):
        row=next(r for r in changed['chunks'] if r['offset']<=offset and offset+len(value)<=r['offset']+r['size'])
        data=bytearray.fromhex(row['hex']);start=offset-row['offset'];data[start:start+len(value)]=value
        row['hex']=data.hex();row['sha256']=__import__('hashlib').sha256(data).hexdigest()
    for name in ('fixed-data-target-bytes','fixed-store-opcode','fixed-fetch-recovery-opcode',
                 'fixed-target-symbol-size','fixed-target-wrong-section','fixed-helper-missing-chunk',
                 'legacy-layout-missing-current-boundary'):
        changed=copy.deepcopy(proof)
        if name in ('fixed-data-target-bytes','fixed-store-opcode','fixed-fetch-recovery-opcode'):
            symbol='protection_data_target' if name=='fixed-data-target-bytes' else 'protection_probe_store' if name=='fixed-store-opcode' else 'protection_probe_fetch'
            offset=ranges[symbol][0]+(2 if name=='fixed-store-opcode' else 13 if name=='fixed-fetch-recovery-opcode' else 0)
            patch(changed,offset,b'\x90')
        elif name=='fixed-target-symbol-size':patch(changed,elf.symbol('protection_data_target')['entry_offset']+16,struct.pack('<Q',7))
        elif name=='fixed-target-wrong-section':patch(changed,elf.symbol('protection_data_target')['entry_offset']+6,struct.pack('<H',elf.section('.text')['index']))
        elif name=='legacy-layout-missing-current-boundary':patch(changed,elf.symbol('_data_end')['string_file_offset'],b'X')
        else:
            offset=ranges['protection_probe_store'][0]
            changed['chunks'].remove(next(r for r in changed['chunks'] if r['offset']<=offset<r['offset']+r['size']))
        reject(name,lambda:ELF.replay(changed),{'base':base,'mutation':name,'outer_metadata':'SYNTHETIC_FRAGMENT_HASH_REBOUND'})
        ELF.replay(proof)
    gate=V.LOW.module('kernel-wx-gate')
    with tempfile.TemporaryDirectory(prefix='image-elf-binding-') as temp:
        work=Path(temp)
        for name in ('symbols.txt','elf.txt','tlb-disassembly.txt','candidate.json','image-elf-proof.json'):shutil.copyfile(candidate/name,work/name)
        gate.codegen(work,True);changed=copy.deepcopy(proof);changed['elf_sha256']='0'*64
        (work/'image-elf-proof.json').write_bytes(E.canonical(changed))
        reject('fixed-probe-candidate-binding',lambda:gate.codegen(work,True),{'base':base,'mutation':'unbound candidate ELF digest'})
        (work/'image-elf-proof.json').write_bytes(E.canonical(proof));gate.codegen(work,True)

def run(evidence,output,diagnostic=None,nx_run=None):
    output.mkdir(parents=True,exist_ok=False);cases=[]
    if diagnostic is None:diagnostic=evidence/json.loads((evidence/'direct-owner-readback.json').read_text())['path']
    if nx_run is None:nx_run=evidence/'nx-disabled'/json.loads((evidence/'nx-disabled/series.json').read_text())['runs'][0]
    plan,active,coverage,topology,raw=inputs(diagnostic)
    E.require(active['nx'] and len(topology)>1,'image fixture needs actual multiprocessor NX base')
    V.physical_tables(diagnostic,active,coverage);positive=V.trace(raw,plan,active,coverage,topology)
    originals={p:(E.sha(p),p.stat().st_size,p.stat().st_mode&0o777) for base in (diagnostic,nx_run) for p in base.rglob('*') if p.is_file()}
    def reject(name,action,recipe):
        try:action()
        except (ValueError,KeyError,AssertionError) as error:
            reason=str(error);E.require(CASES[name] in reason,'image unrelated rejection '+name+': '+reason)
        else:raise ValueError('image contradiction accepted: '+name)
        cases.append({'name':name,'synthetic':True,'actual_exit':1,'expected_boundary':CASES[name],
            'actual_reason':reason,'recipe':recipe})
    with tempfile.TemporaryDirectory(prefix='image-permission-negative-') as temporary:
        work=Path(temporary)/'table';work.mkdir()
        copy_table_dependencies(diagnostic,work)
        data=(work/'table-frames.bin').read_bytes();meta=json.loads((work/'table-snapshot.json').read_text())
        offsets={pa:i*4096 for i,pa in enumerate(meta['frame_addresses'])};mask=((1<<active['physical_bits'])-1)&~4095
        def path(va):
            table=active['root'];result=[]
            for level in (4,3,2,1):
                at=offsets[table]+((va>>(12+9*(level-1)))&511)*8;entry=struct.unpack_from('<Q',data,at)[0]
                E.require(entry&1,'image negative unmapped target');result.append((at,entry))
                if level==1 or level in (2,3) and entry&128:return result
                table=entry&mask
            raise ValueError('image negative table depth')
        names=plan['symbols'];text=names['protection_text_target'];mutable=names['protection_data_target'];tramp=names['_trampoline_start']
        targets={'linked-text-writable':text,'direct-text-writable':V.BASE+text-V.IMAGE,
            'linked-data-executable':mutable,'direct-data-executable':V.BASE+mutable-V.IMAGE,
            'text-ancestor-nx':text,'data-ancestor-readonly':mutable,
            'original-trampoline-writable':tramp,'original-trampoline-executable':tramp,
            'boot-text-executable':V.BASE+plan['boot_first'],'wrong-protected-physical-frame':text}
        # The managed base is reserved; locate an actual mapped owned leaf.
        def first_managed(pa,level,va):
            for index in range(512):
                at=offsets[pa]+index*8;e=struct.unpack_from('<Q',data,at)[0]
                if not e&1:continue
                v=va|(index<<(12+9*(level-1)))
                if level==4 and index>=256:v|=0xffff000000000000
                if level==1:
                    if V.WINDOW<=v<V.WINDOW+0x40000000:return v
                elif not(level in (2,3) and e&128):
                    found=first_managed(e&mask,level-1,v)
                    if found is not None:return found
            return None
        managed=first_managed(active['root'],4,0);E.require(managed is not None,'image managed negative base absent')
        targets.update({'third-writable-image-alias':managed,'managed-data-executable':managed})
        for name in list(CASES)[:16]:
            changed=bytearray(data);m=copy.deepcopy(meta)
            def write(at,value):struct.pack_into('<Q',changed,at,value)
            if name in targets:
                chain=path(targets[name]);at,old=chain[-1]
                if name.endswith('writable'):write(at,old|2)
                elif name=='third-writable-image-alias':write(at,(old&~mask)|(text-V.IMAGE)&~4095|2)
                elif name=='text-ancestor-nx':write(chain[0][0],chain[0][1]|V.NX)
                elif name=='data-ancestor-readonly':write(chain[0][0],chain[0][1]&~2)
                elif name=='wrong-protected-physical-frame':write(at,old^4096)
                else:
                    for pos,value in chain:write(pos,value&~V.NX)
            elif name=='low-middle-entry':write(offsets[active['root']]+127*8,2)
            elif name=='missing-physical-table-chunk':changed=changed[:-4096]
            elif name=='mixed-table-candidate':m['candidate_id']='0'*64
            else:m['memory_access']='UNVERIFIED_LINEAR_READ'
            m=synthetic_tables(work,m,changed)
            reject(name,lambda:V.physical_tables(work,active,coverage),{'base_sha256':meta['frames_sha256'],
                'mutation':name,'synthetic_sha256':m['frames_sha256'],'outer_metadata':'SYNTHETIC_HASH_REBOUND'})
            synthetic_tables(work,meta,data);V.physical_tables(work,active,coverage)
        for name in ('retained-entry-table-relabel','unclassified-unreachable-table'):
            m=copy.deepcopy(meta);changed=data
            if name=='retained-entry-table-relabel':m['static_frames']['boot_pd3']+=4096
            else:
                extra=meta['static_frames']['boot_pd3']+4096;E.require(extra not in offsets,'image synthetic extra frame collides')
                pages={pa:data[at:at+4096] for pa,at in offsets.items()};pages[extra]=bytes(4096)
                m['frame_addresses']=sorted(pages);changed=b''.join(pages[pa] for pa in m['frame_addresses'])
            m=synthetic_tables(work,m,changed)
            reject(name,lambda:V.physical_tables(work,active,coverage),{'base_sha256':meta['frames_sha256'],'mutation':name,'outer_metadata':'SYNTHETIC_HASH_REBOUND'})
            synthetic_tables(work,meta,data);V.physical_tables(work,active,coverage)
        header=(work/'elf.txt').read_text();symbols=(work/'symbols.txt').read_text()
        for name in ('missing-rodata-plan','missing-fixup-plan','image-boundary-widened','unknown-allocated-section'):
            if name=='image-boundary-widened':
                address=names['_data_start'];(work/'symbols.txt').write_text(symbols.replace(f'{address:016x} D _data_start',f'{address-1:016x} D _data_start'))
            else:
                section='.rodata' if name=='missing-rodata-plan' else '.cpu_msr_fixup' if name=='missing-fixup-plan' else '.text'
                (work/'elf.txt').write_text(header.replace(section+' ','.synthetic_unknown ',1))
            reject(name,lambda:V.elf_plan(work),{'elf_facts_sha256':plan['elf_facts_sha256'],'mutation':name})
            (work/'symbols.txt').write_text(symbols);(work/'elf.txt').write_text(header);E.require(V.elf_plan(work)['sections']==plan['sections'],'image section positive changed')
        rows=parse(raw)
        mutations={
            'one-cpu-wp-clear':('CPU','cr0',str(0x80000033)),'fault-nonpresent':('FAULT','error','2'),
            'write-reported-as-fetch':('FAULT','error','17'),'null-reported-as-protection':('FAULT','cr2','0'),
            'wrong-target-rip':('FAULT','rip','0'),'wrong-target-cr2':('FAULT','cr2','4096'),
            'wrong-main-root':('FAULT','root','4096'),'wrong-local-cpu':('FAULT','slot','1'),
            'wrong-local-apic':('FAULT','apic','255'),'stale-fault-generation':('FAULT','generation','0'),
            'inactive-fault-collection':('FAULT','collection','0'),'unexpected-access-success':('FAULT','completed','0'),
            'ack-before-disarm':('CPU','disarmed','0'),'missing-cpu-ack':('SERVICE','acknowledged','1'),
            'hidden-probe-timeout':('SERVICE','status','1'),'nx-enabled-unsupported':('CPU','unsupported','48'),
            'reported-rights-disagree':('SCAN','image_bytes','0'),
            'probe-notifications-unbounded':('SERVICE','notifications',str(len(topology)+1))}
        trace_names=list(CASES)[20:39]+['reported-rights-disagree','missing-terminal-record',
            'fetch-nonpresent','service-before-final-facts','probe-notifications-unbounded']
        for name in trace_names:
            changed=copy.deepcopy(rows)
            if name in mutations:
                kind,key,value=mutations[name];next(r for k,r in changed if k==kind)[key]=value
            elif name in ('one-cpu-missing','one-target-missing'):
                kind='CPU' if name=='one-cpu-missing' else 'FAULT';changed.pop(next(i for i,(k,_) in enumerate(changed) if k==kind))
            elif name=='same-cpu-relabeled-results':
                faults=[r for k,r in changed if k=='FAULT'];faults[6].update(faults[0])
            elif name=='fetch-nonpresent':next(r for k,r in changed if k=='FAULT' and V.n(r,'kind')==2)['error']='16'
            elif name=='service-before-final-facts':
                item=changed.pop(next(i for i,(k,_) in enumerate(changed) if k=='SERVICE'))
                changed.insert(next(i for i,(k,_) in enumerate(changed) if k=='CPU'),item)
            else:changed=changed[:-1]
            synthetic=encode(changed)
            reject(name,lambda:V.trace(synthetic,plan,active,coverage,topology),{'base_sha256':positive['raw_sha256'],
                'mutation':name,'synthetic_sha256':__import__('hashlib').sha256(synthetic).hexdigest(),'outer_metadata':'SYNTHETIC_CRC_LENGTH_FNV_REBOUND'})
            E.require(V.trace(raw,plan,active,coverage,topology)==positive,'image probe positive changed')
        T=V.LOW.module('verify-tlb-shootdown');audits=[T.rows(b.decode('ascii').splitlines()) for b in T.trace_blocks(diagnostic)]
        live=copy.deepcopy(audits);row=next(r for r in live[0]['RIGHTS'] if V.n(r,'stage')==0 and V.n(r,'va')==V.SCRATCH[0]);row['nx']='0';row['flags']=str(V.n(row,'flags')&~V.NX)
        reject('live-scratch-rwx',lambda:V.live_scans(diagnostic,{'coverage':coverage},live),{'mutation':'effective NX removed from actual live scratch run','base':E.sha(diagnostic/'qemu-debugcon.log')})
        V.live_scans(diagnostic,{'coverage':coverage},audits)
        service_and_live_fixtures(diagnostic,reject)
        np,na,nc,nt,nraw=inputs(nx_run);E.require(not na['nx'],'image NX fallback base is enabled');nr=parse(nraw)
        for name in ('nx-off-reserved-bit','nx-off-zero-rwx-claim'):
            changed=copy.deepcopy(nr)
            if name=='nx-off-reserved-bit':r=next(r for k,r in changed if k=='LEAF');r['flags']=str(V.n(r,'flags')|V.NX)
            else:next(r for k,r in changed if k=='SCAN')['rwx_bytes']='0'
            synthetic=encode(changed);reject(name,lambda:V.trace(synthetic,np,na,nc,nt),{'base_sha256':__import__('hashlib').sha256(nraw).hexdigest(),'mutation':name,'outer_metadata':'SYNTHETIC_CRC_LENGTH_FNV_REBOUND'})
            V.trace(nraw,np,na,nc,nt)
        gate=V.LOW.module('kernel-wx-gate');production=Path(temporary)/'production';production.mkdir();origin=evidence/'candidates/production'
        for name in ('symbols.txt','elf.txt','tlb-disassembly.txt','candidate.json','image-elf-proof.json'):shutil.copyfile(origin/name,production/name)
        original=(production/'symbols.txt').read_bytes();gate.codegen(production,False)
        (production/'symbols.txt').write_bytes(original+b'\nffffffff82001000 T protection_probe_fixup\n')
        reject('production-probe-symbol',lambda:gate.codegen(production,False),{'base':E.sha(origin/'symbols.txt'),'symbol':'protection_probe_fixup'})
        (production/'symbols.txt').write_bytes(original);gate.codegen(production,False)
        fixed_elf_fixtures(evidence/'candidates/selftest',reject)
    run=evidence/json.loads((evidence/'production.json').read_text())['runs'][0]
    row=next(json.loads(l) for l in (run/'framed.jsonl').read_text().splitlines() if json.loads(l)['payload']=='wxtest')
    result=json.loads((run/'result.json').read_text());lines=[];ends=[];offset=0
    for line in (run/'qemu-serial.log').read_bytes().splitlines(keepends=True):offset+=len(line);lines.append(line.rstrip(b'\r\n').decode());ends.append(offset)
    observer=V.LOW.module('verify-memory-observation');profile={'name':result['profile'],'vm_id':result['vm_id']}
    action=lambda r:observer.production_absence_transaction(r,run,lines,ends,profile,result['candidate_elf'])
    action(row);reject('production-probe-command',lambda:action(dict(row,handler_status=0)),{'base':E.identity(row),'handler_status':0});action(row)
    E.require(all((E.sha(p),p.stat().st_size,p.stat().st_mode&0o777)==v for p,v in originals.items()),'image negatives modified original capture')
    result={'schema':1,'status':'PASS','cases':cases,'restored_positives':['physical-main-root','linked-section-plan','all-cpu-faults',
        'live-scratch-rights','nx-absent-write-protection','production-codegen','production-command-absence',
        'migration-service-binding','topology-derived-live-scans','fixed-ELF-targets']}
    verify_receipt(result);E.write(output/'results.json',result);return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();print(json.dumps(run(a.evidence,a.output),sort_keys=True))
