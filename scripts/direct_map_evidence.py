"""Descriptor-derived RAM coverage and actual pointer/leaf predicates.

Historical identity captures retain their original validator contract. These
predicates require new raw activation and leaf records, never inferred offsets.
"""
import binascii
import json
import os
import re
import struct
from pathlib import Path
import memory_evidence as E
import low_current as LOW
import kernel_image_current as WX

BASE=0xffff800000000000
SPAN=0x400000000000
IMAGE=0xffffffff80000000
CONTRACT='verified-ram-direct-map'

def enabled(): return os.environ.get('HOBBYOS_VALIDATION_CONTRACT')==CONTRACT or LOW.enabled()
def number(row,key):
    value=row[key]
    E.require(re.fullmatch(r'(?:0x[0-9a-f]+|[0-9]+)',value),'RAM numeric field: '+key)
    return int(value,16 if value.startswith('0x') else 10)
def fields(text):
    row={}
    for item in text.split():
        key,sep,value=item.partition('=')
        E.require(sep and key not in row,'RAM duplicate/malformed field')
        row[key]=value
    return row
def single(rows,key):
    E.require(len(rows.get(key,[]))==1,'RAM missing/duplicate '+key)
    return rows[key][0]
def merge(intervals):
    out=[]
    for first,end in sorted(intervals):
        E.require(first<end and (not out or first>=out[-1][1]),'RAM overlap/empty interval')
        if out and first==out[-1][1]:out[-1]=[out[-1][0],end]
        else:out.append([first,end])
    return out
def contains(intervals,first,size):
    return size>0 and any(a<=first and size<=b-first for a,b in intervals)
def leaf_runs(leaves):
    result=[]
    for first,end,shift in sorted(leaves):
        if result and result[-1][1]==first and result[-1][2]==shift:
            result[-1]=[result[-1][0],end,shift]
        else:result.append([first,end,shift])
    return result
def activation(lines):
    result={}
    for kind in ('PRELOAD','ACTIVE'):
        prefix='[RAM_MAP] '+kind+' '
        rows=[(i,fields(l[len(prefix):])) for i,l in enumerate(lines) if l.startswith(prefix)]
        E.require(len(rows)==1,'RAM activation missing/duplicate '+kind)
        result[kind]=rows[0]
    pi,p=result['PRELOAD'];ai,a=result['ACTIVE']
    E.require(pi<ai and p['verified']=='1' and a['bitmap_rebased']=='1' and
        a['identity_retained']==('0' if LOW.enabled() else '1'),
        'RAM activation ordering or incomplete transition')
    root=int(p['root'],16)
    E.require(0<root<2**32 and root%4096==0 and int(a['root'],16)==root and
        int(a['base'],16)==BASE and int(a['span'],16)==SPAN and a['epoch']=='2',
        'RAM active root/domain/epoch')
    E.require(36<=number(p,'physical_bits')<=52 and p['nx'] in ('0','1') and
        a['nx']==('ENABLED' if p['nx']=='1' else 'NX_UNAVAILABLE'),'RAM NX capability/activation')
    ceiling=int(a['ceiling'],16)
    E.require(0<ceiling<=min(SPAN,2**number(p,'physical_bits')) and ceiling%4096==0,'RAM coverage ceiling')
    return {'root':root,'base':BASE,'span':SPAN,'epoch':2,'ceiling':ceiling,
            'nx':p['nx']=='1','physical_bits':number(p,'physical_bits'),
            'tables':number(p,'tables'),'ram_bytes':number(p,'ram_bytes'),
            'leaves':[number(p,'leaves_'+s) for s in ('4k','2m','1g')]}

