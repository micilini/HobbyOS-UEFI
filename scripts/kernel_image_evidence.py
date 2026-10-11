"""Current image permissions from linked sections, raw leaves and exact local faults.

Historical RAM/no-low captures use their original contracts. This module never
infers a fault from unchanged bytes or turns NX absence into execute protection.
"""
import binascii
import json
import hashlib
import re
import struct
from pathlib import Path
import memory_evidence as E
import kernel_image_current as CURRENT
import direct_map_evidence as RAM
import low_address_evidence as LOW
import stack_current as STACK

enabled=CURRENT.enabled
BASE=RAM.BASE
IMAGE=RAM.IMAGE
WINDOW=0xffffd00000000000
SCRATCH=(0xffffc000001ff000,0xffffc000401ff000)
NX=1<<63
ROLES=('text','trampoline','rodata','cpu_msr_fixup','data','bss','boot_text','boot_data')
MUTABLE={4,5,7}
n=RAM.number
fields=RAM.fields
single=RAM.single

def elf_plan(directory):
    directory=Path(directory);names=LOW.symbols((directory/'symbols.txt').read_text())
    header=(directory/'elf.txt').read_text();sections={}
    for match in re.finditer(r'^\s*\[\s*\d+\]\s+(\.\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+(.*?)\s+\d+\s+\d+\s+\d+\s*$',header,re.M):
        name,kind,address,offset,size,flags=match.groups()
        E.require(name not in sections,'image duplicate ELF section')
        sections[name]={'type':kind,'address':int(address,16),'offset':int(offset,16),'bytes':int(size,16),'flags':flags.strip()}
    loads=[]
    for m in re.finditer(r'^\s*LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+([RW E]+)\s+0x([0-9a-f]+)$',header,re.M):
        loads.append((*[int(m[i],16) for i in range(1,6)],m[6].replace(' ',''),int(m[7],16)))
    return dict(section_plan(names,sections,loads),symbols=names,
        symbols_sha256=E.sha(directory/'symbols.txt'),elf_facts_sha256=E.sha(directory/'elf.txt'))

def section_plan(names,sections,loads):
    E.require({name for name,s in sections.items() if 'A' in s['flags']}=={'.'+name for name in ROLES if name!='boot_data'},
        'image unknown/missing allocated ELF section')
    rows=[]
    for role,name in enumerate(ROLES):
        section=sections.get('.'+name)
        E.require(section is not None,'image required ELF section missing: '+name)
        first,end=names['_'+name+'_start'],names['_'+name+'_end']
        E.require(first==section['address'] and end-first==section['bytes'] and
            (first<end or role in (4,5)),'image linker/section extent differs: '+name)
        expected={'AX'} if role in (0,1,6) else {'WA'} if role in (4,5) else {''} if role==7 else {'A'}
        E.require(section['flags'] in expected,'image unexpected ELF section flags: '+name)
        if role<6:E.require(IMAGE<=first<=end<1<<64,'image linked domain');first-=IMAGE;end-=IMAGE
        rows.append({'role':role,'name':name,'first':first,'end':end,'page_first':first&~4095,'page_end':(end+4095)&~4095})
    E.require(names['_kernel_start']==names['_text_start'] and names['_kernel_end']==names['_bss_end'] and
        names['_phys_end_boot']==names['_boot_data_end'] and names['_boot_data_end']==rows[0]['first'],
        'image/boot reservation envelope differs')
    E.require(names['bsp_stack_top']-names['bsp_stack_bottom']==16384 and
        names['_bss_start']<=names['bsp_stack_bottom']<names['bsp_stack_top']<=names['_bss_end'],
        'image writable BSP stack backing')
    for i,row in enumerate(rows):
        E.require(row['first']<=row['end'] and (i in (0,6) or row['first']>=rows[i-1]['end']),
            'image section order/overlap')
        for other in rows[:i]:
            if row['first']==row['end'] or other['first']==other['end']:continue
            if row['page_first']<other['page_end'] and other['page_first']<row['page_end']:
                E.require((i in MUTABLE)==(other['role'] in MUTABLE) and (i==0)==(other['role']==0),
                    'image incompatible permission page sharing')
    envelopes=[]
    for offset,va,pa,filesize,size,flags,alignment in loads:
        E.require(filesize<=size and size>0 and alignment==4096 and
            (va==pa if va<IMAGE else va-pa==IMAGE),'image ELF LOAD physical relationship')
        covered=[r for r in rows if r['role']!=7 and pa<=r['first']<r['end']<=pa+size]
        E.require(covered and min(r['first'] for r in covered)==pa and max(r['end'] for r in covered)==pa+size,
            'image unclassified LOAD extent')
        E.require(flags==('RW' if any(r['role'] in MUTABLE for r in covered) else 'RE' if any(r['role'] in (0,1,6) for r in covered) else 'R'),
            'image LOAD writable code or role flags')
        envelopes.append((pa,pa+size))
    E.require(len(envelopes)==4 and all(sum(a<=r['first'] and r['end']<=b for a,b in envelopes)==1 for r in rows if r['role']!=7 and r['end']>r['first']),
        'image ELF load/section coverage')
    # The accepted .boot_data is explicit physical reservation, not an ELF LOAD.
    result={'sections':rows,'first':rows[0]['first'],'end':rows[5]['page_end'],
        'boot_first':rows[6]['first'],'boot_end':rows[7]['page_end']}
    if STACK.enabled():
        import stack_guard_evidence as SG
        result['stacks']=SG.linker_plan(names,rows)
    return result

