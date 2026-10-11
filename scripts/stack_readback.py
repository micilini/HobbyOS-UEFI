"""Exact read-only task lifetime stops, integrated with the bootstrap diagnostic."""
import json
from pathlib import Path
import re
import struct
import memory_evidence as E

NAMES={'kernel_stack_retire','scheduler_reap_zombies','g_stack_owners','g_kernel_root_physical',
       'g_map','g_audit','g_audit_completed','g_bitmap_buffer','g_reserved_bitmap_buffer'}
def return_site(text,names):
    rows=[]
    for line in text.splitlines():
        m=re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(.*)$',line)
        if m:rows.append((int(m[1],16),bytes.fromhex(m[2]),m[3]))
    hits=[i for i,row in enumerate(rows) if re.fullmatch(r'call\s+[0-9a-f]+ <kernel_stack_retire>',row[2])]
    E.require(len(hits)==1,'stack lifetime actual reaper call missing/duplicate')
    i=hits[0];at,code,_=rows[i]
    E.require(len(code)==5 and code[0]==0xe8 and at+5+int.from_bytes(code[1:],'little',signed=True)==names['kernel_stack_retire'] and
        i+2<len(rows) and rows[i+1][0]==at+5 and '<kfree>' in rows[i+2][2],
        'stack lifetime actual call/return/free instruction order')
    return at+5

def lines(cfg):
    output=Path(cfg['output']);source=output/'stack-lifetime-capture.py';config=output/'stack-lifetime-config.json'
    result=[]
    for phase,site in (('before',cfg['symbols']['kernel_stack_retire']),('after',cfg['return_site'])):
        result += [f'hbreak *0x{site:x}','continue',
            f'python exec(compile(open({json.dumps(str(source))}).read(), "stack-lifetime", "exec"), dict(STACK_LIFETIME_CONFIG={json.dumps(str(config))}, STACK_LIFETIME_PHASE={json.dumps(phase)}))',
            'delete breakpoints']
    return result