def trace(raw,serial,active,*,high=False,image_plan=None):
    E.require(0<len(raw)<=1024*1024 and raw.endswith(b'\n'),'RAM trace bound/framing')
    rows={};kinds=[]
    for line in raw.decode('ascii').splitlines():
        body,sep,crc=line.rpartition(' crc=')
        E.require(sep and re.fullmatch('[0-9a-f]{8}',crc) and binascii.crc32(body.encode())==int(crc,16),'RAM record CRC')
        m=re.match(r'^\[RAM_MAP\]\[(\w+)\] ',body)
        E.require(m,'RAM record namespace')
        kinds.append(m[1]);rows.setdefault(m[1],[]).append(fields(body[m.end():]))
    E.require(kinds[0]=='BEGIN' and kinds[-1]=='END','RAM trace terminal boundary')
    E.require(set(rows)==set('BEGIN LEAVES STATE MTRR VARIABLE FIXED DESCRIPTOR EXCLUSION BITMAP BOOT HEAP END'.split()) |
        ({'HIGH'} if high else set()),'RAM raw coverage record set')
    begin=single(rows,'BEGIN');end=single(rows,'END');state=single(rows,'STATE')
    generation=number(begin,'generation')
    E.require(generation>0 and begin['schema']=='1' and number(begin,'high')==int(high) and
        number(end,'generation')==generation and end['status']=='PASS','RAM generation/result')
    prefix='[RAM_MAP_TRACE][COMPLETE] '
    receipts=[fields(l[len(prefix):]) for l in serial if l.startswith(prefix)]
    E.require(len(receipts)==1,'RAM raw/serial terminal binding')
    r=receipts[0];digest=14695981039346656037
    for byte in raw:digest=((digest^byte)*1099511628211)&((1<<64)-1)
    E.require(r['status']=='PASS' and r['channel']=='debugcon' and r['port']=='0x402' and
        number(r,'generation')==generation and number(r,'high')==int(high) and
        number(r,'bytes')==len(raw) and number(r,'records')==len(kinds) and number(r,'fnv64')==digest,
        'RAM raw/serial checksum or completion')
    for key in ('root','base','span','epoch','ceiling','physical_bits','nx','tables'):
        E.require(number(state,key)==active[key],'RAM activation/observation differs: '+key)
    E.require(number(state,'table_cpu')==BASE+active['root'] and
        number(state,'extended_max')>=0x80000008 and number(state,'address_eax')&255==active['physical_bits'],
        'RAM table pointer or raw address capability')
    edx=number(state,'extended_edx');large=bool(edx&(1<<26))
    E.require(edx&(1<<29) and bool(edx&(1<<20))==active['nx'] and number(state,'page1g')==large,
        'RAM raw page-format capability')
    exclude=single(rows,'EXCLUSION');fb=(number(exclude,'framebuffer_first'),number(exclude,'framebuffer_end'))
    image=(number(exclude,'image_first'),number(exclude,'image_end'))
    E.require(fb[0]<fb[1] and image[0]<image[1] and not any(x%4096 for x in (*fb,*image)) and
        (image[1]<=fb[0] or fb[1]<=image[0]),'RAM exclusion/image envelope')
    descriptors=rows['DESCRIPTOR'];expected=[];boundaries={0x200000,*image,*fb};previous=0
    E.require(0<len(descriptors)<=1024,'RAM descriptor count')
    for i,d in enumerate(descriptors):
        first,last=number(d,'first'),number(d,'end');kind=number(d,'type')
        E.require(number(d,'index')==i and previous<=first<last<=2**48 and not(first%4096 or last%4096),
            'RAM descriptor order/extent')
        previous=last;boundaries.update((first,last))
        if kind in (1,2,3,4,5,6,7,9,10):
            E.require(last<=min(SPAN,2**active['physical_bits']),'RAM unsupported actual extent')
            if first<fb[1] and fb[0]<last:
                if first<fb[0]:expected.append((first,fb[0]))
                if fb[1]<last:expected.append((fb[1],last))
            else:expected.append((first,last))
    cache=single(rows,'MTRR');defaults=number(cache,'defaults');width=(1<<active['physical_bits'])-1;maskbits=width&~4095
    variables=rows.get('VARIABLE',[]);typed_ranges=[]
    wc_supported=bool(number(cache,'capability')&0x400)
    def cache_type(kind):return kind in (0,1,4,5,6) and (kind!=1 or wc_supported)
    E.require(number(cache,'count')==len(variables)==number(cache,'capability')&255 and
        len(variables)<=256 and defaults&~0xcff==0 and cache_type(defaults&255), 'RAM MTRR input coverage')
    for i,v in enumerate(variables):
        mask=number(v,'mask');base=number(v,'base')
        E.require(number(v,'index')==i,'RAM MTRR input order')
        if not mask&0x800:continue
        size=((~mask)&maskbits)+4096;first=base&maskbits
        E.require(mask&~(maskbits|0x800)==0 and base&~(maskbits|255)==0 and cache_type(base&255) and
            size&(size-1)==0 and first%size==0 and first+size<=width+1 and mask&maskbits==(maskbits&~(size-1)),
            'RAM malformed MTRR range')
        if defaults&0x800:
            boundaries.update((first,first+size));typed_ranges.append((first,first+size,base&255))
    E.require(len(rows['FIXED'])==11 and [number(v,'index') for v in rows['FIXED']]==list(range(11)),'RAM fixed MTRR inputs')
    if defaults&0xc00==0xc00:
        E.require(number(cache,'capability')&0x100,'RAM fixed MTRR capability')
        E.require(all(cache_type((number(v,'value')>>(j*8))&255) for v in rows['FIXED'] for j in range(8)),
            'RAM malformed fixed MTRR type')
    for first,last in expected:
        cuts=sorted({first,last,*[p for p in boundaries|{0x100000} if first<p<last]})
        for start in cuts[:-1]:
            if defaults&0xc00==0xc00 and start<0x100000:continue
            types={kind for a,b,kind in typed_ranges if a<=start<b}
            E.require(len(types)<=1 or 0 in types or types=={4,6},'RAM undefined MTRR overlap')
    expected=merge(expected);actual=[];geometry=[];counts=[0,0,0]
    for leaf in rows['LEAVES']:
        va,pa,size,shift,flags=(number(leaf,k) for k in ('va','physical','bytes','shift','flags'))
        E.require(shift in (12,21,30) and size>0 and va==BASE+pa and (va|pa|size)% (1<<shift)==0,
            'RAM leaf alignment/domain')
        E.require(shift!=30 or large,'RAM one-GiB leaf lacks actual capability')
        expected_write=True
        if WX.enabled():
            import kernel_image_evidence as W
            E.require(image_plan is not None,'RAM current image permission plan missing')
            expected_write,_=W.rights(image_plan,pa,size,False,active['nx'])
        E.require(flags==1|(2 if expected_write else 0)|(128 if shift>12 else 0)|((1<<63) if active['nx'] else 0),'RAM PS/PAT/NX/permission')
        if shift>12:
            E.require(pa>=0x200000 and (pa+size<=image[0] or pa>=image[1]),'RAM prohibited prefix/image large leaf')
            E.require(all(point%(1<<shift)==0 for point in boundaries if pa<point<pa+size),
                'RAM large leaf crosses cache/provenance boundary')
        E.require(contains(expected,pa,size),'RAM extra Reserved/device/unreported page')
        actual.append((pa,pa+size));geometry.append((pa,pa+size,shift));counts[(shift-12)//9]+=size>>shift
    E.require(merge(actual)==expected,'RAM missing/extra interval coverage')
    total=sum(b-a for a,b in expected)
    E.require(total==number(state,'bytes')==active['ram_bytes'] and counts==active['leaves'] and
        max(b for _,b in expected)==active['ceiling'],'RAM leaf coverage/count/ceiling reconciliation')
    bitmap=single(rows,'BITMAP');physical=number(bitmap,'physical');size=number(bitmap,'bytes')
    represented=max(number(d,'end') for d in descriptors if number(d,'type') in (0,1,2,3,4,5,6,7,9,10))//4096
    bitmap_bytes=(represented+7)//8
    E.require(contains(expected,physical,size) and size==(bitmap_bytes*2+4095)&~4095 and
        number(bitmap,'owner')==number(bitmap,'member')==BASE+physical and
        number(bitmap,'reserved_owner')==number(bitmap,'reserved_member')==BASE+physical+bitmap_bytes,
        'RAM bitmap owner/member old epoch')
    heap=single(rows,'HEAP');E.require(number(heap,'accessed')==1 and number(heap,'bytes')==64 and
        number(heap,'cpu')==BASE+number(heap,'physical') and contains(expected,number(heap,'physical'),64),'RAM heap actual access/domain')
    boot=single(rows,'BOOT')
    for key in ('view','map','descriptors','font','header','logo'):
        value=number(boot,key)
        E.require((key in ('font','header','logo') and not value) or IMAGE+image[0]<=value<IMAGE+image[1],
            'RAM image-owned boot copy was rebased')
    for key in ('glyph','pixels'):
        cpu=number(boot,'glyphs' if key=='glyph' else key);pa=number(boot,key+'_physical')
        E.require(not pa and not cpu or cpu==BASE+pa and contains(expected,pa,1),'RAM nested boot pointer stale')
    rsdp=number(boot,'rsdp');rpa=number(boot,'rsdp_physical')
    E.require(rsdp==BASE+rpa if contains(expected,rpa,1) else 0xffffd00000000000<=rsdp<0xffffd00040000000,
        'RAM RSDP successor domain')
    E.require(fb[0]<=number(boot,'framebuffer_physical')<fb[1] and
        0xffffd00000000000<=number(boot,'framebuffer')<0xffffd00040000000,'RAM framebuffer lost owned domain')
    if high:
        h=single(rows,'HIGH');pa=number(h,'physical');table=number(h,'table')
        E.require(pa>=2**32 and pa%4096==0 and number(h,'bytes')==12288 and
            contains(expected,pa,12288) and number(h,'cpu')==BASE+pa and number(h,'inverse')==pa and
            number(h,'first')==0x686967686672616d and number(h,'last')==0x7461696c61636365 and
            table>=2**32 and table%4096==0 and table!=pa and contains(expected,table,4096) and
            number(h,'table_cpu')==BASE+table and number(h,'entry')==pa|3|((1<<63) if active['nx'] else 0) and
            number(h,'reallocated')==pa and number(h,'released')==number(h,'valid')==1,
            'RAM real high frame/table access or retirement')
    return {'generation':generation,'coverage':expected,'bytes':total,'leaves':counts,'one_gib_supported':large,
            'high':high,'bitmap':bitmap,'boot':boot,'heap':heap,'root':active['root'],'epoch':active['epoch'],
            'leaf_runs':leaf_runs(geometry)}

def physical_tables(directory, observed, active):
    """Decode stopped physical bytes independently of the guest's leaf emitter."""
    meta=json.loads((directory/'table-snapshot.json').read_text())
    run=json.loads((directory/'result.json').read_text())
    data=(directory/'table-frames.bin').read_bytes();addresses=meta['frame_addresses']
    E.require(meta['candidate_id']==run['candidate_id'] and meta['vm_id']==run['vm_id'] and
        meta.get('memory_access')=='QEMU_PHYSICAL_PACKET_VERIFIED' and
        meta['active_root']==active['root']==meta['cr3'] and
        meta['frames_sha256']==E.sha(directory/'table-frames.bin') and
        addresses==sorted(set(addresses)) and len(data)==len(addresses)*4096,
        'RAM physical snapshot missing or source/root binding')
    frames={a:data[i*4096:(i+1)*4096] for i,a in enumerate(addresses)}
    physical_mask=((1<<active['physical_bits'])-1)&~4095
    allowed_address=0x000ffffffffff000
    seen=set();leaves=[];geometry=[];counts=[0,0,0]
    def walk(pa,level,va,inherited_nx):
        E.require(pa in frames and pa not in seen and pa%4096==0,'RAM snapshot link/cycle')
        seen.add(pa)
        for index,(entry,) in enumerate(struct.iter_unpack('<Q',frames[pa])):
            if not entry&1:continue
            first=va+(index<<(12+9*(level-1)))
            if level==4:
                first|=0xffff000000000000 if index&256 else 0
                if not BASE<=first<BASE+SPAN:continue
            E.require(entry&7 in ((1,3) if WX.enabled() else (3,)) and not(entry&allowed_address&~physical_mask),
                'RAM snapshot physical width/supervisor/write permission')
            E.require(not(entry&~(physical_mask|0xe3|(1<<63))),
                'RAM snapshot unknown entry flags')
            nx=inherited_nx or bool(entry&(1<<63))
            E.require(not(entry&(1<<63)) or active['nx'],'RAM snapshot reserved NX')
            leaf=level==1 or level in (2,3) and entry&128
            E.require(level!=4 or not(entry&128),'RAM snapshot invalid PML4 PS')
            if leaf:
                shift=12+9*(level-1);size=1<<shift
                E.require(level!=3 or observed['one_gib_supported'],'RAM snapshot unsupported one-GiB')
                cache_mask=24|(128 if level==1 else 4096)
                E.require(not(entry&cache_mask) and nx==active['nx'] and
                    not(entry&(size-1)&~(4095 if level==1 else 8191)),
                    'RAM snapshot cache/NX/alignment')
                physical=entry&physical_mask&~(size-1)
                E.require(first==BASE+physical,'RAM snapshot wrong physical leaf')
                leaves.append((physical,physical+size));geometry.append((physical,physical+size,shift));counts[level-1]+=1
            else:
                E.require(not(entry&0xff8&~0x20),'RAM snapshot link flags')
                walk(entry&physical_mask,level-1,first,nx)
    walk(active['root'],4,0,False)
    E.require(merge(leaves)==observed['coverage'] and counts==observed['leaves'],
        'RAM independent physical tables differ from full descriptor coverage')
    E.require(leaf_runs(geometry)==observed['leaf_runs'],
        'RAM independent physical leaf geometry differs from checked boundary runs')
    return {'status':'PASS','table_frames':len(seen),'leaves':counts,'bytes':sum(b-a for a,b in leaves),
        'memory_access':meta['memory_access'],'snapshot_sha256':meta['frames_sha256']}

def cpu_formats(lines, active):
    prefix='[CPU_CONTROL][CPU] '
    cpus=[fields(l[len(prefix):]) for l in lines if l.startswith(prefix)]
    E.require(cpus,'RAM local CPU feature evidence absent')
    for cpu in cpus:
        width=number(cpu,'address_eax')&255;edx=number(cpu,'extended_edx')
        E.require(number(cpu,'cr3')==active['root'] and width==number(cpu,'physical_bits') and
            active['physical_bits']<=width<=52 and edx&(1<<29) and
            (not active['nx'] or edx&(1<<20)) and
            (not active['leaves'][2] or edx&(1<<26)),
            'RAM participating CPU cannot load declared page format')
    return {'cpus':len({number(c,'slot') for c in cpus}),
        'generations':sorted({number(c,'generation') for c in cpus})}

def owner_readback(directory):
    """Check retained owner bytes, including every structural table CPU pointer."""
    import device_mapping_evidence as M
    directory=Path(directory);run=json.loads((directory/'result.json').read_text())
    proof=json.loads((directory/'ram-owner-readback.json').read_text())
    meta=json.loads((directory/'table-snapshot.json').read_text())
    config=json.loads((directory/'capture-config.json').read_text())
    lines=(directory/'qemu-serial.log').read_text().splitlines();active=activation(lines)
    raw=M.split_traces((directory/'qemu-debugcon.log').read_bytes())['RAM_MAP'][0]
    observed=trace(raw,lines,active,image_plan=__import__('kernel_image_evidence').elf_plan(directory) if WX.enabled() else None)
    E.require(run['readback'] and proof['candidate_id']==run['candidate_id'] and proof['vm_id']==run['vm_id'] and
        proof['root']==active['root']==meta['active_root'] and
        proof['access']=='STOPPED_VIRTUAL_AND_PHYSICAL_READS_EQUAL' and
        E.sha(directory/'ram-capture.py')==E.sha(E.ROOT/'scripts/ram-gdb-capture.py'),
        'RAM owner snapshot identity/source')
    expected={'schema':1,'cpu':{'slot':0,'apic_id':4,'is_bsp':12,'state':16,'stack_top':24,'tss_ptr':32,'gdt_ptr':40,
        'size':48,'online':3,'stack_bytes':16384},'tss':{'rsp0':4,'ist1':36,'ist2':44,'ist3':52,'size':104},
        'gdt':{'tss':40,'size':4096},'bitmap':{'buffer':0,'size':8,'total_bits':16,'size_bytes':24},
        'table':{'cpu':0,'physical':8,'size':16},'window':{'tables':0,'allocated':8224,'ready':8228,'size':8232,'capacity':514},
        'build':{'tables':0,'count':204928,'window':196608,'verified':204937,'activated':204938,
            'size':204944,'capacity':8192,'table_stride':24}}
    if LOW.enabled():
        expected['build']={'tables':0,'count':270464,'window':262144,'verified':270473,'activated':270474,
            'size':270480,'capacity':8192,'table_stride':32}
    if WX.enabled():
        expected['build'].update(count=270472,verified=270481,activated=270482,size=270488)
    if __import__('stack_current').enabled():
        # The 104-MiB stack region owns 52 PTs plus a PD and PDPT.
        expected['build'].update(count=271368,verified=271377,activated=271378,size=271384)
    E.require(proof['layout']==expected==config['ram_layout']==json.loads((directory/'ram-layout.stdout.log').read_text()),'RAM owner layout differs')
    captured={}
    for name,row in proof['objects'].items():
        path=directory/('ram-owner-'+name+'.bin');data=path.read_bytes()
        E.require(len(data)==row['size'] and E.sha(path)==row['sha256'] and
            row['address']==config['symbols'][name],'RAM owner raw bytes changed')
        captured[name]=data
    E.require(set(captured)=={'g_cpus','g_owned_window','g_boot_build','g_bitmap','g_reserved_bitmap',
        'g_bitmap_buffer','g_reserved_bitmap_buffer'},'RAM owner raw inventory')
    def word(data,offset,size=8):
        E.require(0<=offset<=len(data)-size,'RAM owner raw extent')
        return int.from_bytes(data[offset:offset+size],'little')
    def direct(cpu,size):
        E.require(BASE<=cpu<BASE+SPAN and contains(observed['coverage'],cpu-BASE,size),'RAM owner stale pointer/extent')
    stack_inventory=None
    if __import__('stack_current').enabled():
        import stack_guard_evidence as SG
        data=(directory/'table-frames.bin').read_bytes()
        frames={pa:data[i*4096:(i+1)*4096] for i,pa in enumerate(meta['frame_addresses'])}
        plan=__import__('kernel_image_evidence').elf_plan(directory)
        topology=__import__('kernel_image_evidence').ready_topology(lines)
        stack_inventory=SG.readback(directory,plan,observed['coverage'],active,topology,frames)['inventory']
    def stack(cpu,kind,first,size):
        if stack_inventory is None:direct(first,size);return
        selected=[v for v in stack_inventory.values() if v['cpu']==cpu and v['kind']==kind]
        E.require(len(selected)==1 and selected[0]['base']==first and selected[0]['capacity']==size,
            'RAM owner guarded stack CPU/IST extent mismatch')
    bitmap=observed['bitmap'];size=(number(bitmap,'reserved_owner')-number(bitmap,'owner'))
    for name,owner in (('g_bitmap','owner'),('g_reserved_bitmap','reserved_owner')):
        data=captured[name];cpu=word(data,0)
        E.require(len(data)==24 and cpu==number(bitmap,owner)==word(captured[name+'_buffer'],0) and
            word(data,8)==size and (word(data,16)+7)//8==size,'RAM owner bitmap/member mismatch')
        direct(cpu,size)
    count=proof['cpu_count'];cpus=captured['g_cpus'];facts=cpu_formats(lines,active)
    E.require(count==facts['cpus'] and len(cpus)==count*48 and count>1,'RAM owner CPU inventory')
    children={r['slot']:r for r in proof['children']}
    E.require(len(children)==len(proof['children'])==count-1,'RAM owner AP identity duplicate/missing')
    bsp=0;apics=set()
    for slot in range(count):
        cpu=cpus[slot*48:(slot+1)*48];apic=word(cpu,4,4);apics.add(apic)
        E.require(word(cpu,0,4)==slot and word(cpu,16,4)==3,'RAM owner CPU slot/readiness')
        if cpu[12]:
            bsp+=1;E.require(slot not in children,'RAM BSP confused with an AP owner');continue
        child=children[slot];top,tss,gdt=(word(cpu,n) for n in (24,32,40))
        E.require((top,tss,gdt)==(child['stack_top'],child['tss'],child['gdt']),'RAM AP pointer identity')
        stack(slot,3,top-16384,16384);direct(tss,104);direct(gdt,4096)
        tss_bytes=bytes.fromhex(child['tss_hex']);gdt_bytes=bytes.fromhex(child['gdt_hex'])
        E.require(len(tss_bytes)==104 and len(gdt_bytes)==4096 and word(tss_bytes,4)==top and
            len(bytes.fromhex(child['stack_first_hex']))==8,'RAM AP stack/TSS handoff')
        descriptor=gdt_bytes[40:56]
        target=word(descriptor,2,2)|(descriptor[4]<<16)|(descriptor[7]<<24)|(word(descriptor,8,4)<<32)
        E.require(target==tss and descriptor[5] in (0x89,0x8b),'RAM GDT TSS CPU pointer')
        E.require(len(child['ist'])==3,'RAM IST inventory')
        for index,row in enumerate(child['ist']):
            E.require(row['top']==word(tss_bytes,36+index*8) and len(bytes.fromhex(row['first_hex']))==8,'RAM IST pointer identity')
            stack(slot,4+index,row['top']-32768,32768)
    E.require(bsp==1 and len(apics)==count,'RAM CPU topology identity')
    frames=set(meta['frame_addresses']);build=captured['g_boot_build'];window=captured['g_owned_window']
    layout=expected['build']
    E.require(len(build)==layout['size'] and build[layout['verified']:layout['activated']+1]==b'\x01\x01','RAM owner preactivation state')
    def tables(data,count,stride):
        physicals=set()
        for i in range(count):
            cpu,pa=word(data,i*stride),word(data,i*stride+8)
            E.require(cpu==BASE+pa and pa in frames and pa%4096==0 and pa not in physicals,'RAM owner table pointer old epoch')
            direct(cpu,4096);physicals.add(pa)
        return physicals
    count=word(build,layout['count'],4);E.require(0<count<=8192,'RAM construction owner count')
    constructed=tables(build,count,layout['table_stride'])
    E.require(len(window)==8232 and word(window,8224,4)==514 and window[8228]==1 and
        window==build[layout['window']:layout['window']+8232],'RAM owner window publication')
    E.require(tables(window,514,16)<=constructed,'RAM window construction ownership')
    root=proof['bootstrap_root']
    if LOW.enabled():
        E.require(root==proof['bootstrap_pointer']==0,'retired bootstrap observer pointer remains live')
    else:
        E.require(0<root<2**32 and root%4096==0 and root in frames and
            proof['bootstrap_pointer']==BASE+root,'RAM physical bootstrap root or CPU pointer')
    return {'status':'PASS','cpu_owners':facts['cpus'],'ap_owners':len(children),'window_tables':514,
        'construction_tables':count,'bootstrap_root':root,'source_sha256':E.sha(directory/'ram-capture.py'),
        'snapshot_sha256':E.sha(directory/'ram-owner-readback.json')}

def verify_directory(directory, mappings):
    import device_mapping_evidence as D
    lines=(directory/'qemu-serial.log').read_text().splitlines()
    active=activation(lines);run=json.loads((directory/'result.json').read_text())
    blocks=D.split_traces((directory/'qemu-debugcon.log').read_bytes())['RAM_MAP']
    E.require(len(blocks)==1,'RAM workload omitted/duplicated')
    high=run.get('ram_profile','normal')!='normal'
    result=trace(blocks[0],lines,active,high=high,image_plan=__import__('kernel_image_evidence').elf_plan(directory) if WX.enabled() else None)
    result['cpu_formats']=cpu_formats(lines,active)
    for mapping in mappings:
        for view in mapping['view_records']:
            if D.number(view,'origin')==0:continue
            first=D.number(view,'physical')&~4095
            end=(D.number(view,'physical')+D.number(view,'bytes')+4095)&~4095
            E.require(all(end<=a or first>=b for a,b in result['coverage']),
                'RAM managed device has a WB direct alias')
    if run.get('ram_profile')=='high-fallback':
        E.require(not result['one_gib_supported'] and result['leaves'][2]==0 and result['leaves'][1]>0,
            'RAM fallback lacks actual two-MiB path')
    if run.get('ram_profile')=='high':
        E.require(result['one_gib_supported'] and result['leaves'][2]>0,
            'RAM high KVM lacks actual one-GiB path')
    result['independent']=physical_tables(directory,result,active) if run['readback'] else None
    return result