PROBE_BYTES={
    'protection_probe_store':('.text',2,'31c0408837c3b801000000c3'),
    'protection_probe_fetch':('.text',2,'4883ec0831c0ffd74883c408c3b801000000c3'),
    'protection_text_target':('.text',1,'5a'*8),
    'protection_rodata_target':('.rodata',1,'a7'*8),
    'protection_data_target':('.data',1,'c3'+'00'*7),
}

def fixed_probe_ranges(elf):
    """File extents from actual symbol/section headers; no guessed VA offset."""
    names={r['name'] for r in elf.symbols()}
    sites={'protection_store_site':('protection_probe_store',2),
        'protection_store_recovery':('protection_probe_store',6),
        'protection_fetch_call':('protection_probe_fetch',6),
        'protection_fetch_recovery':('protection_probe_fetch',13)}
    required=set(PROBE_BYTES)|set(sites)|{'cmd_wxtest'}
    if not names&required:return []
    E.require(required<=names,'image fixed probe symbol inventory')
    ranges=[]
    for name,(section,kind,expected) in PROBE_BYTES.items():
        symbol=elf.symbol(name);owner=elf.section(section);size=len(bytes.fromhex(expected))
        E.require(symbol['size']==size and symbol['info']&15==kind and symbol['shndx']==owner['index'] and
            owner['addr']<=symbol['value'] and symbol['value']+size<=owner['addr']+owner['size'],
            'image fixed probe symbol size/section')
        ranges.append((owner['offset']+symbol['value']-owner['addr'],size))
    for name,(owner,offset) in sites.items():
        E.require(elf.symbol(name)['value']==elf.symbol(owner)['value']+offset and
            elf.symbol(name)['shndx']==elf.section('.text')['index'],'image fixed probe recovery offset')
    return ranges