def prepare(root,directory,candidate,result,run):
    names={s[2]:int(s[0],16) for line in (directory/'symbols.txt').read_text().splitlines()
        if len(s:=line.split())==3 and s[2] in NAMES}
    E.require(set(names)==NAMES,'stack lifetime current ELF symbols')
    proof=json.loads((candidate/'image-elf-proof.json').read_text())
    identity=json.loads((candidate/'candidate.json').read_text())
    E.require(proof['elf_sha256']==identity['artifacts']['kernel.elf']['sha256'] and
        proof['elf_size']==identity['artifacts']['kernel.elf']['size'],'stack lifetime ELF fragment candidate binding')
    E.write(directory/'image-elf-proof.json',proof)
    run('stack-lifetime-code',['objdump','-d','-Mintel','--disassemble=scheduler_reap_zombies',str(candidate/'kernel.elf')])
    binary=directory/'stack-lifetime-layout'
    run('stack-lifetime-layout-build',['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        'scripts/stack-lifetime-layout.c','-o',str(binary)])
    run('stack-lifetime-layout',[str(binary)])
    cfg={'schema':1,'output':str(directory),'symbols':names,'layout':json.loads((directory/'stack-lifetime-layout.stdout.log').read_text()),
        'return_site':return_site((directory/'stack-lifetime-code.stdout.log').read_text(),names),
        **{k:result[k] for k in ('candidate_id','vm_id','boot_id')}}
    E.write(directory/'stack-lifetime-config.json',cfg)
    (directory/'stack-lifetime-capture.py').write_bytes((root/'scripts/stack-lifetime-gdb.py').read_bytes())
    return lines(cfg)

LAYOUT={'schema':1,'audit':{'spec':0,'result':184,'mapping':248,'size':256},
    'spec':{'generation':0,'root':8,'interval':16,'targets':48,'apic':52,'operation':180,'size':184},
    'result':{'status':0,'requested':4,'acknowledged':8,'missing':12,'failed':16,'send_failed':24,'generation':40,'size':64},
    'interval':{'first':0,'last':8,'pages':16,'strategy':24,'size':32}}

def verify(directory,plan,active,topology,coverage):
    """Reconstruct both page paths and the actual completed request from bytes."""
    import direct_map_evidence as RAM
    import stack_guard_evidence as SG
    import memory_elf_proof as ELF
    directory=Path(directory);cfg=json.loads((directory/'stack-lifetime-config.json').read_text())
    result=json.loads((directory/'result.json').read_text());launch=json.loads((directory/'launch.json').read_text())
    E.require(cfg['schema']==1 and cfg['symbols']=={k:plan['symbols'][k] for k in NAMES} and
        cfg['layout']==LAYOUT==json.loads((directory/'stack-lifetime-layout.stdout.log').read_text()) and
        all(cfg[k]==result[k] for k in ('candidate_id','vm_id','boot_id')) and
        E.sha(directory/'stack-lifetime-capture.py')==E.sha(E.ROOT/'scripts/stack-lifetime-gdb.py'),
        'stack lifetime source/ELF/candidate/layout binding')
    commands=[json.loads(line) for line in (directory/'commands.jsonl').read_text().splitlines()]
    selected={r['name']:r for r in commands if r['name'] in
        ('stack-lifetime-code','stack-lifetime-layout-build','stack-lifetime-layout')}
    output=Path(cfg['output']);binary=str(output/'stack-lifetime-layout')
    expected={'stack-lifetime-code':['objdump','-d','-Mintel','--disassemble=scheduler_reap_zombies',launch['elf']['path']],
        'stack-lifetime-layout-build':['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','scripts/stack-lifetime-layout.c','-o',binary],
        'stack-lifetime-layout':[binary]}
    E.require(set(selected)==set(expected) and all(selected[k]['exit_status']==0 and selected[k]['argv']==v
        for k,v in expected.items()),'stack lifetime actual code/layout commands')
    site=return_site((directory/'stack-lifetime-code.stdout.log').read_text(),cfg['symbols'])
    E.require(site==cfg['return_site'],'stack lifetime return address changed')
    # The independently retained ELF bytes must contain this exact near call.
    elf_proof=json.loads((directory/'image-elf-proof.json').read_text())
    candidate=json.loads((directory/'candidate.json').read_text())
    E.require(elf_proof['elf_sha256']==candidate['artifacts']['kernel.elf']['sha256'] and
        elf_proof['elf_size']==candidate['artifacts']['kernel.elf']['size'],'stack lifetime ELF fragment candidate binding')
    elf=ELF.replay(elf_proof)
    text=elf.section('.text');symbol=elf.symbol('scheduler_reap_zombies');at=text['offset']+site-5-text['addr']
    code=elf.data[at:at+5]
    E.require(symbol['value']<=site-5<site<symbol['value']+symbol['size'] and len(code)==5 and code[0]==0xe8 and
        site+int.from_bytes(code[1:],'little',signed=True)==cfg['symbols']['kernel_stack_retire'],
        'stack lifetime return stop disagrees with actual ELF call')
    raw_output=(directory/'bootstrap-before.stdout.log').read_text().splitlines();states={}
    mask=((1<<active['physical_bits'])-1)&~4095
    for phase in ('before','after'):
        prefix='stack-lifetime-'+phase;proof=json.loads((directory/(prefix+'.json')).read_text())
        E.require(proof['schema']==1 and proof['phase']==phase and all(proof[k]==result[k] for k in ('candidate_id','vm_id','boot_id')) and
            proof['root']==active['root'] and proof['cr0']==0x80010033 and proof['cr4']==0x20 and
            proof['efer']==(0xd00 if active['nx'] else 0x500) and
            proof['rip']==(cfg['symbols']['kernel_stack_retire'] if phase=='before' else site) and
            proof['memory_access']=='QEMU_PHYSICAL_PACKET_VERIFIED' and raw_output.count(
                'STACK_LIFETIME_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))==1,
            'stack lifetime raw debugger stop/root/controls binding')
        E.require(set(proof['blobs'])=={'owner','tables','audit','memory-map'},'stack lifetime blob inventory')
        blobs={}
        for name,row in proof['blobs'].items():
            path=directory/(prefix+'-'+name+'.bin');blobs[name]=path.read_bytes()
            E.require(row=={'size':len(blobs[name]),'sha256':E.sha(path)},'stack lifetime raw blob differs')
        E.require(len(blobs['owner'])==400,'stack lifetime owner size')
        sequence,*cells=struct.unpack('<50Q',blobs['owner']);identity=proof['identity'];slot=identity['slot']
        E.require(sequence%2==0 and 128<=slot<1664 and identity==dict(slot=slot,generation=cells[0],task=cells[5],lifecycle=cells[6]) and
            all(identity[k]>0 for k in ('generation','task','lifecycle')) and cells[8]==slot and cells[9]==16384 and
            cells[12]==1 and cells[13]==(5 if phase=='before' else 0) and cells[14]==cells[16]==1 and
            proof['owner_address']==cfg['symbols']['g_stack_owners']+slot*400,
            'stack lifetime missing/stale task owner or wrong state')
        guard,base,end,physical=cells[1:5]
        E.require(guard==SG.BASE+slot*SG.PITCH and base==guard+4096 and end==base+16384 and physical%4096==0 and
            RAM.contains(coverage,physical,16384),'stack lifetime payload/backing geometry')
        stride=proof['map_stride'];raw_map=blobs['memory-map']
        E.require(40<=stride<=4096 and 0<len(raw_map)<=1048576 and len(raw_map)%stride==0,
            'stack lifetime raw firmware map extent')
        ranges=[struct.unpack_from('<IIQQQQ',raw_map,i) for i in range(0,len(raw_map),stride)]
        addresses=proof['frame_addresses']
        E.require(addresses==sorted(set(addresses)) and 0<len(addresses)<=8 and
            len(blobs['tables'])==len(addresses)*4096 and all(RAM.contains(coverage,pa,4096) and
                any(kind in (1,2,3,4,5,6,7,9,10) and first<=pa and pa+4096<=first+pages*4096
                    for kind,_,first,_,pages,_ in ranges) for pa in addresses),
            'stack lifetime table chunk/physical provenance')
        frames={pa:blobs['tables'][i*4096:(i+1)*4096] for i,pa in enumerate(addresses)};visited=set()
        def lookup(va):
            table=active['root'];write=True;nx=False
            for level in (4,3,2,1):
                E.require(table in frames,'stack lifetime table path missing');visited.add(table)
                raw=struct.unpack_from('<Q',frames[table],((va>>(12+9*(level-1)))&511)*8)[0]
                if not raw:return None
                E.require(raw&1 and not raw&4 and not raw&~(mask|0xfb|(1<<63)) and
                    (active['nx'] or not raw&(1<<63)),'stack lifetime reserved entry format')
                write=write and bool(raw&2);nx=nx or bool(raw&(1<<63))
                if level==1 or level in (2,3) and raw&128:
                    return raw&mask,level,write,nx,raw
                E.require(level!=4 or not raw&128,'stack lifetime root PS');table=raw&mask
            raise ValueError('stack lifetime table depth')
        E.require(lookup(guard) is None,'stack lifetime guard is present')
        for va in range(base,end,4096):
            entry=lookup(va)
            if phase=='before':
                E.require(entry and entry[:4]==(physical+va-base,1,True,bool(active['nx'])) and not entry[4]&0x98,
                    'stack lifetime live payload/rights incorrect')
            else:E.require(entry is None,'stack lifetime retired payload still mapped')
        E.require(visited==set(addresses),'stack lifetime unexplained table chunk')
        bits=proof['allocation_bits']
        E.require(len(bits)==4 and [r['physical'] for r in bits]==list(range(physical,physical+16384,4096)),
            'stack lifetime physical allocation extent')
        for row in bits:
            for name in ('g_bitmap_buffer','g_reserved_bitmap_buffer'):
                b=row['fields'][name];index=row['physical']//4096
                E.require(RAM.BASE<=b['pointer']<RAM.BASE+RAM.SPAN and b['byte_address']==b['pointer']+index//8 and
                    0<=b['byte']<256 and b['bit']==(b['byte']>>(index%8))&1 and
                    b['bit']==(int(phase=='before') if name=='g_bitmap_buffer' else 0),
                    'stack lifetime backing released early, reserved or retained after completion')
        states[phase]=dict(proof=proof,cells=cells,blobs=blobs)
    a,b=states['before'],states['after'];before,after=a['cells'],b['cells']
    E.require(a['proof']['identity']==b['proof']['identity'] and before[:7]==after[:7] and
        before[8:13]==after[8:13] and before[14:]==after[14:] and not a['blobs']['audit'],
        'stack lifetime owner replaced before retirement observation')
    generation=after[7];audit=b['blobs']['audit'];targets=sum(1<<cpu for cpu in topology)
    E.require(0<generation<=b['proof']['audit_completed']<=8192 and len(audit)==256,
        'stack lifetime missing completed audit record')
    def u(at,size=8):return int.from_bytes(audit[at:at+size],'little')
    E.require(u(0)==u(224)==generation and u(8)==active['root'] and u(16)==after[2] and u(24)==after[3]-4096 and
        u(32)==4 and u(40,4)==1 and u(48,4)==u(188,4)==u(192,4)==targets and u(180,4)==1 and
        all(u(at,4)==0 for at in (184,196,200,208)) and audit[248]==1 and
        all(u(52+slot*4,4)==apic for slot,apic in topology.items()),
        'stack lifetime backing/VA freed without actual all-CPU ACK')
    records=SG.serial_records(directory)
    matching=[r for kind,r in records if kind=='RETIRE' and SG.serial_number(r,'slot')==after[8] and
        SG.serial_number(r,'generation')==after[0]]
    E.require(len(matching)==1 and SG.serial_number(matching[0],'request')==generation and
        SG.serial_number(matching[0],'task')==after[5] and SG.serial_number(matching[0],'lifecycle')==after[6],
        'stack lifetime raw retirement differs from terminal audit chronology')
    return {'status':'PASS','identity':b['proof']['identity'],'request':generation,'acknowledged_cpus':len(topology),
        'guard_pages':1,'payload_pages':4,'before':'LIVE_MAPPED_OWNED','after':'ABSENT_FREE_AFTER_ALL_ACK',
        'backing_bytes':16384,'retired_payload_read':False}
