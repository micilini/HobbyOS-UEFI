"""Owned-mapping predicates layered on the existing raw memory evidence."""
import binascii
import json
from pathlib import Path
import re
import struct
import memory_evidence as E
import direct_map_evidence as RAM
import kernel_image_current as WX

WINDOW=0xffffd00000000000
WINDOW_END=WINDOW+0x40000000
MASK=0x000ffffffffff000
PAT=(6,4,7,0,6,1,7,0)

def fields(line,prefix):
    E.require(line.startswith(prefix),'mapping record prefix')
    result={}
    for item in line[len(prefix):].split():
        key,sep,value=item.partition('=')
        E.require(sep and key not in result,'mapping record fields')
        result[key]=value
    return result

def number(row,key):
    text=row[key]
    E.require(re.fullmatch(r'(?:0x[0-9a-f]+|[0-9]+)',text),'mapping numeric field: '+key)
    return int(text,16 if text.startswith('0x') else 10)

def split_traces(raw,maximum_bytes=32*1024*1024):
    """Both producers use complete records on the same existing debug console.

    Unknown bytes, interleaving, missing terminal records and incomplete lines
    fail; no broken line is joined or discarded to create an accepted block.
    """
    E.require(len(raw)<=maximum_bytes and b'\r' not in raw,'mapping trace size/framing')
    result={'TLB':[],'MAPPING':[]};active=None;block=bytearray()
    if RAM.enabled():result['RAM_MAP']=[]
    if WX.enabled():result['IMAGE_PROTECTION']=[]
    for line in raw.splitlines(keepends=True):
        E.require(line.endswith(b'\n'),'truncated debug-console record')
        match=re.match(rb'^\[(TLB|MAPPING|RAM_MAP|IMAGE_PROTECTION)\]\[(\w+)\] ',line)
        E.require(match,'unknown debug-console producer')
        producer,kind=(v.decode() for v in match.groups())
        E.require(producer in result,'unknown debug-console producer contract')
        if active is None:
            E.require(kind=='BEGIN','trace starts without independent begin')
            active=producer
        E.require(active==producer and (not block or kind!='BEGIN'),'interleaved/duplicate trace begin')
        block.extend(line)
        if kind==('INVENTORY' if producer=='MAPPING' else 'END'):
            result[producer].append(bytes(block));block.clear();active=None
    E.require(not block and active is None,'missing trace terminal record')
    return result

def mapping_rows(raw):
    result={}
    for line in raw.decode('ascii').splitlines():
        body,sep,crc=line.rpartition(' crc=')
        E.require(sep and re.fullmatch('[0-9a-f]{8}',crc) and
            binascii.crc32(body.encode())==int(crc,16),'mapping record CRC')
        match=re.match(r'^\[MAPPING\]\[(\w+)\] ',body)
        E.require(match,'mapping record namespace')
        result.setdefault(match[1],[]).append(fields(body,match[0]))
    E.require(set(result)<=set('BEGIN VIEW LEAVES DMA USB ROOT HUB CHILD INVENTORY'.split()),'unknown mapping record')
    return result

def single(data,name):
    E.require(len(data.get(name,[]))==1,'mapping missing/duplicate '+name)
    return data[name][0]