def native_image_plan(elf):
    """Apply the same current linker contract to every native ELF build role."""
    required=['_'+role+suffix for role in ROLES for suffix in ('_start','_end')]+[
        '_kernel_start','_kernel_end','_phys_end_boot','bsp_stack_bottom','bsp_stack_top']
    if STACK.enabled(): required+=['bsp_stack_guard_start','bsp_stack_guard_end','g_ist1','g_ist2','g_ist3']
    E.require(set(required)<={r['name'] for r in elf.symbols()},'image required current linker symbol')
    names={name:elf.symbol(name)['value'] for name in required}
    sections={s['name']:{'address':s['addr'],'bytes':s['size'],
        'flags':''.join(letter for mask,letter in ((1,'W'),(2,'A'),(4,'X')) if s['flags']&mask)} for s in elf.sections}
    E.require(all(s['flags']&~7==0 for s in elf.sections if s['flags']&2 or s['name']=='.boot_data'),
        'image unknown native section flags')
    loads=[(p['offset'],p['vaddr'],p['paddr'],p['filesz'],p['memsz'],
        ''.join(letter for mask,letter in ((4,'R'),(2,'W'),(1,'E')) if p['flags']&mask),p['align'])
        for p in elf.programs if p['type']==1]
    return section_plan(names,sections,loads)

def verify_native_image(elf):
    plan=native_image_plan(elf)
    if STACK.enabled():
        import stack_guard_evidence as SG
        SG.verify_native(elf,plan)
    for (name,(_,_,expected)),(offset,size) in zip(PROBE_BYTES.items(),fixed_probe_ranges(elf)):
        E.require(elf.data[offset:offset+size].hex()==expected,'image fixed target/helper bytes: '+name)
    return plan

def rights(plan,physical,size,linked,nx):
    E.require(size>0 and 0<=physical<physical+size<=1<<48,'image physical extent overflow')
    end=physical+size
    image=plan['first']<=physical and end<=plan['end']
    E.require(not linked or image,'image alias exceeds its classified envelope')
    if STACK.enabled() and linked:
        a,b=(v-IMAGE for v in plan['stacks']['guard'])
        E.require(physical+size<=a or physical>=b,'stack BSP guard is mapped in the linked image')
    cuts={physical,end}
    for a,b in ((plan['first'],plan['end']),(plan['boot_first'],plan['boot_end'])):
        if physical<a<end:cuts.add(a)
        if physical<b<end:cuts.add(b)
    for row in plan['sections']:
        if row['first']==row['end']:continue
        for p in (row['page_first'],row['page_end']):
            if physical<p<end:cuts.add(p)
    expected=set()
    for p in sorted(cuts)[:-1]:
        selected=[r['role'] for r in plan['sections'] if r['first']<r['end'] and r['page_first']<=p<r['page_end']]
        protected=plan['first']<=p<plan['end'] or plan['boot_first']<=p<plan['boot_end']
        writable=any(role in MUTABLE for role in selected) if protected else True
        executable=linked and 0 in selected
        expected.add((writable,bool(nx and not executable)))
    E.require(len(expected)==1,'image leaf crosses a permission boundary')
    return expected.pop()

def overlaps(plan,pa,size):
    return any(pa<b and a<pa+size for a,b in ((plan['first'],plan['end']),(plan['boot_first'],plan['boot_end'])))

def leaf_check(plan,row,nx,coverage,owners=None):
    va,pa,size,shift,flags,write,noexec=(n(row,k) for k in ('va','physical','bytes','shift','flags','writable','nx'))
    E.require(shift in (12,21,30) and size>0 and (va|pa|size)% (1<<shift)==0 and va+size<=1<<64 and
        pa+size<=1<<48 and write in (0,1) and noexec in (0,1),'image leaf extent/rights')
    E.require(flags&1 and not flags&~(0x9b|NX|(4096 if shift>12 else 0)) and
        bool(flags&128)==(shift>12) if shift>12 else flags&1 and not flags&~(0x9b|NX),
        'image raw leaf unknown flags')
    E.require(nx or not(flags&NX or noexec),'image reserved NX in unsupported profile')
    direct=BASE<=va<BASE+RAM.SPAN
    linked=IMAGE+plan['first']<=va<IMAGE+plan['end']
    cache=(flags>>3)&3 | ((flags>>(12 if shift>12 else 7))&1)<<2
    if direct or linked:
        E.require(va==(BASE if direct else IMAGE)+pa and cache==0,'image/direct physical/cache relationship')
        E.require((bool(write),bool(noexec))==rights(plan,pa,size,linked,nx),'image effective section/alias permission')
        E.require(bool(flags&2)==bool(write),'image unexpected ancestor RO restriction')
        if overlaps(plan,pa,size):E.require(shift==12,'image/boot backing covered by a large leaf')
        if direct:E.require(RAM.contains(coverage,pa,size),'image scan admits excluded RAM/device range')
        return 'direct' if direct else 'image'
    if STACK.enabled():
        import stack_guard_evidence as SG
        if SG.BASE<=va<SG.END:
            E.require(owners is not None,'stack scan missing bound owner records')
            return SG.leaf(plan,row,nx,owners)
    managed=WINDOW<=va and va+size<=WINDOW+0x40000000
    scratch=any(first<=va and va+size<=first+65*4096 for first in SCRATCH)
    E.require((managed or scratch) and shift==12 and not overlaps(plan,pa,size),
        'image unexpected third alias or unknown virtual region')
    E.require(bool(noexec)==nx and (not scratch or write and cache==0) and (write or cache==0),
        'image non-image data RWX/cache policy')
    return 'managed' if managed else 'scratch'

