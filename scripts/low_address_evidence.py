"""Explicit no-low root, architectural AP departure and exact NULL predicates."""
import importlib.util
import json
from pathlib import Path
import re
import struct
import memory_evidence as E
import low_current as CURRENT
import stack_current as STACK

enabled=CURRENT.enabled
BASE=0xffff800000000000
DIRECT_END=0xffffc00000000000
MASK=0x000ffffffffff000

def module(name):
    spec=importlib.util.spec_from_file_location('low_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def fields(line,prefix):
    E.require(line.startswith(prefix),'low-domain record prefix')
    row={}
    for item in line[len(prefix):].split():
        key,sep,value=item.partition('=')
        E.require(sep and key not in row,'low-domain duplicate/malformed field');row[key]=value
    return row

def records(lines,prefix):return [fields(l,prefix) for l in lines if l.startswith(prefix)]
def single(lines,prefix):
    rows=records(lines,prefix);E.require(len(rows)==1,'missing/duplicate '+prefix);return rows[0]
def h(row,key):
    value=row[key];E.require(re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]+',value),'low-domain hexadecimal field '+key)
    return int(value,16)
def d(row,key):
    E.require(re.fullmatch('[0-9]+',row[key]),'low-domain decimal field '+key);return int(row[key])
def high(first,size):return BASE<=first<1<<64 and 0<size<=((1<<64)-first)
def symbols(text):return {v[2]:int(v[0],16) for l in text.splitlines() if len(v:=l.split())==3}
def word(data,offset,size=8):
    E.require(0<=offset<=len(data)-size,'low-domain object bound');return int.from_bytes(data[offset:offset+size],'little')
def table_hash(data):
    E.require(len(data)==4096,'bootstrap table byte size');value=14695981039346656037
    for entry, in struct.iter_unpack('<Q',data):
        for byte in (entry&~0x60).to_bytes(8,'little'):value=((value^byte)*1099511628211)&((1<<64)-1)
    return value

def boot(lines,elf,cpus):
    preload=single(lines,'[LOW_ROOT] PRELOAD ');active=single(lines,'[LOW_ROOT] ACTIVE ')
    names=symbols(elf);first=names['bsp_stack_bottom'];end=names['bsp_stack_top']
    E.require(end-first==16384 and high(first,16384) and h(preload,'stack_first')==first and
        d(preload,'stack_bytes')==16384 and first<=h(preload,'rsp')<end and
        first<=h(active,'rsp')<end,'actual BSP stack is not the complete high 16-KiB stack')
    E.require(d(preload,'low_slots')==d(active,'low_slots')==256 and
        preload['empty']==active['empty']=='1' and active['table_readback']=='COMPLETE',
        'preactivation or actual-table no-low verification missing')
    E.require(all(high(h(preload,k),4096 if k!='tss' else 104) for k in ('gdtr','idtr','tss')),
        'BSP descriptor state is not high')
    root=h(active,'root');E.require(root and not root&4095 and root<1<<32,'main physical root')
    ready=records(lines,'[CPU_STATE][READY] ');online=records(lines,'[SMP][CPU] ONLINE ')
    E.require(len(ready)==len(online)==cpus and {d(r,'slot') for r in ready}==set(range(cpus)),
        'local CPU readback coverage')
    topology={d(r,'slot'):d(r,'apic') for r in ready}
    E.require(len(set(topology.values()))==cpus,'duplicate architectural CPU identity')
    for row in ready:
        E.require(h(row,'cr3')==root and h(row,'cr0')==0x80010033 and h(row,'cr4')==0x20 and
            h(row,'efer') in (0xd00,0x500) and h(row,'pat')==0x0007010600070406,
            'actual CPU root/control readback differs')
    E.require({(d(r,'slot'),d(r,'apic'),r['role']) for r in online}==
        {(d(r,'slot'),d(r,'apic'),r['role']) for r in ready},'ONLINE identity has no matching local root')
    bsp=[d(r,'slot') for r in ready if r['role']=='BSP'];E.require(len(bsp)==1,'BSP identity missing')
    expected=set(range(cpus))-{bsp[0]}
    created=records(lines,'[AP_ROOT] CREATED ');departed=records(lines,'[AP_ROOT] DEPART ')
    retired=records(lines,'[AP_ROOT] RETIRED ');absent=records(lines,'[AP_ROOT] NOT_NEEDED ')
    if cpus==1:
        E.require(not created and not departed and not retired and len(absent)==1 and
            d(absent[0],'private')==d(absent[0],'freed')==0,'SMP1 allocated or fabricated bootstrap retirement')
        return {'status':'PASS','root':root,'cpus':cpus,'topology':topology,'bsp':bsp[0],
            'private_allocated':0,'private_freed':0,'bootstrap':'NOT_NEEDED','departures':[]}
    E.require(len(created)==len(retired)==1 and not absent and len(departed)==cpus-1,'bootstrap lifetime records incomplete')
    start,endrow=created[0],retired[0];generation=d(start,'generation');temporary=h(start,'root')
    intended=sum(1<<s for s in expected);trampoline=h(start,'trampoline')
    E.require(generation>0 and 0<temporary<1<<32 and not temporary&4095 and temporary!=root and
        0<trampoline<0xa0000 and not trampoline&4095 and h(start,'main')==root and
        h(start,'intended')==intended and d(start,'private')==4 and d(start,'low_pages')==1 and start['verified']=='1',
        'private bootstrap root/claim/target set')
    E.require({d(r,'slot') for r in departed}==expected,'missing/duplicate AP departure')
    start_index=next(i for i,l in enumerate(lines) if l.startswith('[AP_ROOT] CREATED '))
    finish_index=next(i for i,l in enumerate(lines) if l.startswith('[AP_ROOT] RETIRED '))
    for row in departed:
        slot=d(row,'slot');stack=h(row,'stack')
        stack_domain=BASE<=stack<DIRECT_END-16384
        ist_domain=all(BASE+32768<=h(row,k)<DIRECT_END for k in ('ist1','ist2','ist3'))
        if STACK.enabled():
            import stack_guard_evidence as SG
            stack_domain=SG.BASE<=stack<SG.BASE+SG.CPU_SLOTS*SG.PITCH and (stack-SG.BASE)%SG.PITCH==4096
            ist_domain=all(SG.BASE<h(row,k)<SG.BASE+SG.CPU_SLOTS*SG.PITCH and
                (h(row,k)-SG.BASE)%SG.PITCH==4096+32768 for k in ('ist1','ist2','ist3'))
            E.require(len({stack//SG.PITCH,*(h(row,k)//SG.PITCH for k in ('ist1','ist2','ist3'))})==4,
                'AP boot/IST stack views overlap')
        E.require(d(row,'generation')==generation and d(row,'apic')==topology[slot] and
            h(row,'previous')==temporary and h(row,'root')==root and h(row,'cs')==8 and h(row,'tr')==0x28 and
            stack_domain and stack<=h(row,'rsp')<stack+16384 and
            all(high(h(row,k),4096 if k in ('gdtr','idtr') else 104) for k in ('gdtr','idtr','tss')) and
            ist_domain,
            'AP final CR3 precedes safe high stack/descriptor departure')
        depart_index=next(i for i,l in enumerate(lines) if l.startswith('[AP_ROOT] DEPART ') and d(fields(l,'[AP_ROOT] DEPART '),'slot')==slot)
        ready_index=next(i for i,l in enumerate(lines) if l.startswith('[CPU_STATE][READY] ') and d(fields(l,'[CPU_STATE][READY] '),'slot')==slot)
        online_index=next(i for i,l in enumerate(lines) if l.startswith('[SMP][CPU] ONLINE ') and d(fields(l,'[SMP][CPU] ONLINE '),'slot')==slot)
        E.require(start_index<ready_index<depart_index<online_index<finish_index,'AP root/departure/readiness/retirement chronology')
    E.require(d(endrow,'generation')==generation and h(endrow,'root')==temporary and
        h(endrow,'intended')==intended and d(endrow,'private')==d(endrow,'observer_live')==0 and
        d(endrow,'freed')==4 and d(endrow,'free_after')-d(endrow,'free_before')==4*4096,
        'private retirement/free/observer accounting')
    E.require(not any('already' in l.lower() and ('reserved' in l.lower() or 'used' in l.lower())
        for l in lines),'duplicate bootstrap reservation warning')
    return {'status':'PASS','root':root,'cpus':cpus,'topology':topology,'bsp':bsp[0],
        'generation':generation,'temporary_root':temporary,'trampoline':trampoline,'intended':intended,
        'private_allocated':4,'private_freed':4,'bootstrap':'RETIRED','departures':departed}

def probe(lines,elf,topology):
    row=single(lines,'[LOW_ROOT][NULL] ');names=symbols(elf)
    E.require(d(row,'generation')>0 and d(row,'slot') in topology and d(row,'fault')==1 and row['status']=='PASS' and
        h(row,'cr2')==h(row,'error')==0 and h(row,'rip')==names['null_probe_fault'] and
        h(row,'recovery')==names['null_probe_recovery'],'NULL result lacks the exact non-present supervisor read fault')
    E.require(not records(lines,'[LOW_ROOT][DESTRUCTIVE] '),'destructive read mislabeled recovery')
    return {'generation':d(row,'generation'),'slot':d(row,'slot'),'root':h(row,'root'),
        'rip':h(row,'rip'),'recovery':h(row,'recovery'),'fault':True}

def verify_run(directory):
    directory=Path(directory);result=json.loads((directory/'result.json').read_text())
    lines=(directory/'qemu-serial.log').read_text().splitlines();elf=(directory/'symbols.txt').read_text()
    match=re.search(r'-smp(\d+)',result['profile']);E.require(match,'low-domain profile topology')
    proof=boot(lines,elf,int(match[1]));rows=[json.loads(l) for l in (directory/'framed.jsonl').read_text().splitlines()]
    if result.get('production'):
        for token in ('cmd_lowmap','null_probe','null_destructive','g_bootstrap_snapshot'):
            E.require(token not in elf and token not in (directory/'compiled-strings.txt').read_text(),'production NULL test payload')
        E.require(not records(lines,'[LOW_ROOT][NULL] '),'production recovered a test fault')
        E.require(all(any(r['payload']==name and r['handler_status']==127 for r in rows)
            for name in ('lowmap','lowmap crash')),'production NULL selectors not rejected')
    else:
        positions=[i for i,r in enumerate(rows) if r['payload']=='lowmap']
        E.require(len(positions)==1,'NULL recovery workload missing/duplicate')
        index=positions[0]
        E.require(rows[index]['handler_status']==0 and index+1<len(rows) and
            rows[index+1]['payload']=='mem' and rows[index+1]['handler_status']==0,
            'NULL recovery lacks subsequent independent framed input/memory completion')
        O=module('verify-memory-observation');segment=O.FOUNDATION.transaction_segment(lines,rows[index])
        recovered=probe(segment,elf,proof['topology']);E.require(recovered['root']==proof['root'],'NULL fault on wrong active root')
        proof['probe']=recovered
        snapshots=records(segment,'[LOW_ROOT][BOOT_SNAPSHOT] ')
        E.require(len(snapshots)==proof['private_allocated'],'bootstrap snapshot lifetime coverage')
        for i,row in enumerate(snapshots):
            E.require(d(row,'index')==i and d(row,'generation')==proof['generation'] and
                row['capture']=='BEFORE_AP_LAUNCH' and h(row,'physical')<1<<32,'bootstrap snapshot origin/generation')
    if result.get('readback'):
        meta=json.loads((directory/'table-snapshot.json').read_text());raw=(directory/'table-frames.bin').read_bytes()
        E.require(E.sha(directory/'table-frames.bin')==meta['frames_sha256'],'low-root raw table binding')
        i=meta['frame_addresses'].index(proof['root']);E.require(raw[i*4096:i*4096+2048]==bytes(2048),
            'reported empty lower half contradicted by actual PML4 bytes')
        E.require(meta['bootstrap_root']==0,'observer retains a retired bootstrap root')
    return proof

LAYOUT={'schema':1,'tables':{'tables':0,'claim':64,'main':112,'count':144,'verified':148,'published':149,'size':152},
    'life':{'generation':0,'root':8,'main_root':16,'intended':24,'state':32,'count':36,'cpus':40,
        'size':4136,'cpu_stride':128,'departure':16},
    'departure':{'previous_root':0,'root':8,'rsp':16,'gdtr':24,'idtr':32,'tss':40,'ist1':48,'ist2':56,'ist3':64,
        'stack_first':72,'stack_bytes':80,'generation':88,'apic_id':96,'slot':100,'cs':104,'tr':106,'size':112},
    'snapshot':{'generation':0,'root':8,'physical':16,'bytes':4096,'count':20480,'size':24576}}

def readback(directory):
    directory=Path(directory);run=json.loads((directory/'result.json').read_text());bootproof=verify_run(directory)
    E.require(run['readback'] and run['low_bootstrap_readback'],'missing separate pre/post bootstrap diagnostic')
    names=symbols((directory/'symbols.txt').read_text());commands=[json.loads(l) for l in (directory/'commands.jsonl').read_text().splitlines()]
    launch=json.loads((directory/'launch.json').read_text());selected={c['name']:c for c in commands}
    E.require(json.loads((directory/'low-layout.stdout.log').read_text())==LAYOUT,'actual bootstrap owner layout differs')
    E.require(selected['low-layout-build']['argv']==['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
        '-DHOBBYOS_SELFTEST=1','scripts/low-readback-layout.c','-o',str(Path(selected['low-layout']['argv'][0]))] and
        selected['low-layout-build']['exit_status']==selected['low-layout']['exit_status']==0,
        'bootstrap actual layout compilation missing')
    proof={};objects={}
    for phase in ('bootstrap-before','bootstrap-retired','bootstrap-after'):
        row=json.loads((directory/(phase+'.json')).read_text());cfg=json.loads((directory/(phase+'-config.json')).read_text())
        command='bootstrap-before' if phase=='bootstrap-retired' else phase
        capture=command+'-capture.py'
        E.require(cfg['phase']==row['phase']==phase and row['layout']==cfg['layout']==LAYOUT and
            all(row[k]==cfg[k]==run[k] for k in ('candidate_id','vm_id','boot_id')) and
            row['root']==row['cr3']==bootproof['root'],'bootstrap diagnostic phase/candidate/root binding')
        E.require(E.sha(directory/capture)==E.sha(E.ROOT/'scripts/low-gdb-capture.py'),
            'bootstrap captured reader source differs')
        E.require(all(names[n]==v for n,v in cfg['symbols'].items()),'bootstrap symbol identity differs')
        c=selected[command];output=Path(cfg['output']);script=output/(command+'.gdb')
        E.require(c['exit_status']==0 and c['argv']==['timeout','240','gdb','-q','-batch','-x',str(script),launch['elf']['path']],
            'bootstrap real debugger invocation/exit')
        lines=['set pagination off','set confirm off','target remote '+str(Path(run['runtime_session'])/'r/gdb.sock')]
        if command=='bootstrap-before':lines += [f'hbreak *0x{names["paging_bootstrap_start"]:x}','continue']
        lines += [f'python exec(compile(open({json.dumps(str(output/capture))}).read(), "low-capture", "exec"), dict(LOW_CAPTURE_CONFIG={json.dumps(str(output/(command+"-config.json")))}))']
        if command=='bootstrap-before':
            lines += ['delete breakpoints',f'hbreak *0x{names["kernel_tlb_runtime_init"]:x}','continue',
                f'python exec(compile(open({json.dumps(str(output/capture))}).read(), "low-capture", "exec"), dict(LOW_CAPTURE_CONFIG={json.dumps(str(output/"bootstrap-retired-config.json"))}))','delete breakpoints']
            if __import__('stack_current').enabled():
                import stack_readback
                lines+=stack_readback.lines(json.loads((directory/'stack-lifetime-config.json').read_text()))
        lines+=['detach','quit']
        E.require((directory/(command+'.gdb')).read_text().splitlines()==lines,'bootstrap debugger uses unapproved commands')
        E.require((directory/(command+'.stdout.log')).read_text().splitlines().count(
            'LOW_ROOT_GDB '+json.dumps(row,sort_keys=True,separators=(',',':')))==1,'bootstrap raw debugger receipt missing')
        sizes={'g_bootstrap_tables':152,'g_bootstrap_life':4136,'g_bootstrap_snapshot':24576,'g_bootstrap_freed':8,
            'g_bootstrap_free_before':8,'g_bootstrap_free_after':8,'g_observer_boot_root':8,'g_kernel_root_physical':8,'g_free_frames':8}
        E.require(set(row['objects'])==set(sizes),'bootstrap object coverage')
        raw={}
        for name,size in sizes.items():
            path=directory/(phase+'-'+name+'.bin');data=path.read_bytes()
            E.require(row['objects'][name]=={'address':names[name],'size':size,'sha256':E.sha(path)} and len(data)==size,
                'bootstrap owner bytes missing or changed');raw[name]=data
        main=(directory/(phase+'-main-root.bin')).read_bytes()
        E.require(len(main)==4096 and main[:2048]==bytes(2048) and E.sha(directory/(phase+'-main-root.bin'))==row['main_sha256'],
            'actual main root lower half is not empty')
        life=raw['g_bootstrap_life']
        E.require(word(life,0)==bootproof['generation'] and word(life,8)==bootproof['temporary_root'] and
            word(life,16)==bootproof['root'] and word(life,24)==bootproof['intended'] and word(life,36,4)==bootproof['cpus'],
            'bootstrap lifetime identity/generation/intended set')
        expected_state=1 if phase=='bootstrap-before' else 3
        E.require(word(life,32,4)==expected_state,'bootstrap lifecycle state contradicts capture boundary')
        for slot,apic in bootproof['topology'].items():
            start=40+slot*128;target=slot!=bootproof['bsp']
            E.require(word(life,start,4)==apic and word(life,start+4,4)==(2 if target and expected_state==3 else 0) and
                word(life,start+8,4)==int(target and expected_state==3),'bootstrap completion/admission contradicts owner bytes')
            if target and expected_state==3:
                departure=next(r for r in bootproof['departures'] if d(r,'slot')==slot);record=life[start+16:start+128]
                for key,offset in LAYOUT['departure'].items():
                    if key=='size':continue
                    alias={'previous_root':'previous','stack_first':'stack','apic_id':'apic'}.get(key,key)
                    expected=16384 if key=='stack_bytes' else d(departure,alias) if key in ('generation','slot','apic_id') else h(departure,alias)
                    if key=='apic_id':expected=d(departure,'apic')
                    size=2 if key in ('cs','tr') else 4 if key in ('slot','apic_id') else 8
                    E.require(word(record,offset,size)==expected,'actual AP departure differs from serial receipt: '+key)
        proof[phase]=row;objects[phase]=raw
    before=proof['bootstrap-before'];retired=proof['bootstrap-retired'];b=objects['bootstrap-before']['g_bootstrap_tables']
    E.require(before['rip']==names['paging_bootstrap_start'] and retired['rip']==names['kernel_tlb_runtime_init'] and
        all(names['bsp_stack_bottom']<=proof[p]['rsp']<names['bsp_stack_top'] for p in ('bootstrap-before','bootstrap-retired')),
        'pre/post bootstrap capture did not stop at the high BSP handoff')
    E.require(word(b,144,4)==4 and b[148:150]==b'\x01\x01','bootstrap private publication/verification')
    addresses=[word(b,i*16+8) for i in range(4)];trampoline=word(b,64)
    E.require(addresses==before['private_addresses'] and addresses[0]==bootproof['temporary_root'] and
        len(set(addresses))==4 and all(0<a<1<<32 and not a&4095 for a in addresses) and
        all(word(b,i*16)==BASE+a for i,a in enumerate(addresses)), 'bootstrap table ownership/low root constraint')
    E.require(trampoline==before['trampoline']==bootproof['trampoline'] and trampoline not in addresses and
        word(b,72)==4096 and word(b,80)==bootproof['generation'] and word(b,104,4)==7 and word(b,108,4)==1 and
        word(b,88)<=trampoline and trampoline+4096<=word(b,96),'single conventional reserved trampoline claim')
    data=(directory/'bootstrap-before-private.bin').read_bytes();main=(directory/'bootstrap-before-main-root.bin').read_bytes()
    E.require(len(data)==16384 and E.sha(directory/'bootstrap-before-private.bin')==before['private_sha256'],
        'missing independent private physical table bytes')
    for index in range(4):
        page=data[index*4096:(index+1)*4096]
        for slot,(entry,) in enumerate(struct.iter_unpack('<Q',page)):
            if index==0 and slot>=256:expected=word(main,slot*8)
            elif index<3 and slot==0:expected=addresses[index+1]|3
            elif index==3 and slot==trampoline//4096:expected=trampoline|3
            else:expected=0
            E.require((entry^expected)&~0x60==0 and (entry==0 if expected==0 else True),
                'bootstrap low map is broader than the exact trampoline page or shared high links differ')
    snapshot=objects['bootstrap-before']['g_bootstrap_snapshot']
    E.require(word(snapshot,0)==bootproof['generation'] and word(snapshot,8)==addresses[0] and
        [word(snapshot,16+i*8) for i in range(4)]==addresses and word(snapshot,20480,4)==4 and
        snapshot[4096:20480]==data,'guest saved snapshot differs from independently read pre-launch physical bytes')
    summaries=records((directory/'qemu-serial.log').read_text().splitlines(),'[LOW_ROOT][BOOT_SNAPSHOT] ')
    E.require(len(summaries)==4 and all(h(r,'physical')==addresses[i] and h(r,'hash')==table_hash(data[i*4096:(i+1)*4096])
        for i,r in enumerate(summaries)),'bootstrap snapshot hash differs from original physical bytes')
    for phase in ('bootstrap-retired','bootstrap-after'):
        raw=objects[phase];tables=raw['g_bootstrap_tables']
        E.require(tables[:64]==bytes(64) and word(tables,112)==word(tables,144,4)==0 and not tables[148] and
            word(raw['g_observer_boot_root'],0)==0 and word(raw['g_bootstrap_freed'],0)==4 and
            word(raw['g_bootstrap_free_after'],0)-word(raw['g_bootstrap_free_before'],0)==16384 and
            raw['g_bootstrap_snapshot']==snapshot,'retired table owners/observer/free accounting or immutable snapshot differs')
    E.require(word(objects['bootstrap-retired']['g_free_frames'],0)*4096==
        word(objects['bootstrap-retired']['g_bootstrap_free_after'],0),'post-retirement actual PMM free count differs')
    for phase,private_value in (('bootstrap-before',1),('bootstrap-retired',0)):
        bits=proof[phase]['allocation_bits'];E.require([r['physical'] for r in bits]==addresses+[trampoline],
            'bootstrap PMM actual bitmap sample coverage')
        for row in bits:
            index=row['physical']//4096;is_trampoline=row['physical']==trampoline
            for name,fact in row['bits'].items():
                E.require(name in ('g_bitmap_buffer','g_reserved_bitmap_buffer') and BASE<=fact['pointer']<DIRECT_END and
                    fact['byte_address']==fact['pointer']+index//8 and 0<=fact['byte']<=255 and
                    fact['value']==(fact['byte']>>(index%8))&1 and fact['value']==
                    (1 if is_trampoline else private_value if name=='g_bitmap_buffer' else 0),
                    'bootstrap PMM page not released exactly or trampoline reservation changed')
    blob=(directory/'bootstrap-before-trampoline.bin').read_bytes()
    E.require(len(blob)==4096 and E.sha(directory/'bootstrap-before-trampoline.bin')==before['trampoline_sha256'] and
        word(blob,names['smp_trampoline_cr3']-names['_trampoline_start'])==addresses[0],
        'actual trampoline parameter has wrong/truncated physical root')
    return {'status':'PASS','physical_private_tables':4,'low_pages':1,'main_low_slots_empty':256,
        'freed_private_pages':4,'bitmap_ownership':'ALLOCATED_THEN_FREE','shared_high':'PRESERVED',
        'snapshots':'PRE_LAUNCH_PHYSICAL_BYTES_AND_IMMUTABLE_GUEST_COPY',
        'candidate_id':run['candidate_id'],'vm_id':run['vm_id']}

def destructive_records(directory,lines,topology,commands,result):
    import zlib
    elf=(directory/'symbols.txt').read_text();names=symbols(elf)
    state=boot(lines,elf,len(topology));payload='lowmap crash'
    E.require(result['status']=='FAIL' and result['cleanup'] and len(commands)==1 and
        commands[0]['payload']==payload and commands[0]['candidate_sha256']==result['candidate_elf'] and
        commands[0]['handler_status'] is None,'unguarded NULL fault returned or lacks failed transport receipt')
    row=commands[0];accept=f'[HARNESS][FRAME] ACCEPT seq=1 crc={zlib.crc32(payload.encode())&0xffffffff:08x} len={len(payload)}'
    E.require(lines.count(accept)==lines.count('[HARNESS][BEGIN] seq=1')==1 and
        not any(l.startswith('[HARNESS][END]') or l.startswith('[LOW_ROOT][NULL]') for l in lines),
        'destructive NULL framing or accidental recovery')
    injection=single(lines,'[LOW_ROOT][DESTRUCTIVE] ')
    E.require(h(injection,'site')==names['null_destructive_fault'] and
        injection['armed']=='0' and h(injection,'cr2')==h(injection,'error')==0,'unguarded NULL exact site')
    E.require(lines.count('[PANIC][REASON] text=Page Fault: NULL-address access')==1 and
        lines.count('[PANIC][DUMP_END] status=complete channel=serial')==1,'truthful NULL panic diagnostic absent')
    vector=single(lines,'[PANIC][VECTOR] ');frame=single(lines,'[PANIC][FRAME] ')
    cr2=single(lines,'[PANIC][CR2] ');error=single(lines,'[PANIC][ERROR_CODE] ')
    fault=single(lines,'[PANIC][PAGE_FAULT] ');owner=single(lines,'[PANIC][OWNER] ')
    E.require(d(vector,'known')==1 and d(vector,'value')==14 and d(frame,'available')==1 and
        h(frame,'rip')==names['null_destructive_fault'] and h(frame,'cs')==8 and
        d(cr2,'valid')==d(error,'valid')==1 and h(cr2,'value')==h(error,'value')==0 and
        set(fault)=={'present','write','user','reserved','instruction'} and all(v=='0' for v in fault.values()),
        'destructive hardware exception is not the exact non-present supervisor NULL read')
    slot=d(owner,'cpu_slot');E.require(slot in topology and d(owner,'cpu_id')==topology[slot][0] and
        d(owner,'depth')==1 and owner['origin']=='exception' and
        h(single(lines,'[PANIC][CR3] '),'value')==state['root'],'NULL panic ownership/root differs')
    E.require(row['completed_monotonic']-row['dispatcher_observed_monotonic']<5,
        'destructive NULL was recognized only as host timeout')
    commands_raw=[json.loads(l) for l in (directory/'commands.jsonl').read_text().splitlines()]
    E.require(any(r['name']=='command-1' and r['exit_status']!=0 for r in commands_raw) and
        any(r['name']=='stop' and r['exit_status']==0 for r in commands_raw),'destructive attempt/cleanup chronology')
    return {'status':'EXPECTED_NULL_FAULT','root':state['root'],'rip':names['null_destructive_fault'],
        'vector':14,'cr2':0,'error':0,'slot':slot,'recovered':False,'handler_returned':False,
        'transport_classification':row['classification'],'cleanup':True}

def destructive(directory):
    return module('verify-memory-observation').verify_run(Path(directory)/'qemu-serial.log',failure_experiment='null-read')

def historical_copy(directory):
    """Replay the exact accepted copy subset without claiming a retired campaign."""
    import statistics
    directory=Path(directory);index=json.loads((directory/'index.json').read_text())
    expected={'selftest':'6fb07baa23a2ea30ce6c534090e15a1ea0208efe3c9e9a80413156a19a9ec752',
        'framebuffer-reference':'566324ca5988dc56a58c3618053e4e2c875e4a2d5545a691ee29310420d8edc7'}
    E.require(index['schema']==1 and index['archive_sha256']==
        'b98963b6426886865d61773a4a3a75f7339a8d18ec075cfe4dcc5ece9c3bcacc','historical copy origin')
    names=[r['path'] for r in index['files']]
    E.require(len(names)==len(set(names)) and set(names)==
        {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}-{'index.json'},
        'historical copy dependency inventory')
    for row in index['files']:
        E.safe(row['path']);path=directory/row['path']
        E.require(path.is_file() and not path.is_symlink() and E.sha(path)==row['sha256'] and
            path.stat().st_size==row['size'],'historical copy bytes differ')
    candidates={role:json.loads((directory/(role+'-candidate.json')).read_text()) for role in expected}
    E.require(all(E.identity(candidates[role])==identity for role,identity in expected.items()),
        'historical copy candidate identity')
    V=module('verify-device-mapping');boots=set();groups={};geometry=None
    for row in index['runs']:
        role=row['role'];E.require(role in expected,'historical copy role');E.safe(row['path'])
        path=directory/row['path'];result=json.loads((path/'result.json').read_text())
        launch=json.loads((path/'launch.json').read_text());profile=result['profile'];candidate=candidates[role]
        E.require(profile in ('q35-tcg-smp4','q35-kvm-smp4') and result['status']=='COLLECTED' and
            result['cleanup'] and not result['readback'] and not result['trace_enabled'], 'historical copy run')
        E.require(all(launch[k]==result[k] for k in ('candidate_id','launch_id','vm_id','boot_id')) and
            result['candidate_id']==expected[role] and result['candidate_elf']==candidate['artifacts']['kernel.elf']['sha256'] and
            result['candidate_image']==candidate['artifacts']['hobbyos.img']['sha256'] and
            launch['source_sha256']==candidate['source_sha256'],'historical copy run binding')
        argv=launch['argv'];accel='tcg' if '-tcg-' in profile else 'kvm'
        for flag,value in (('-machine','q35'),('-accel',accel),('-smp','4'),('-m','2G'),('-cpu','max' if accel=='tcg' else 'host')):
            E.require(argv.count(flag)==1 and argv[argv.index(flag)+1].split(',')[0]==value,'historical copy launch')
        pair=(result['vm_id'],result['boot_id']);E.require(pair not in boots,'historical copy repeated boot');boots.add(pair)
        value=V.benchmark((path/'qemu-serial.log').read_text().splitlines(),role=='framebuffer-reference')
        if geometry is None:geometry=value['geometry']
        E.require(value['geometry']==geometry,'historical copy geometry differs')
        groups.setdefault((profile,role),[]).append(value)
    medians={'q35-tcg-smp4':(8494090,8082440),'q35-kvm-smp4':(3086315,2631260)};result={}
    E.require(len(boots)==12 and len(groups)==4,'historical copy profile coverage')
    for profile,pair in medians.items():
        result[profile]={}
        for role,expected_median in zip(expected,pair):
            values=groups[(profile,role)];median=statistics.median(r['median_ns'] for r in values)
            E.require(len(values)==3 and median==expected_median,'historical copy median differs')
            result[profile][role]={'median_ns':median,'boots':values}
    return {'profiles':result,'candidate_ids':expected,'scope':'Accepted raw copy subset only; no retired full campaign replay.',
        'effective_type':'NOT_COMPUTED','uncertainty':'UNQUANTIFIED'}