def failure_chronology(lines,topology,kind):
    checkpoints=[fields(l,'[TLB_FAILURE][CHECKPOINT] ') for l in lines if l.startswith('[TLB_FAILURE][CHECKPOINT] ')]
    E.require(len(checkpoints)==1,'failure audit checkpoint missing/duplicate')
    checkpoint=checkpoints[0];before=number(checkpoint,'before');root=number(checkpoint,'root')
    E.require(before==number(checkpoint,'completed') and before>0,'failure audit incomplete checkpoint')
    roots={int(fields(l,'[CPU_STATE][READY] ')['cr3'],16) for l in lines if l.startswith('[CPU_STATE][READY] ')}
    E.require(roots=={root},'failure audit actual CPU root')
    records=[fields(l,'[TLB_FAILURE][SERVICE] ') for l in lines if l.startswith('[TLB_FAILURE][SERVICE] ')]
    end=before+(3 if kind=='delayed-response' else 0)
    E.require([number(r,'generation') for r in records]==list(range(1,end+1)),
              'failure audit generation coverage')
    mask=sum(1<<s for s in topology);identities=','.join(f'{s}:{topology[s][0]}' for s in sorted(topology))
    for r in records:
        generation=number(r,'generation');prelude=generation<=before
        E.require(number(r,'root')==root and number(r,'targets')==mask and r['identities']==identities,
                  'failure audit target/root identity')
        E.require(number(r,'sent')&~mask==0 and number(r,'notifications')==number(r,'sent').bit_count(),
                  'failure audit notification round')
        if prelude:
            E.require(number(r,'mapping')==1 and number(r,'operation') in (1,3),'failure audit unknown prelude')
        else:
            E.require(number(r,'mapping')==0 and number(r,'operation')==(2 if generation==before+2 else 1),
                      'failure audit fixture operation')
            E.require(number(r,'first')==number(r,'last')==0xffffc000001ff000 and number(r,'pages')==1,
                      'failure audit fixture interval')
        first,last,pages=(number(r,k) for k in ('first','last','pages'))
        E.require(pages>0 and first%4096==0 and last==first+(pages-1)*4096 and
            number(r,'strategy')==(1 if pages<=64 else 2),'failure audit normalized interval')
        if number(r,'operation')==3:E.require(first==last==0 and pages==1,'failure audit cache operation range')
        timeout=kind=='delayed-response' and generation==end
        E.require(number(r,'status')==(7 if timeout else 0) and number(r,'ack')==(mask&~2 if timeout else mask) and
            number(r,'missing')==(2 if timeout else 0) and number(r,'failed')==number(r,'send_failed')==0,
            'failure audit terminal completion/quarantine')
        E.require((1000000000<=number(r,'elapsed_ns')<2000000000) if timeout else number(r,'elapsed_ns')<1000000000,
                  'failure audit deadline')
    return end

def walk_snapshot(directory):
    meta=json.loads((directory/'table-snapshot.json').read_text());data=(directory/'table-frames.bin').read_bytes()
    addresses=meta['frame_addresses']
    E.require(E.sha(directory/'table-frames.bin')==meta['frames_sha256'] and
        addresses==sorted(set(addresses)) and len(data)==len(addresses)*4096,'device table bytes missing/changed')
    frames={a:data[i*4096:(i+1)*4096] for i,a in enumerate(addresses)}
    def walk(va):
        table=meta['active_root']
        for level in (4,3,2,1):
            E.require(table in frames,'device table frame absent')
            entry=struct.unpack_from('<Q',frames[table],((va>>(12+9*(level-1)))&511)*8)[0]
            E.require(entry&1,'device owned range is unmapped')
            if level==1 or level in (2,3) and entry&128:
                offset=(1<<(12+9*(level-1)))-1
                return (entry&MASK&~offset)|(va&offset)
            table=entry&MASK
        raise ValueError('device raw walk exhausted')
    return walk