def scan(rows,summary,plan,active,coverage,owner_rows=None,topology=None):
    totals={key:0 for key in ('bytes','leaves','image_bytes','direct_bytes','managed_bytes','scratch_bytes','rwx_bytes')}
    sets={'image':[],'direct':[]};last=0
    owners=None;image_intervals=[[plan['first'],plan['end']]]
    if STACK.enabled():
        import stack_guard_evidence as SG
        E.require(owner_rows is not None,'stack full-root scan has no owner inventory')
        owners=SG.owners(owner_rows,plan,coverage,topology);totals['stack_bytes']=0;sets['stack']=[]
        image_intervals=SG.linked_intervals(plan)
    for row in rows:
        role=leaf_check(plan,row,active['nx'],coverage,owners)
        va,pa,size,shift=(n(row,k) for k in ('va','physical','bytes','shift'))
        E.require(va>=last,'image overlapping or reordered virtual leaf runs');last=va+size
        totals['bytes']+=size;totals['leaves']+=size>>shift;totals[role+'_bytes']+=size
        if n(row,'writable') and not n(row,'nx'):totals['rwx_bytes']+=size
        if role in sets:sets[role].append((va,va+size) if role=='stack' else (pa,pa+size))
    if STACK.enabled(): SG.coverage_check(sets['stack'],owners)
    E.require(RAM.merge(sets['direct'])==coverage and RAM.merge(sets['image'])==image_intervals,
        'image full-root coverage missing tail or alias')
    E.require(n(summary,'root')==active['root'] and n(summary,'nx')==active['nx'] and n(summary,'complete')==1 and
        n(summary,'tables')>0 and all(n(summary,k)==v for k,v in totals.items()),'image actual scan summary contradicts leaves')
    E.require(not active['nx'] or totals['rwx_bytes']==0,'image effective RWX main-root mapping')
    return totals

def trace(raw,plan,active,coverage,topology):
    rows={};order=[];lines=raw.splitlines(keepends=True);digest=14695981039346656037
    E.require(0<len(raw)<=4*1024*1024 and raw.endswith(b'\n'),'image trace bound/framing')
    for index,line in enumerate(lines):
        body,sep,crc=line.rstrip(b'\n').rpartition(b' crc=')
        E.require(sep and re.fullmatch(b'[0-9a-f]{8}',crc) and binascii.crc32(body)==int(crc,16),'image record CRC')
        match=re.match(rb'^\[IMAGE_PROTECTION\]\[(\w+)\] ',body)
        E.require(match is not None,'image record namespace')
        kind=match[1].decode();row=fields(body[match.end():].decode())
        rows.setdefault(kind,[]).append(row);order.append(kind)
        if index==0:E.require(kind=='BEGIN','image trace missing begin')
        if index==len(lines)-1:
            E.require(kind=='END' and n(row,'records')==index and n(row,'bytes')==len(raw)-len(line) and
                n(row,'digest')==digest and row['status']=='PASS','image missing/contradictory terminal checksum')
        else:
            E.require(kind!='END','image early terminal record')
            for byte in line:digest=((digest^byte)*1099511628211)&((1<<64)-1)
    E.require(set(rows)==set('BEGIN LEAF SECTION CPU FAULT SERVICE SCAN END'.split()) |
        ({'STACK'} if STACK.enabled() else set()) | (set() if active['nx'] else {'UNSUPPORTED'}),
        'image probe raw record inventory')
    begin=single(rows,'BEGIN');end=single(rows,'END');service=single(rows,'SERVICE')
    E.require(order.index('SERVICE')>max(i for i,k in enumerate(order) if k in ('CPU','FAULT','UNSUPPORTED')),
        'image service receipt precedes final disarmed CPU facts')
    collection=n(begin,'collection');generation=n(service,'generation');targets=sum(1<<slot for slot in topology)
    E.require(begin['schema']=='1' and collection>0 and generation>0 and n(begin,'root')==active['root'] and
        n(begin,'nx')==active['nx'] and n(end,'collection')==n(service,'collection')==collection,
        'image collection/root/capability binding')
    E.require(n(service,'requested')==n(service,'acknowledged')==targets and
        all(n(service,k)==0 for k in ('missing','failed','status')) and
        0<=n(service,'notifications')<=len(topology) and (len(topology)>1 or n(service,'notifications')==0),
        'image service missing target/ACK/completion')
    E.require([(n(r,'role'),n(r,'first'),n(r,'end')) for r in rows['SECTION']]==
        [(r['role'],r['first'],r['end']) for r in plan['sections']],'image raw plan differs from linked sections')
    total=scan(rows['LEAF'],single(rows,'SCAN'),plan,active,coverage,rows.get('STACK'),topology)
    E.require(len(rows['CPU'])==len(topology) and {n(r,'slot') for r in rows['CPU']}==set(topology),'image missing/duplicate CPU result')
    for r in rows['CPU']:
        slot=n(r,'slot')
        E.require(n(r,'collection')==collection and n(r,'generation')==generation and n(r,'apic')==topology[slot] and
            n(r,'cr0')==0x80010033 and n(r,'cr3')==active['root'] and n(r,'cr4')==0x20 and
            n(r,'efer')==(0xd00 if active['nx'] else 0x500) and bool(n(r,'extended_edx')&(1<<20))==active['nx'] and
            n(r,'completed')==(63 if active['nx'] else 15) and n(r,'unsupported')==(0 if active['nx'] else 48) and
            n(r,'disarmed')==1,'image local CPU controls/identity/completion')
    seen=set();names=plan['symbols'];target_names=('protection_text_target','protection_rodata_target','protection_data_target')
    for r in rows['FAULT']:
        slot,attempt=n(r,'slot'),n(r,'attempt');key=slot,attempt
        E.require(slot in topology and attempt< (6 if active['nx'] else 4) and key not in seen,'image duplicate/missing attempt');seen.add(key)
        kind,alias=divmod(attempt,2);linked=names[target_names[kind]];pa=linked-IMAGE;va=BASE+pa if alias else linked
        rip=va if kind==2 else names['protection_store_site'];recovery=names['protection_fetch_recovery' if kind==2 else 'protection_store_recovery']
        original=(0x5a,0xa7,0xc3)[kind]
        expected={'collection':collection,'generation':generation,'slot':slot,'apic':topology[slot],
            'attempt':attempt,'kind':kind,'alias':alias,'root':active['root'],'physical':pa,'target':va,'rip':rip,
            'cr2':va,'error':17 if kind==2 else 3,'cs':8,'recovery':recovery,'before':original,'after':original,'completed':1}
        E.require(set(r)==set(expected) and all(n(r,k)==v for k,v in expected.items()),'image exact protection fault/target/CPU/root/generation')
        section=plan['sections'][(0,2,4)[kind]]
        E.require(section['first']<=pa and pa+8<=section['end'],'image fixed target is in wrong section')
    E.require(seen=={(s,a) for s in topology for a in range(6 if active['nx'] else 4)},'image all-CPU fault coverage')
    unsupported=[]
    for row in rows.get('UNSUPPORTED',[]):
        E.require(n(row,'collection')==collection and n(row,'generation')==generation and row['reason']=='UNAVAILABLE_NX' and
            n(row,'executed')==0,'image unsupported execution was attempted or unbound')
        unsupported.append((n(row,'slot'),n(row,'attempt')))
    E.require(sorted(unsupported)==([(s,a) for s in sorted(topology) for a in (4,5)] if not active['nx'] else []),'image NX unsupported exact coverage')
    return {'status':'PASS','collection':collection,'generation':generation,'cpus':len(topology),
        'faults':len(seen),'unsupported':len(unsupported),'scan':total,'service':service,
        'write_protection':'ENFORCED','execute_protection':'ENFORCED' if active['nx'] else 'UNAVAILABLE_NX',
        'strict_wx':'ENFORCED' if active['nx'] else 'UNAVAILABLE','raw_sha256':hashlib.sha256(raw).hexdigest()}