def verify_mapping(raw,serial,root,topology,reference=False,*,usb_controller=True,ram=None):
    receipts=[fields(l,'[MAPPING_TRACE][COMPLETE] ') for l in serial if l.startswith('[MAPPING_TRACE]')]
    E.require(len(receipts)==1,'mapping trace binding missing/duplicate')
    receipt=receipts[0];digest=14695981039346656037
    for byte in raw:digest=((digest^byte)*1099511628211)&((1<<64)-1)
    E.require(receipt['status']=='PASS' and receipt['channel']=='debugcon' and receipt['port']=='0x402' and
        number(receipt,'bytes')==len(raw) and number(receipt,'records')==raw.count(b'\n') and
        number(receipt,'fnv64')==digest,'mapping serial/raw trace binding')
    data=mapping_rows(raw);begin=single(data,'BEGIN');end=single(data,'INVENTORY')
    generation=number(begin,'generation')
    E.require(generation==number(end,'generation')==number(receipt,'generation') and
        number(begin,'root')==root and number(begin,'window')==WINDOW and
        number(begin,'window_bytes')==0x40000000 and number(begin,'tables')==514,'mapping root/window/identity')
    views={};vas=set();pages=0;aliases={};framebuffer=[]
    for row in data.get('VIEW',[]):
        slot=number(row,'slot');va=number(row,'va');pa=number(row,'physical');size=number(row,'bytes')
        offset=pa&4095;count=(offset+size+4095)//4096;flags=number(row,'flags');origin=number(row,'origin')
        E.require(slot<512 and slot not in views and number(row,'generation')>0 and size>0 and
            count==number(row,'pages') and number(row,'state')==2 and va&4095==offset and
            WINDOW<=va-offset<va+size<=WINDOW_END and pa+size<=1<<48,'owned view extent/state')
        nx=(1<<63) if WX.enabled() and ram['nx'] else 0
        E.require(flags&1 and not flags&~(0x9b|nx) and flags&(1<<63)==nx and 0<=origin<=7,'owned view permission bits')
        selection=((flags>>3)&3)|(((flags>>7)&1)<<2);cache=PAT[selection]
        if origin==0:E.require(flags==1|nx,'firmware must be WB read-only')
        elif origin==1:
            E.require(selection==(4 if reference else 5),'framebuffer selected cache policy');framebuffer.append(row)
        else:E.require(cache in (0,7),'device registers have cacheable selection')
        entries=[r for r in data.get('LEAVES',[]) if number(r,'slot')==slot]
        expected=0
        for item in entries:
            E.require(number(item,'first')==expected,'mapping leaf coverage gap/duplicate')
            pairs=item['entries'].split(',');n=min(8,count-expected)
            E.require(number(item,'count')==len(pairs)==n and n>0,'mapping leaf block length')
            for pair in pairs:
                E.require(re.fullmatch('[0-9a-f]+:[0-9a-f]+',pair),'mapping leaf pair syntax')
                leaf,identity=(int(v,16) for v in pair.split(':'));physical=(pa&~4095)+expected*4096
                address=(va&~4095)+expected*4096
                E.require(address not in vas,'overlapping owned VA reservation');vas.add(address)
                E.require(leaf&~0x60==physical|flags,'actual managed leaf differs from ownership')
                if RAM.LOW.enabled():E.require(identity==0,'retired identity alias unexpectedly restored')
                if origin:E.require(identity==0,'incompatible device identity alias remains')
                elif identity:E.require(identity&MASK==physical and identity&0x98==0,'firmware alias cache conflict')
                E.require(physical not in aliases or aliases[physical]==cache,'incompatible managed aliases')
                aliases[physical]=cache;expected+=1
        E.require(expected==count,'mapping complete page envelope missing');pages+=count;views[slot]=row
    E.require(len(framebuffer)==1 and number(end,'views')==len(views) and number(end,'pages')==pages and views,
              'mapping inventory cardinality')
    E.require(all(number(r,'slot') in views for r in data.get('LEAVES',[])),'unowned leaf evidence')
    fb=framebuffer[0]
    summaries=[fields(l,'[MAPPING][FRAMEBUFFER] ') for l in serial if l.startswith('[MAPPING][FRAMEBUFFER]')]
    E.require(len(summaries)==1,'framebuffer full readback absent');summary=summaries[0]
    for key in ('va','physical','bytes','pages'):E.require(number(summary,key)==number(fb,key),'framebuffer inventory mismatch')
    E.require(number(summary,'identity_absent')==number(fb,'pages') and
        number(summary,'pat_slot')==(4 if reference else 5) and summary['effective_type']=='NOT_COMPUTED','framebuffer alias/type proof')
    dmas=set()
    for row in data.get('DMA',[]):
        slot=number(row,'slot');pa=number(row,'device');size=number(row,'backing');alignment=number(row,'alignment')
        E.require(slot<2048 and slot not in dmas and number(row,'generation')>0 and size>0 and size%4096==0 and
            0<number(row,'requested')<=size and alignment>0 and alignment&(alignment-1)==0 and
            pa%max(alignment,4096)==0 and pa+size-1<=number(row,'maximum') and
            pa+size<=number(row,'ceiling')<=(ram['ceiling'] if ram else 1<<32) and
            number(row,'cpu')==pa+(RAM.BASE if ram else 0) and
            number(row,'state') in (0,1,2,3),'DMA extent/domain/lifetime/constraint')
        if ram:E.require(number(row,'ceiling')==ram['ceiling'],'DMA differs from verified RAM ceiling')
        dmas.add(slot)
    E.require(len(dmas)==number(end,'dma_objects') and (bool(dmas) if usb_controller else not dmas),'DMA inventory cardinality')
    usb=single(data,'USB');cap=number(usb,'capabilities')
    if usb_controller:
        E.require(number(usb,'maximum')==((1<<64)-1 if cap&1 else (1<<32)-1) and
            number(usb,'active')==number(usb,'failed')==0,'actual xHCI capability/readiness')
    else:E.require(all(number(usb,k)==0 for k in ('capabilities','maximum','polls','active','failed')) and
        not any(data.get(k) for k in ('ROOT','HUB','CHILD')),'legacy PC falsely reports USB ownership')
    roots={}
    for r in data.get('ROOT',[]):
        port,slot=number(r,'port'),number(r,'slot')
        E.require(0<port<=16 and port not in roots and 0<slot<64 and
            number(r,'generation')>0 and number(r,'phase')==3,'root configured ownership')
        roots[port]=r
    hubs={}
    for h in data.get('HUB',[]):
        slot=number(h,'slot');port=number(h,'root')
        E.require(slot not in hubs and port in roots and number(h,'generation')==number(roots[port],'generation') and
            1<=number(h,'speed')<=5 and number(h,'lifecycle')>0,'hub root/lifecycle identity');hubs[slot]=h
    children=set()
    for child in data.get('CHILD',[]):
        parent=number(child,'parent');slot=number(child,'slot');port=number(child,'port')
        route=number(hubs[parent],'route') if parent in hubs else 0
        shift=0
        while shift<20 and (route>>shift)&15:shift+=4
        E.require(parent in hubs and slot not in children and slot!=parent and 0<slot<64 and
            0<port<=number(hubs[parent],'ports') and number(child,'retiring')==0 and
            shift<20 and route>>shift==0 and number(child,'route')==route|(port<<shift),
            'child topology/ownership')
        speed=number(child,'speed');hub=hubs[parent]
        tt=(parent,port) if speed in (1,2) and number(hub,'speed')==3 else (
            (number(hub,'tt_slot'),number(hub,'tt_port')) if speed in (1,2) and number(hub,'speed') in (1,2) else (0,0))
        E.require((number(child,'tt_slot'),number(child,'tt_port'))==tt,'translator ancestry mismatch');children.add(slot)
    probes=[fields(l,'[MAPPING][UNMAP] ') for l in serial if l.startswith('[MAPPING][UNMAP]')]
    E.require(len(probes)==8,'unmap cycle coverage')
    handles=set();views_seen=set();released=0
    for cycle,row in enumerate(probes):
        E.require(number(row,'generation')==generation and number(row,'cycle')==cycle and
            number(row,'before')==number(row,'marker')==0x7341520000000000|cycle and
            number(row,'removed')==number(row,'after')==number(row,'error')==0 and
            all(number(row,k)==1 for k in ('recovered','freed','stale','dma_stale')) and number(row,'cpu') in topology and
            number(row,'site')!=number(row,'recovery') and number(row,'backing')==number(row,'zeroed')==(cycle%4+1)*4096 and
            number(row,'alignment')==64<<cycle and number(row,'maximum')==0xffffffff and
            number(row,'physical')%max(4096,64<<cycle)==0 and number(row,'physical')+number(row,'backing')<=1<<32,
            'owned unmap/probe/zeroed constrained allocation')
        buffer=(number(row,'dma_slot'),number(row,'dma_generation'))
        view=(number(row,'view_slot'),number(row,'view_generation'))
        E.require(buffer not in handles and view not in views_seen and 0<=buffer[0]<2048 and 0<=view[0]<512 and
            buffer[1]>0 and view[1]>0,'fixture stale/duplicate resource generation')
        handles.add(buffer);views_seen.add(view);released+=number(row,'backing')
    terminal=[fields(l,'[MAPPING][END] ') for l in serial if l.startswith('[MAPPING][END]')]
    E.require(len(terminal)==1 and terminal[0]['status']=='PASS' and number(terminal[0],'cycles')==8 and
        number(terminal[0],'schema')==2 and number(terminal[0],'retained')==0 and
        number(terminal[0],'allocated_bytes')==number(terminal[0],'released_bytes')==released==20*4096 and
        number(terminal[0],'live_buffers')==number(terminal[0],'live_views')==0,
        'mapping repeated-cycle retention/accounting')
    return {'generation':generation,'views':len(views),'pages':pages,'dma_objects':len(dmas),'unmap_cycles':8,
        'framebuffer':fb,'usb':usb,'roots':list(roots.values()),'hubs':list(hubs.values()),'children':data.get('CHILD',[]),
        'view_records':list(views.values()),'dma_records':data.get('DMA',[])}