def boot(directory,active):
    lines=(directory/'qemu-serial.log').read_text().splitlines()
    pre=LOW.single(lines,'[IMAGE_PROTECTION] PRELOAD ')
    E.require(pre['verified']=='1' and LOW.h(pre,'cr0')==0x80010033 and LOW.h(pre,'efer')==(0xd00 if active['nx'] else 0x500) and
        pre['sections']=='8' and pre['write']=='ENFORCED' and pre['execute']==('ENFORCED' if active['nx'] else 'UNAVAILABLE_NX'),
        'image preactivation enforcement/control proof')
    scans=LOW.records(lines,'[IMAGE_PROTECTION] SCAN ')
    E.require([r['stage'] for r in scans[:2]]==['ACTIVATION','DEVICES'],'image boot readiness scan coverage')
    for r in scans:
        E.require(LOW.h(r,'root')==active['root'] and n(r,'nx')==active['nx'] and r['status']=='PASS' and
            (not active['nx'] or n(r,'rwx_bytes')==0),'image boot/live scan RWX or root mismatch')
    E.require(lines.index(next(l for l in lines if l.startswith('[IMAGE_PROTECTION] PRELOAD '))) <
        lines.index(next(l for l in lines if l.startswith('[RAM_MAP] ACTIVE '))),'image permission validation follows activation')
    return {'status':'PASS','scans':len(scans),'nx':active['nx']}

def ready_topology(lines):
    rows=LOW.records(lines,'[CPU_STATE][READY] ')
    topology={LOW.d(r,'slot'):LOW.d(r,'apic') for r in rows}
    E.require(0<len(rows)<=32 and len(rows)==len(topology)==len(set(topology.values())) and
        set(topology)==set(range(len(rows))),'image online CPU topology missing or duplicated')
    return topology

def service_binding(proofs,audits,root,topology):
    """Migration may change the local sender; bind IPIs to the actual sent mask."""
    services=[r for data in audits for r in data['SERVICE'] if n(r,'operation')==4]
    E.require(len(services)==len(proofs),'image protection service audit coverage')
    targets=sum(1<<slot for slot in topology)
    for proof,row in zip(proofs,services):
        observed=proof['service'];sent=n(row,'sent')
        E.require(n(row,'generation')==proof['generation'] and n(row,'mapping')==0 and
            n(row,'root')==root and n(row,'targets')==targets and
            all(n(row,key)==0 for key in ('first','last','pages','strategy','send_failed')) and
            all(n(row,a)==n(observed,b) for a,b in (('targets','requested'),('ack','acknowledged'),
                ('missing','missing'),('failed','failed'),('status','status'),('notifications','notifications'))) and
            sent&~targets==0 and n(row,'notifications')==sent.bit_count() and
            (len(topology)>1 or sent==0) and 0<=n(row,'elapsed_ns')<1000000000 and
            row['identities']==','.join(f'{slot}:{topology[slot]}' for slot in sorted(topology)),
            'image protection notification/audit binding')

def verify_directory(directory,ram,*,complete=True):
    import device_mapping_evidence as D
    directory=Path(directory);lines=(directory/'qemu-serial.log').read_text().splitlines();active=RAM.activation(lines)
    plan=elf_plan(directory);state=boot(directory,active)
    topology=ready_topology(lines)
    blocks=D.split_traces((directory/'qemu-debugcon.log').read_bytes())['IMAGE_PROTECTION']
    E.require(len(blocks)==2 if complete else len(blocks)>0,'image protection command coverage')
    proofs=[trace(raw,plan,active,ram['coverage'],topology) for raw in blocks]
    T=LOW.module('verify-tlb-shootdown')
    service_binding(proofs,[T.rows(b.decode('ascii').splitlines()) for b in T.trace_blocks(directory)],active['root'],topology)
    E.require([p['collection'] for p in proofs]==list(range(1,len(proofs)+1)) and
        [p['generation'] for p in proofs]==sorted(set(p['generation'] for p in proofs)),'image repeated/stale collection')
    framed=[json.loads(l) for l in (directory/'framed.jsonl').read_text().splitlines()]
    selected=[(i,r) for i,r in enumerate(framed) if r['payload']=='wxtest']
    E.require(len(selected)==len(proofs) and all(r['handler_status']==0 for _,r in selected),'image command did not return success')
    return {'status':'PASS','boot':state,'collections':proofs,'faults':sum(p['faults'] for p in proofs),
        'unsupported':sum(p['unsupported'] for p in proofs),'section_plan_sha256':E.identity(plan['sections'])}

def live_scans(directory,ram,audits):
    lines=(directory/'qemu-serial.log').read_text().splitlines()
    plan=elf_plan(directory);active=RAM.activation(lines);results=[];count=len(ready_topology(lines))
    for invocation,data in enumerate(audits,1):
        stages=[0,1,2] if count>1 else [0,2]
        E.require([n(r,'stage') for r in data['RIGHTSSCAN']]==stages,'image live/terminal table scan missing')
        E.require(all(n(r,'invocation')==invocation for r in data['RIGHTS']+data['RIGHTSSCAN']),'image live scan invocation')
        E.require({n(r,'stage') for r in data['RIGHTS']}==set(stages),'image extra/missing live leaf scan')
        for summary in data['RIGHTSSCAN']:
            stage=n(summary,'stage');rows=[r for r in data['RIGHTS'] if n(r,'stage')==stage]
            owners=[r for r in data.get('STACK',[]) if n(r,'stage')==stage] if STACK.enabled() else None
            result=scan(rows,summary,plan,active,ram['coverage'],owners,ready_topology(lines))
            E.require(result['scratch_bytes']==(0 if stage==2 else (stage+1)*65*4096),'image scratch proof not captured while live')
            results.append({'invocation':invocation,'stage':stage,**result})
    return results