def cache_snapshot(directory):
    """Current active/bootstrap roots allow WC only in the original framebuffer.

    The unchanged entry root is inactive, after a non-global CR3 transition;
    it remains a separately inspected historical bootstrap construction.
    """
    import memory_table_proof as T
    meta=json.loads((directory/'table-snapshot.json').read_text());raw=(directory/'table-frames.bin').read_bytes()
    E.require(E.sha(directory/'table-frames.bin')==meta['frames_sha256'],'owned cache raw binding')
    addresses=meta['frame_addresses'];E.require(len(raw)==len(addresses)*4096 and len(set(addresses))==len(addresses),'owned table frame inventory')
    frames={a:raw[i*4096:(i+1)*4096] for i,a in enumerate(addresses)}
    boot=bytes.fromhex(meta['boot_metadata_hex']);_,_,_,_,fb,size,_=struct.unpack_from('<7Q',boot)
    first=fb&~4095;end=(fb+size+4095)&~4095;results=[]
    import low_current
    roots=[meta['active_root'],meta['static_frames']['boot_pml4']]
    if low_current.enabled():
        E.require(meta['bootstrap_root']==0,'retired bootstrap root remains registered')
        E.require(frames[meta['active_root']][:2048]==bytes(2048),'retired main root has a low cache alias')
    else:
        roots.insert(1,meta['bootstrap_root'])
    for root in roots:
        levels=T.traverse(root,frames,meta['physical_bits'],meta['page1g']);leaves={};links={};fb_frames=set()
        for address,level in levels.items():
            for entry, in struct.iter_unpack('<Q',frames[address]):
                if not entry&1:continue
                leaf=level==1 or level in (2,3) and entry&128
                slot=(entry>>3)&3
                if leaf:slot|=((entry>>(7 if level==1 else 12))&1)<<2
                if slot==5:
                    E.require(leaf and level==1 and first<=entry&MASK<end and root!=meta['static_frames']['boot_pml4'],
                        'WC outside owned framebuffer');fb_frames.add(entry&MASK)
                elif leaf and root!=meta['static_frames']['boot_pml4']:
                    physical=entry&MASK&~((1<<(12+9*(level-1)))-1)
                    E.require(not physical<end or physical+(1<<(12+9*(level-1)))<=first,'framebuffer incompatible alias in actual tables')
                target=leaves if leaf else links;target[str(slot)]=target.get(str(slot),0)+1
        if root!=meta['static_frames']['boot_pml4']:
            E.require(fb_frames==set(range(first,end,4096)),'actual full framebuffer WC coverage')
        results.append({'root':root,'frames':len(levels),'leaf_slots':leaves,'nonleaf_slots':links})
    return results