def physical_tables(directory,active,coverage,*,production=False):
    directory=Path(directory);meta=json.loads((directory/('production-memory.json' if production else 'table-snapshot.json')).read_text());run=json.loads((directory/'result.json').read_text())
    path=directory/('production-table-frames.bin' if production else 'table-frames.bin')
    data=path.read_bytes();addresses=meta['frame_addresses'];plan=elf_plan(directory)
    E.require(meta['candidate_id']==run['candidate_id'] and meta['vm_id']==run['vm_id'] and meta['active_root']==meta['cr3']==active['root'] and
        meta['memory_access']=='QEMU_PHYSICAL_PACKET_VERIFIED' and meta['frames_sha256']==E.sha(path) and
        len(data)==4096*len(addresses) and addresses==sorted(set(addresses)),'image independent table bytes/binding missing')
    frames={pa:data[i*4096:(i+1)*4096] for i,pa in enumerate(addresses)};levels={};ancestry=set();rows=[];mask=((1<<active['physical_bits'])-1)&~4095
    def walk(pa,level,va,write,noexec):
        E.require(pa in frames and pa not in ancestry and (pa not in levels or levels[pa]==level),'image independent link/cycle/level')
        levels[pa]=level;ancestry.add(pa)
        for index,(entry,) in enumerate(struct.iter_unpack('<Q',frames[pa])):
            if level==4 and index<256:E.require(entry==0,'image raw main PML4 lower half is nonzero')
            if not entry:continue
            E.require(entry&1 and not entry&4 and not(entry&~(mask|0xe3|0x18|NX)) and (active['nx'] or not entry&NX),
                'image independent entry format/supervisor/width/NX')
            first=va|(index<<(12+9*(level-1)))
            if level==4 and index>=256:first|=0xffff000000000000
            w=write and bool(entry&2);x=noexec or bool(entry&NX)
            leaf=level==1 or level in (2,3) and entry&128
            E.require(level!=4 or not entry&128,'image independent invalid root PS')
            if leaf:
                shift=12+9*(level-1);size=1<<shift
                E.require(level!=3 or any(n(c,'extended_edx')&(1<<26) for c in LOW.records((directory/'qemu-serial.log').read_text().splitlines(),'[CPU_STATE][READY] ')),
                    'image independent unsupported one-GiB leaf')
                E.require(not(entry&(size-1)&~(4095 if level==1 else 8191)),'image independent misaligned large leaf')
                physical=entry&mask&~(size-1);flags=entry&~(mask|0x60)
                if level>1:flags|=entry&4096
                rows.append({k:str(v) for k,v in {'va':first,'physical':physical,'bytes':size,'shift':shift,'flags':flags,'writable':int(w),'nx':int(x)}.items()})
            else:
                E.require(not entry&0xfd8,'image independent unknown table-link flag')
                walk(entry&mask,level-1,first,w,x)
        ancestry.remove(pa)
    walk(active['root'],4,0,True,False)
    retained=set()
    if not production:
        from memory_table_proof import STATIC_NAMES
        expected={name:plan['symbols'][name] for name in STATIC_NAMES}
        retained=set(expected.values())
        boot=plan['sections'][7]
        E.require(meta['static_frames']==expected and len(retained)==7 and
            all(pa%4096==0 and boot['first']<=pa and pa+4096<=boot['end'] for pa in retained) and
            not retained&set(levels),'image retained entry-table identity or lifetime')
    E.require(set(levels)|retained==set(addresses),'image independent table inventory includes unreachable storage')
    stack_proof=None;owner_rows=None;inventory=None;topology=ready_topology((directory/'qemu-serial.log').read_text().splitlines())
    if STACK.enabled():
        import stack_guard_evidence as SG
        stack_proof=SG.readback(directory,plan,coverage,active,topology,frames,production=production)
        owner_rows=stack_proof['owner_rows'];inventory=stack_proof['inventory']
    totals={k:0 for k in ('bytes','leaves','image_bytes','direct_bytes','managed_bytes','scratch_bytes','rwx_bytes')}
    if STACK.enabled():totals['stack_bytes']=0
    for row in rows:
        role=leaf_check(plan,row,active['nx'],coverage,inventory);size=n(row,'bytes');totals['bytes']+=size;totals['leaves']+=1;totals[role+'_bytes']+=size
        if n(row,'writable') and not n(row,'nx'):totals['rwx_bytes']+=size
    summary={k:str(v) for k,v in dict(totals,root=active['root'],nx=int(active['nx']),complete=1,tables=len(levels)).items()}
    checked=scan(rows,summary,plan,active,coverage,owner_rows,topology)
    return {'status':'PASS','physical_tables':len(levels),'snapshot_sha256':meta['frames_sha256'],**checked,
        **({'stack_owners':{k:v for k,v in stack_proof.items() if k not in ('owner_rows','inventory')}} if stack_proof else {}),
        'image_plan_sha256':E.identity(plan['sections']),'effective_framebuffer_type':'NOT_COMPUTED'}
