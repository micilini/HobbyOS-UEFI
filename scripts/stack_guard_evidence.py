"""Independent guarded-stack predicates for the local HobbyOS QEMU campaign.

These tests validate protections in the maintainer's own operating-system kernel.
Hosted frames and destructive guest experiments retain separate authority.
"""
import binascii
import json
import re
from pathlib import Path
import memory_evidence as E
import direct_map_evidence as RAM

BASE=0xffffe00000000000
PITCH=65536
CPU_SLOTS=128
SLOTS=1664
END=BASE+SLOTS*PITCH
BSP_SLOT=1664
IMAGE=0xffffffff80000000
OWNER_KEYS='slot generation kind state task lifecycle cpu apic guard base end physical capacity owned published retirement'.split()
MAPPED={2,3,4,5}
n=RAM.number
TASKMAN_PEAK_BEFORE=['taskmantest reset-controls','taskmantest stats-reset','taskmantest heap-begin',
    'taskmantest setup 257','taskdiag summary','ps 1 128','ps 2 128','ps 3 128',
    'taskmantest anchor-reset','taskmantest auto-exit-frames 3','taskman 50',
    'taskmantest stats','taskmantest fixture-status','taskdiag check']
TASKMAN_PEAK_AFTER=['taskmantest cleanup','taskmantest heap-end','taskmantest fixture-status','taskdiag check']

def taskman_peak_commands(commands):
    E.require(commands.count('stacktest')==1,'stack TASKMAN peak requires one inventory')
    return [item for command in commands for item in
        (TASKMAN_PEAK_BEFORE+[command]+TASKMAN_PEAK_AFTER if command=='stacktest' else [command])]

def taskman_command(row,segment):
    """Exact current adapter for the inherited real 257-task scenario."""
    command=row['payload'];E.require(row['handler_status']==0,'stack TASKMAN command failed')
    def one(prefix):
        selected=[line for line in segment if line.startswith(prefix)]
        E.require(len(selected)==1,'stack TASKMAN missing/duplicate record: '+prefix)
        return dict(re.findall(r'(\w+)=(\S+)',selected[0]))
    markers={'taskmantest reset-controls':'[TASKMANTEST][CONTROLS] RESET',
        'taskmantest stats-reset':'[TASKMANTEST][STATS_RESET] PASS',
        'taskmantest anchor-reset':'[TASKMANTEST][ANCHOR_RESET] PASS',
        'taskmantest auto-exit-frames 3':'[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=3',
        'taskdiag summary':'[TASKDIAG][SUMMARY] PASS','taskdiag check':'[TASKDIAG][CHECK] PASS'}
    if command in markers:return one(markers[command])
    if command=='taskman 50':
        E.require(segment.count('[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE')==1 and
            segment.count('[MODAL] session_end OK')==1,'stack TASKMAN real modal session missing')
        return {}
    prefixes={'taskmantest heap-begin':'[TASKMANTEST][HEAP_BEGIN] PASS',
        'taskmantest heap-end':'[TASKMANTEST][HEAP] PASS','taskmantest setup 257':'[TASKMANTEST][SETUP] PASS',
        'taskmantest cleanup':'[TASKMANTEST][CLEANUP] PASS','taskmantest stats':'[TASKMANTEST][STATS]',
        'taskmantest fixture-status':'[TASKMANTEST][FIXTURE_STATUS] PASS'}
    if command.startswith('ps ') and command in TASKMAN_PEAK_BEFORE:
        result=one('[PS][SNAPSHOT]');page=int(command.split()[1])
        E.require(all(int(result[k])==v for k,v in {'page':page,'pages':3,'page_size':128,
            'offset':(page-1)*128}.items()) and (int(result['written'])==128 if page<3 else
            int(result['written'])>0),'stack TASKMAN real paginated snapshot')
        return result
    E.require(command in prefixes,'stack TASKMAN unknown command')
    result=one(prefixes[command])
    if command=='taskmantest setup 257':
        E.require(all(int(result[k])==v for k,v in {'count':257,'requested':257,'created':257,
            'present':257,'max':257,'stack_bytes_est':4210688}.items()) and int(result['total'])>=257,
            'stack TASKMAN did not create 257 actual tasks at unchanged capacity')
    elif command=='taskmantest cleanup':
        E.require(all(int(result[k])==v for k,v in {'removed':257,'handles_gone':257,'free_inflight':0}.items()) and
            result['origin']=='manual','stack TASKMAN cleanup incomplete')
    elif command=='taskmantest heap-end':
        E.require(result['baseline_used']==result['final_used'] and result['baseline_blocks']==result['final_blocks'] and
            int(result['drift'])==0,'stack TASKMAN heap did not return to baseline')
    elif command=='taskmantest fixture-status':
        E.require(int(result['max'])==257 and result['active']==result['present'] and
            int(result['active']) in (0,257) and result['gate_release']==result['cleanup']=='0',
            'stack TASKMAN fixture state inconsistent')
    elif command=='taskmantest stats':
        E.require(int(result['last_session_full_frames'])==3 and int(result['last_session_fallback_frames'])==0 and
            result['last_mode'] in ('WIDE','COMPACT') and int(result['pages'])>=2 and int(result['captured'])>=257 and
            all(int(result[k])==0 for k in ('truncated','render_failures','last_session_scroll_delta',
                'workspace_live','stable_frame_full_clears','stale_cells','auto_exit_pending')),
            'stack TASKMAN rendering/real snapshot contract')
    return result

def taskman_peak(directory,plan,coverage,topology):
    import low_address_evidence as L
    frames=[json.loads(line) for line in (directory/'framed.jsonl').read_text().splitlines()]
    lines=(directory/'qemu-serial.log').read_text().splitlines()
    selected={};statuses=[]
    for row in frames:
        if row['payload'] not in TASKMAN_PEAK_BEFORE+TASKMAN_PEAK_AFTER:continue
        segment=lines[row['begin_line']:row['end_line']-1]
        value=taskman_command(row,segment);selected[row['payload']]=segment
        if row['payload']=='taskmantest fixture-status':statuses.append(int(value['active']))
    E.require(statuses==[257,0],'stack TASKMAN peak and cleanup were not independently observed')
    records=serial_records(directory);number=serial_number
    inventory=owners([{k:str(number(row,k)) for k in OWNER_KEYS} for kind,row in records
        if kind=='OWNER' and number(row,'collection')==1],plan,coverage,topology)
    allocations=[]
    for line in selected['taskmantest setup 257']:
        if line.startswith('[STACK_GUARD][ALLOC] '):allocations.append(L.fields(line,'[STACK_GUARD][ALLOC] '))
    created=[]
    for line in selected['taskmantest setup 257']:
        match=re.fullmatch(r'\[TASK\]\[CREATE\] id=(\d+) name=\S+ flags=0x0000000000000010 class=NORMAL source=kthread',line)
        if match:created.append(int(match[1]))
    E.require(len(created)==len(set(created))==257,'stack TASKMAN actual creation coverage')
    allocated={(number(row,'slot'),number(row,'generation')):row for row in allocations}
    E.require(len(allocated)==len(allocations),'stack TASKMAN duplicate allocation')
    retired={(number(row,'slot'),number(row,'generation')):row for kind,row in records if kind=='RETIRE'}
    identities=set();prepared=0;used=set()
    for task_id in created:
        matches=[owner for owner in inventory.values() if owner['kind']==1 and owner['task']==task_id]
        E.require(len(matches)==1,'stack TASKMAN peak task identity missing/duplicate')
        owner=matches[0];slot=owner['slot'];generation=owner['generation'];key=slot,generation
        E.require(owner['state']==5 and owner['capacity']==16384,
            'stack TASKMAN peak owner/backing missing')
        allocation=allocated.get(key)
        if allocation is None:
            E.require(CPU_SLOTS<=slot<CPU_SLOTS+8 and generation==2,
                'stack TASKMAN new mapping lacks its installation acknowledgement')
            prepared+=1
        else:
            E.require(owner['physical']==number(allocation,'physical'),'stack TASKMAN installed backing differs');used.add(key)
        identity=owner['task'],owner['lifecycle'];E.require(identity not in identities,'stack TASKMAN repeated task owner')
        identities.add(identity);reaped=retired.get((slot,generation))
        E.require(reaped is not None and all(number(reaped,k)==owner[k] for k in ('task','lifecycle','physical')),
            'stack TASKMAN owner was not safely reaped')
    E.require(used==set(allocated) and len(allocated)+prepared==257,'stack TASKMAN allocation/prepared ownership mismatch')
    final=[row for kind,row in records if kind=='OWNER' and number(row,'collection')==2]
    E.require(not any((number(row,'task'),number(row,'lifecycle')) in identities for row in final),
        'stack TASKMAN old owners remained after cleanup')
    return {'status':'PASS','actual_tasks':257,'payload_bytes':4210688,'guard_virtual_bytes':257*4096,
        'legacy_canary_bytes_per_task':512,'peak_live_tasks':sum(v['kind']==1 and v['state']==5 for v in inventory.values()),
        'reaped':257,'runtime_installations':len(allocated),'prepared_consumed':prepared,
        'taskman_frames':3,'ps_pages':3,'heap_drift':0}

def fixed_probe_ranges(elf):
    symbols={r['name']:r for r in elf.symbols()}
    code={'guard_probe_read':'480fb607b800000000c3b801000000c3',
          'guard_payload_read':'480fb607c3',
          'guard_overflow_enter':'fa4883ec4048c7042451000000ebf2'}
    present=set(code)&set(symbols)
    E.require(not present or present==set(code),'stack incomplete assembler proof role')
    if not present:
        E.require(not any(name.startswith(('guard_probe_','guard_overflow_','stack_test_')) for name in symbols),
            'stack production recovery/destructive symbol present')
        return []
    text=elf.section('.text');ranges=[]
    for name,hexadecimal in code.items():
        symbol=symbols[name];expected=bytes.fromhex(hexadecimal)
        E.require(symbol['shndx']==text['index'] and symbol['size']==len(expected),
            'stack assembler symbol size/section: '+name)
        offset=text['offset']+symbol['value']-text['addr']
        E.require(elf.data[offset:offset+len(expected)]==expected,'stack assembler opcode/ABI: '+name)
        ranges.append((offset,len(expected)))
    E.require(symbols['guard_probe_site']['value']==symbols['guard_probe_read']['value'] and
        symbols['guard_probe_recovery']['value']==symbols['guard_probe_read']['value']+10 and
        symbols['guard_overflow_step']['value']==symbols['guard_overflow_enter']['value']+1,
        'stack exact load/recovery/overflow instruction boundaries')
    return ranges

def linker_plan(names,rows):
    guard=names['bsp_stack_guard_start'];bottom=names['bsp_stack_bottom'];top=names['bsp_stack_top']
    E.require(guard%4096==0 and names['bsp_stack_guard_end']==guard+4096==bottom and
        top-bottom==16384 and top==names['_bss_end'] and names['_bss_start']<=guard,
        'stack exact BSP linker guard/payload boundary')
    borrowed=[names['g_ist'+str(i)]-IMAGE for i in (1,2,3)]
    bss=rows[5]
    E.require(len(set(borrowed))==3 and all(p%4096==0 and bss['first']<=p and p+32768<=guard-IMAGE for p in borrowed),
        'stack static IST backing extent/role')
    E.require(all(a+32768<=b or b+32768<=a for i,a in enumerate(borrowed) for b in borrowed[i+1:]),
        'stack static IST backing overlaps')
    return {'guard':[guard,guard+4096],'payload':[bottom,top],'borrowed_ist':borrowed}

def verify_native(elf,plan):
    bss=elf.section('.bss');guard,end=plan['stacks']['guard']
    E.require(bss['type']==8,'stack BSP guard has file-backed bytes')
    for symbol in elf.symbols():
        if symbol['shndx']==bss['index'] and symbol['size']:
            E.require(symbol['value']+symbol['size']<=guard or symbol['value']>=end,
                'stack declared BSP guard overlaps allocated image object: '+symbol['name'])
    for i in (1,2,3):
        symbol=elf.symbol('g_ist'+str(i))
        E.require(symbol['size']==32768 and symbol['shndx']==bss['index'],
            'stack borrowed IST object capacity/section')

def linked_intervals(plan):
    guard,end=(v-IMAGE for v in plan['stacks']['guard'])
    return [[plan['first'],guard],[end,plan['end']]]

def owners(rows,plan,coverage,topology=None):
    E.require(0<len(rows)<=1665,'stack owner inventory missing or unbounded')
    result={};extents=[];persistent=set()
    for row in rows:
        E.require(set(OWNER_KEYS)<=set(row),'stack owner fields missing')
        v={k:n(row,k) for k in OWNER_KEYS};slot=v['slot'];kind=v['kind'];state=v['state']
        E.require(slot<=BSP_SLOT and slot not in result and v['generation']>0 and state in range(1,8) and
            kind in range(1,8) and v['owned'] in (0,1) and v['published'] in (0,1),
            'stack owner identity/state/generation')
        E.require(v['physical']%4096==0 and RAM.contains(coverage,v['physical'],v['capacity']),
            'stack backing outside selected RAM')
        if slot==BSP_SLOT:
            E.require(kind==2 and v['capacity']==16384 and not v['owned'] and
                [v['guard'],v['base']]==plan['stacks']['guard'] and
                [v['base'],v['end']]==plan['stacks']['payload'] and v['physical']==v['base']-IMAGE,
                'stack BSP owner differs from actual linker payload')
        else:
            E.require(v['guard']==BASE+slot*PITCH and v['base']==v['guard']+4096 and
                v['end']==v['base']+v['capacity'] and v['end']<=v['guard']+PITCH,
                'stack owner slot/guard/payload geometry')
            if slot>=CPU_SLOTS:
                E.require(kind in (1,7) and v['capacity']==16384 and v['owned'],
                    'stack task reserve/capacity/ownership')
            else:
                E.require(kind in (3,4,5,6) and v['capacity']==(16384 if kind==3 else 32768),
                    'stack persistent reserve/capacity')
            if v['owned']:
                E.require(not any(v['physical']<b and a<v['physical']+v['capacity'] for a,b in
                    ((plan['first'],plan['end']),(plan['boot_first'],plan['boot_end']))),
                    'stack owned writable view aliases image backing')
            else:
                E.require(kind in (4,5,6) and v['physical']==plan['stacks']['borrowed_ist'][kind-4],
                    'stack unauthorized borrowed image view')
            for a,b in extents:
                E.require(v['physical']+v['capacity']<=a or v['physical']>=b,'stack backing has two owners')
            extents.append((v['physical'],v['physical']+v['capacity']))
        if kind in (2,3,4,5,6) and state==5:
            identity=v['cpu'],kind
            E.require(identity not in persistent and v['published']==1,'stack duplicate/unpublished persistent owner')
            persistent.add(identity)
            if topology is not None:
                E.require(v['cpu'] in topology and topology[v['cpu']]==v['apic'],'stack CPU/APIC owner mismatch')
        if kind==1 and state in (4,5,6,7):
            E.require(v['task']>0 and v['lifecycle']>0 and (state!=5 or v['published']==1),
                'stack task lifecycle publication absent')
        if kind==7:E.require(state==2 and not v['published'] and not v['task'] and not v['lifecycle'],
            'stack prepared owner was already published')
        result[slot]=v
    if topology is not None:
        expected={(cpu,kind) for cpu in topology for kind in (2 if cpu==0 else 3,4,5,6)}
        E.require(persistent==expected,'stack missing BSP/AP/IST owner')
    return result

def leaf(plan,row,nx,inventory):
    va,pa,size,shift,flags,write,noexec=(n(row,k) for k in ('va','physical','bytes','shift','flags','writable','nx'))
    slot=(va-BASE)//PITCH;owner=inventory.get(slot)
    E.require(owner is not None and owner['state'] in MAPPED and owner['base']<=va and
        va+size<=owner['end'] and pa==owner['physical']+va-owner['base'],
        'stack mapped guard/padding, missing owner or incorrect backing')
    E.require(shift==12 and write==1 and noexec==nx and flags&2 and flags&0x98==0,
        'stack payload effective write/NX/cache/page-size policy')
    return 'stack'

def coverage_check(mapped,inventory):
    expected=[[v['base'],v['end']] for slot,v in sorted(inventory.items()) if slot<BSP_SLOT and v['state'] in MAPPED]
    E.require(RAM.merge(mapped)==RAM.merge(expected),'stack payload missing from actual table scan')

def readback(directory,plan,coverage,active,topology,frames,*,production=False):
    """Decode fixed scalar owners independently from raw stopped guest bytes."""
    import struct
    import low_address_evidence as L
    directory=Path(directory);proof=json.loads((directory/'stack-owner-readback.json').read_text())
    run=json.loads((directory/'result.json').read_text())
    cfg=json.loads((directory/('production-capture-config.json' if production else 'capture-config.json')).read_text())
    meta=json.loads((directory/('production-memory.json' if production else 'table-snapshot.json')).read_text())
    layout={'schema':1,'records':1665,'record_bytes':400,'sequence':0,'cells':8,
        'cell_count':49,'cpu_bytes':48,'tss_bytes':104,'cpu_max':32}
    E.require(proof['schema']==1 and proof['candidate_id']==cfg['candidate_id']==run['candidate_id'] and
        proof['vm_id']==cfg['vm_id']==run['vm_id'] and proof['root']==active['root'] and
        proof['layout']==cfg['stack_layout']==layout==json.loads((directory/'stack-layout.stdout.log').read_text()) and
        proof['frames_sha256']==meta['frames_sha256'] and proof['access']=='STOPPED_READ_ONLY_ACTUAL_TABLES_AND_TSS' and
        E.sha(directory/'stack-capture.py')==E.sha(E.ROOT/'scripts/stack-gdb-capture.py'),
        'stack independent capture source/candidate/VM/layout binding')
    count=proof['cpu_count'];E.require(count==len(topology) and 0<count<=32,'stack independent CPU coverage')
    sizes={'g_stack_owners':1665*400,'g_stack_phase':4,'g_stack_count_sequence':8,
        'g_stack_allocated':8,'g_stack_retired':8,'g_stack_quarantined':8,'g_stack_high_water':4,
        'g_cpus':count*48,'g_cpu_count':4}
    E.require(set(proof['objects'])==set(sizes),'stack independent object inventory')
    objects={}
    for name,size in sizes.items():
        path=directory/('stack-owner-'+name+'.bin');raw=path.read_bytes();row=proof['objects'][name]
        E.require(row=={'address':plan['symbols'][name],'size':size,'sha256':E.sha(path)} and len(raw)==size,
            'stack independent raw scalar object: '+name);objects[name]=raw
    integer=lambda name:int.from_bytes(objects[name],'little')
    E.require(integer('g_cpu_count')==count and integer('g_stack_phase')==4 and
        integer('g_stack_count_sequence')%2==0 and integer('g_stack_quarantined')==0,
        'stack independent readiness, interrupted writer or quarantine')
    rows=[];free=0
    for slot in range(1665):
        values=struct.unpack_from('<50Q',objects['g_stack_owners'],slot*400);sequence=values[0];cells=values[1:]
        E.require(sequence%2==0 and cells[13]<8 and all(cells[i]<(1<<32) for i in range(8,14)) and
            all(cells[i] in (0,1) for i in (14,15,16)) and all(value<=255 for value in cells[17:]),
            'stack independent inconsistent or noncanonical owner scalar')
        if cells[13]==0:free+=1;continue
        indexes=dict(generation=0,guard=1,base=2,end=3,physical=4,task=5,lifecycle=6,retirement=7,
            slot=8,capacity=9,cpu=10,apic=11,kind=12,state=13,owned=14,published=16)
        row={key:str(cells[index]) for key,index in indexes.items()}
        E.require(cells[8]==slot and cells[-1]==0 and cells[13] in MAPPED,'stack independent live slot identity/state')
        rows.append(row)
    inventory=owners(rows,plan,coverage,topology)
    E.require(integer('g_stack_allocated')>=integer('g_stack_retired') and
        integer('g_stack_allocated')-integer('g_stack_retired')==len(inventory)-1 and
        integer('g_stack_high_water')>=len(inventory)-1,'stack independent allocation/retirement accounting')
    mask=((1<<active['physical_bits'])-1)&~4095
    def lookup(address):
        table=active['root'];write=True;noexec=False
        for level in (4,3,2,1):
            E.require(table in frames,'stack independent payload path missing table')
            raw=struct.unpack_from('<Q',frames[table],((address>>(12+9*(level-1)))&511)*8)[0]
            if not raw:
                return None
            E.require(raw&1 and not raw&4 and not(raw&~(mask|0xfb|(1<<63))) and
                (active['nx'] or not raw&(1<<63)),'stack independent page entry format/privilege')
            write=write and bool(raw&2);noexec=noexec or bool(raw&(1<<63))
            if level==1 or level in (2,3) and raw&128:
                span=1<<(12+9*(level-1))
                return {'physical':(raw&mask&~(span-1))+(address&(span-1)),
                    'write':write,'nx':noexec,'level':level,'raw':raw}
            E.require(level!=4 or not raw&128,'stack independent root PS')
            table=raw&mask
        raise ValueError('stack independent path depth')
    for slot,owner in inventory.items():
        E.require(lookup(owner['guard']) is None and lookup(owner['base']-1) is None,
            'stack independent guard page is present')
        for va in range(owner['base'],owner['end'],4096):
            entry=lookup(va)
            E.require(entry and entry['level']==1 and entry['physical']==owner['physical']+va-owner['base'] and
                entry['write'] and entry['nx']==bool(active['nx']) and not entry['raw']&0x98,
                'stack independent payload/backing/effective rights')
    persistent={(v['cpu'],v['kind']):v for v in inventory.values() if v['kind'] in (2,3,4,5,6)}
    children={row['slot']:row for row in proof['children']}
    E.require(len(children)==len(proof['children'])==count,'stack independent TSS inventory')
    cpus={};bsp=None
    for slot in range(count):
        raw=objects['g_cpus'][slot*48:(slot+1)*48];child=children[slot]
        apic=L.word(raw,4,4);is_bsp=raw[12];top=L.word(raw,24);tss=L.word(raw,32);gdt=L.word(raw,40)
        E.require(L.word(raw,0,4)==slot and topology[slot]==apic and is_bsp in (0,1) and L.word(raw,16,4)==3,
            'stack independent CPU owner identity/state')
        if is_bsp:E.require(bsp is None,'stack independent duplicate BSP');bsp=slot
        E.require(top==persistent[(slot,2 if is_bsp else 3)]['end'] and child['tss']==tss,
            'stack independent CPU boot top/TSS differs')
        tss_bytes=bytes.fromhex(child['bytes']);entry=lookup(tss)
        E.require(len(tss_bytes)==104 and entry and entry['physical']==child['physical'] and
            all(L.word(tss_bytes,36+i*8)==persistent[(slot,4+i)]['end'] for i in range(3)),
            'stack independent actual TSS points to an unowned/unguarded IST')
        if not is_bsp:E.require(L.word(tss_bytes,4)==top,'stack independent AP RSP0 boot top')
        if is_bsp:
            E.require(gdt==0,'stack BSP topology unexpectedly owns an AP GDT')
            gdt=plan['symbols']['g_gdt']
        cpus[slot]={'apic':apic,'tss':tss,'gdt':gdt,'top':top}
    E.require(bsp is not None,'stack independent BSP missing')
    descriptor_path=directory/'stack-descriptors.log';text=descriptor_path.read_text()
    E.require(E.sha(descriptor_path)==proof['descriptors_sha256'],'stack raw hardware descriptor bytes changed')
    chunks=re.split(r'(?m)^CPU#(\d+)\s*\n',text);hardware={}
    E.require(not chunks[0].strip() and len(chunks)==1+count*2,'stack hardware register CPU inventory')
    for index in range(1,len(chunks),2):
        cpu_index=int(chunks[index]);body=chunks[index+1]
        E.require(cpu_index not in hardware,'stack repeated hardware CPU')
        def reg(pattern):
            matches=re.findall(pattern,body,re.M);E.require(len(matches)==1,'stack hardware descriptor field missing')
            return tuple(int(value,16) for value in matches[0])
        hardware[cpu_index]={'tr':reg(r'^TR\s*=([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+).*'),
            'gdt':reg(r'^GDT=\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+).*'),
            'idt':reg(r'^IDT=\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+).*')}
    qmp=[json.loads(line) for line in (directory/'vm-identity.jsonl').read_text().splitlines()]
    requests=[r['message'] for r in qmp if r['direction']=='send']
    def answer(request):
        replies=[r['message'] for r in qmp if r['direction']=='receive' and r['message'].get('id')==request['id']]
        E.require(len(replies)==1 and 'return' in replies[0],'stack raw QMP CPU identity reply missing/duplicate')
        return replies[0]['return']
    queries=[r for r in requests if r.get('execute')=='query-cpus-fast']
    E.require(len(queries)==1,'stack raw QMP CPU inventory missing/duplicate')
    actual=[]
    for cpu in answer(queries[0]):
        query=[r for r in requests if r.get('execute')=='qom-get' and
            r.get('arguments')=={'path':cpu['qom-path'],'property':'apic-id'}]
        E.require(len(query)==1,'stack raw QMP APIC identity query missing/duplicate')
        actual.append({'cpu_index':cpu['cpu-index'],'qom_path':cpu['qom-path'],'apic_id':answer(query[0])})
    E.require(actual==cfg['cpu_identity'] and len({r['cpu_index'] for r in actual})==count and
        {r['apic_id'] for r in actual}==set(topology.values()),'stack raw QMP/owner CPU identity differs')
    identities={r['cpu_index']:r['apic_id'] for r in actual};seen=set()
    for row in proof['registers']:
        index=row['cpu_index'];apic=row['apic_id'];registers=row['registers']
        E.require(index not in seen and identities.get(index)==apic and index in hardware,'stack independent register identity')
        seen.add(index);slot=next((cpu for cpu,value in topology.items() if value==apic),None)
        E.require(slot is not None and registers['cr3']==active['root'] and registers['cr0']==0x80010033 and
            registers['cr4']==0x20 and registers['efer']==(0xd00 if active['nx'] else 0x500),
            'stack independent local controls/root')
        current=[v for v in inventory.values() if v['state']==5 and v['base']<=registers['rsp']<v['end']]
        E.require(len(current)==1 and (current[0]['kind']==1 or current[0]['cpu']==slot),
            'stack actual CPU RSP is outside a live guarded execution payload')
        hw=hardware[index]
        E.require(hw['tr']==(0x28,cpus[slot]['tss'],103) and hw['gdt'][0]==cpus[slot]['gdt'] and
            hw['idt'][0]==plan['symbols']['g_idt'] and hw['idt'][1]==4095,
            'stack actual TR/GDTR/IDTR installation differs from owner backing')
    E.require(seen==set(identities)==set(hardware),'stack hardware register coverage incomplete')
    return {'status':'PASS','owner_rows':rows,'inventory':inventory,'live_owners':len(inventory),'free_slots':free,
        'borrowed_bytes':sum(v['capacity'] for v in inventory.values() if not v['owned']),
        'owned_bytes':sum(v['capacity'] for v in inventory.values() if v['owned']),
        'actual_cpus':len(seen),'guard_pages':len(inventory),'source_sha256':E.sha(directory/'stack-capture.py')}

SERIAL_HEX={'guard','base','end','physical','root','rip','cr2','error','recovery','requested','acknowledged',
            'missing','trigger'}
def serial_number(row,key):
    value=row[key]
    E.require(re.fullmatch('[0-9a-f]+' if key in SERIAL_HEX else '[0-9]+',value),'stack serial numeric field: '+key)
    return int(value,16 if key in SERIAL_HEX else 10)

def serial_records(directory):
    result=[]
    for line in (Path(directory)/'qemu-serial.log').read_text().splitlines():
        if '[STACK_GUARD]' not in line:continue
        body,sep,crc=line.rpartition(' crc=')
        E.require(sep and re.fullmatch('[0-9a-f]{8}',crc) and binascii.crc32(body.encode())==int(crc,16),
            'stack raw serial record CRC/framing')
        m=re.fullmatch(r'\[STACK_GUARD\]\[(\w+)\] (.*)',body)
        E.require(m is not None,'stack raw serial namespace')
        fields={}
        for field in m[2].split():
            key,sep,value=field.partition('=');E.require(sep and key not in fields,'stack duplicate/missing field');fields[key]=value
        result.append((m[1],fields))
    return result

def fault(row,owner,plan,active,topology):
    number=serial_number;cpu=number(row,'actor_cpu');address=number(row,'cr2')
    E.require(cpu in topology and number(row,'actor_apic')==topology[cpu] and
        number(row,'generation')>0 and number(row,'owner_slot')==owner['slot'] and
        number(row,'owner_generation')==owner['generation'] and number(row,'owner_kind')==owner['kind'] and
        number(row,'owner_cpu')==owner['cpu'] and address in (owner['guard'],owner['base']-1) and
        number(row,'root')==active['root'] and number(row,'rip')==plan['symbols']['guard_probe_site'] and
        number(row,'recovery')==plan['symbols']['guard_probe_recovery'] and number(row,'error')==0 and
        number(row,'seen')==1 and row['status']=='PASS','stack exact hardware guard fault mismatch')
    return number(row,'generation'),address

def maintenance(directory,audits,root,topology):
    """Bind each actual stack installation/retirement to its absolute request."""
    services={}
    for audit in audits:
        for row in audit['SERVICE']:
            generation=n(row,'generation')
            E.require(generation not in services,'stack duplicate service generation across audits')
            services[generation]=row
    result={};retirements={};allocations={};targets=sum(1<<cpu for cpu in topology)
    for kind,row in serial_records(directory):
        if kind not in ('ALLOC','RETIRE'):continue
        number=serial_number;generation=number(row,'request');slot=number(row,'slot');owner=number(row,'generation')
        E.require(generation in services and generation not in result and CPU_SLOTS<=slot<SLOTS and owner>0 and
            number(row,'base')==BASE+slot*PITCH+4096 and number(row,'bytes')==16384 and
            number(row,'physical')%4096==0 and number(row,'requested')==number(row,'acknowledged')==targets and
            row['status']=='COMPLETE','stack service binding missing, duplicate or beyond terminal audit')
        service=services[generation]
        E.require(n(service,'root')==root and n(service,'operation')==1 and n(service,'mapping')==1 and
            n(service,'first')==number(row,'base') and n(service,'pages')==4 and
            n(service,'last')==number(row,'base')+3*4096 and n(service,'strategy')==1 and
            n(service,'targets')==n(service,'ack')==targets and
            all(n(service,k)==0 for k in ('status','missing','failed','send_failed')),
            'stack install/retire differs from actual acknowledged service')
        key=slot,owner
        if kind=='ALLOC':
            E.require(key not in allocations and key not in retirements,'stack old owner reused by allocation');allocations[key]=row
        else:
            E.require(key not in retirements,'stack duplicate retired owner');retirements[key]=row
            if key in allocations:
                prior=allocations[key]
                E.require(number(prior,'request')<generation and prior['physical']==row['physical'],
                    'stack retirement precedes allocation or changes backing')
        result[generation]={'kind':kind,'row':row}
    prior_by_slot={}
    for generation,event in sorted(result.items()):
        row=event['row'];slot=serial_number(row,'slot');owner=serial_number(row,'generation')
        prior=prior_by_slot.get(slot)
        if event['kind']=='ALLOC':
            E.require(prior is None or prior['kind']=='RETIRE' and
                serial_number(prior['row'],'generation')<owner,'stack VA reused before acknowledged prior retirement')
        elif prior is not None:
            E.require(prior['kind']=='ALLOC' and serial_number(prior['row'],'generation')==owner,
                'stack stale lifecycle retired a replacement owner')
        prior_by_slot[slot]=event
    for audit in audits:
        begin=audit['BEGIN'][0];workers=audit.get('WORKER_REAP',[]);cpus=n(begin,'cpus');invocation=n(begin,'invocation')
        E.require(len(workers)==(2 if cpus>1 else 0) and {n(r,'window') for r in workers}==({0,1} if cpus>1 else set()),
            'stack TLB worker retirement coverage')
        for row in workers:
            key=n(row,'slot'),n(row,'generation');retired=retirements.get(key)
            E.require(retired is not None and n(row,'invocation')==invocation and n(row,'complete')==1 and
                n(row,'task')==serial_number(retired,'task') and n(row,'lifecycle')==serial_number(retired,'lifecycle') and
                n(row,'observed_generation')>=key[1] and
                n(row,'retirement')==(serial_number(retired,'request') if n(row,'observed_generation')==key[1] else 0) and
                serial_number(retired,'request')<=n(audit['AUDIT'][0],'after'),
                'stack TLB terminal audit precedes worker retirement')
    return result

def destructive_records(directory,lines,topology,commands,result,experiment):
    """Real local guest failures, distinct from healthy-stack recovery and boots."""
    import low_address_evidence as L
    names=L.symbols((directory/'symbols.txt').read_text())
    boot=L.boot(lines,(directory/'symbols.txt').read_text(),len(topology))
    payload='stacktest '+('overflow' if experiment=='stack-overflow' else 'timeout')
    E.require(result['status']=='FAIL' and result['cleanup'] and len(commands)==1 and
        commands[0]['payload']==payload and commands[0]['candidate_sha256']==result['candidate_elf'] and
        commands[0]['handler_status'] is None,'stack destructive command returned or missing failed transport')
    accept=f'[HARNESS][FRAME] ACCEPT seq=1 crc={binascii.crc32(payload.encode()):08x} len={len(payload)}'
    E.require(lines.count(accept)==lines.count('[HARNESS][BEGIN] seq=1')==1 and
        not any(line.startswith('[HARNESS][END]') for line in lines) and
        lines.count('[PANIC][DUMP_END] status=complete channel=serial')==1,
        'stack destructive framing/panic completion')
    owner=L.single(lines,'[PANIC][OWNER] ');cpu=L.d(owner,'cpu_slot')
    E.require(cpu in topology and L.d(owner,'cpu_id')==topology[cpu][0] and
        L.d(owner,'depth')==1 and L.h(L.single(lines,'[PANIC][CR3] '),'value')==boot['root'],
        'stack panic ownership/root mismatch')
    raw_commands=[json.loads(line) for line in (directory/'commands.jsonl').read_text().splitlines()]
    E.require(any(r['name']=='command-1' and r['exit_status']!=0 for r in raw_commands) and
        any(r['name']=='stop' and r['exit_status']==0 for r in raw_commands),
        'stack destructive attempt/cleanup chronology')
    # This deadline is transport recognition, not an extension of service time.
    elapsed=commands[0]['completed_monotonic']-commands[0]['dispatcher_observed_monotonic']
    E.require(elapsed<(5 if experiment=='stack-overflow' else 15),'stack fault observed only as host timeout')
    rows=serial_records(directory);number=serial_number
    def one(kind):
        selected=[row for key,row in rows if key==kind]
        E.require(len(selected)==1,'stack destructive missing/duplicate '+kind);return selected[0]
    E.require(not any(kind in ('BEGIN','END','FAULT','CHURN') for kind,_ in rows),
        'stack destructive experiment mixed with recoverable command')
    if experiment=='stack-overflow':
        trigger=one('OVERFLOW_TRIGGER');fault=L.single(lines,'[STACK_FAULT] ')
        vector=L.single(lines,'[PANIC][VECTOR] ');frame=L.single(lines,'[PANIC][FRAME] ')
        cr2=L.single(lines,'[PANIC][CR2] ');error=L.single(lines,'[PANIC][ERROR_CODE] ')
        guard=number(trigger,'guard');base=number(trigger,'base');end=number(trigger,'end')
        E.require(number(trigger,'slot')>=CPU_SLOTS and number(trigger,'slot')<SLOTS and
            guard==BASE+number(trigger,'slot')*PITCH and base==guard+4096 and end==base+16384 and
            number(trigger,'task')>0 and number(trigger,'lifecycle')>0 and number(trigger,'generation')>0 and
            number(trigger,'trigger')==names['guard_overflow_step'] and number(trigger,'step')==64 and
            number(trigger,'recovery_armed')==0,'stack real bounded overflow trigger identity')
        E.require(L.d(vector,'known')==1 and L.d(vector,'value')==8 and L.d(frame,'available')==1 and
            L.h(frame,'cs')==8 and L.d(cr2,'valid')==L.d(error,'valid')==1 and L.h(error,'value')==0 and
            fault['classification']=='STACK_OVERFLOW_ASSOCIATED' and L.d(fault,'vector')==8 and
            fault['rip_authority']=='ABORT_UNDEFINED' and L.h(fault,'raw_rip')==L.h(frame,'rip') and
            L.h(fault,'raw_rsp')==L.h(frame,'rsp') and L.h(fault,'cr2')==L.h(cr2,'value') and
            guard<=L.h(fault,'cr2')<base and guard<=L.h(fault,'raw_rsp')<=base+64 and
            0<=L.h(fault,'raw_rsp')-L.h(fault,'cr2')<=64,'stack destructive hardware DF/guard/frame mismatch')
        for key,other in (('task','owner_task'),('lifecycle','owner_lifecycle'),('slot','slot'),('generation','generation')):
            E.require(number(trigger,key)==L.d(fault,other),'stack destructive owner identity mismatch')
        E.require(L.d(fault,'state')==5 and L.d(fault,'owner_kind')==1 and L.d(fault,'consistent')==1 and
            [L.h(fault,k) for k in ('guard','base','end')]==[guard,base,end] and
            L.d(fault,'actor_cpu')==cpu and L.d(fault,'actor_apic')==topology[cpu][0] and
            owner['origin']=='exception','stack overflow owner/publication/actor mismatch')
        first,last=L.h(fault,'handler_base'),L.h(fault,'handler_end')
        E.require(BASE<=first<BASE+CPU_SLOTS*PITCH and (first-BASE)%PITCH==4096 and
            last-first==32768 and first<=L.h(fault,'handler_rsp')<last and L.d(fault,'handler_ist1')==1 and
            (last<=guard or first>=end),'stack overflow did not use a separate healthy IST1')
        if cpu==boot['bsp']:
            E.require(first==BASE+4096,'stack BSP double-fault IST1 identity')
        else:
            departure=next(r for r in boot['departures'] if L.d(r,'slot')==cpu)
            E.require(L.h(departure,'ist1')==last,'stack AP double-fault IST1 identity')
        return {'status':'EXPECTED_STACK_DOUBLE_FAULT','vector':8,'owner_task':number(trigger,'task'),
            'owner_lifecycle':number(trigger,'lifecycle'),'guard':guard,'cr2':L.h(fault,'cr2'),
            'raw_rip':L.h(fault,'raw_rip'),'rip_authority':'ABORT_UNDEFINED','handler_ist1':[first,last],
            'recovered':False,'handler_returned':False,'cleanup':True}
    arm=one('TIMEOUT_ARM');out=one('TIMEOUT');failed=L.single(lines,'[CPU_REQUEST][FAIL] ')
    target=number(arm,'target');requested=sum(1<<cpu for cpu in topology);request=number(out,'request')
    E.require(target in topology and number(arm,'apic')==topology[target][0] and
        all(number(arm,k)==number(out,k) for k in ('slot','generation','task','lifecycle')) and
        CPU_SLOTS<=number(arm,'slot')<SLOTS and number(arm,'generation')>0 and
        number(arm,'base')==BASE+number(arm,'slot')*PITCH+4096 and number(arm,'bytes')==16384,
        'stack timeout owner/target geometry')
    E.require(request>0 and number(out,'requested')==requested and number(out,'missing')==1<<target and
        number(out,'acknowledged')==requested^(1<<target) and number(out,'result')==7 and
        1000000000<=number(out,'elapsed_ns') and number(out,'withheld')==1 and
        number(out,'late_delivery')==0 and number(out,'reuse_result')==9 and
        number(out,'retained_pages')==number(out,'absent_pages')==4 and
        number(out,'physical')%4096==0 and out['state']=='QUARANTINED' and out['status']=='EXPECTED',
        'stack timeout was not quarantined across late completion')
    E.require(L.d(failed,'generation')==request and L.d(failed,'operation')==1 and L.d(failed,'status')==7 and
        L.h(failed,'first')==number(arm,'base') and L.h(failed,'last')==number(arm,'base')+12288 and
        L.h(failed,'root')==boot['root'] and L.h(failed,'expected')==requested and
        L.h(failed,'acknowledged')==number(out,'acknowledged') and L.h(failed,'missing')==1<<target and
        L.h(failed,'failed')==L.h(failed,'send_failed')==0 and owner['origin']=='kpanic' and
        lines.count('[PANIC][REASON] text=Stack retirement quarantined pending CPU completion')==1 and
        not any(kind=='RETIRE' and number(row,'slot')==number(out,'slot') and
            number(row,'generation')==number(out,'generation') for kind,row in rows),
        'stack timeout actual service/panic/reuse mismatch')
    return {'status':'EXPECTED_STACK_QUARANTINE','request':request,'missing':1<<target,
        'retained_pages':4,'absent_pages':4,'late_delivery':'SUBMITTED','reuse':'QUARANTINED',
        'handler_returned':False,'cleanup':True}

def verify_directory(directory,plan,active,coverage,topology,*,complete=True):
    records=serial_records(directory);number=serial_number;groups=[];active_group=None;last_generation=0
    for kind,row in records:
        if kind in ('ALLOC','RETIRE'):continue
        E.require(kind in ('BEGIN','END','OWNER','SCAN','FAULT','CYCLE','BASELINE','CHURN'),
            'stack unexpected failure/destructive record in functional boot')
        if kind=='BEGIN':
            E.require(active_group is None and number(row,'collection')==len(groups)+1 and row['mode'] in ('inventory','churn'),
                'stack collection identity/framing')
            active_group={'mode':row['mode'],'collection':number(row,'collection'),'owners':{},'pending':[],
                'scans':[],'faults':[],'cycles':[],'baseline':None,'churn':None}
        else:
            E.require(active_group is not None,'stack record outside a collection')
            group=active_group
            if 'collection' in row:E.require(number(row,'collection')==group['collection'],'stack mixed collection identity')
            if kind=='OWNER':
                converted={k:str(number(row,k)) for k in OWNER_KEYS}
                E.require(number(row,'canary')==(512 if number(row,'kind')==1 else 0),'stack task canary prefix changed')
                group['pending'].append(converted)
                group['owners'][number(row,'slot')]={k:number(row,k) for k in OWNER_KEYS}
            elif kind=='SCAN':
                inventory=owners(group['pending'],plan,coverage,topology);group['pending']=[]
                stack_bytes=sum(v['capacity'] for slot,v in inventory.items() if slot<BSP_SLOT and v['state'] in MAPPED)
                E.require(number(row,'root')==active['root'] and number(row,'nx')==active['nx'] and
                    number(row,'cpus')==len(topology) and number(row,'persistent')==4*len(topology) and
                    number(row,'tasks')==sum(v['kind']==1 and v['state']==5 for v in inventory.values()) and
                    number(row,'prepared')==sum(v['kind']==7 for v in inventory.values()) and
                    number(row,'stack_bytes')==stack_bytes and
                    number(row,'image_bytes')==sum(b-a for a,b in linked_intervals(plan)) and
                    number(row,'guard_bytes')==4096 and number(row,'canary_bytes')==512 and
                    number(row,'complete')==1 and (not active['nx'] or number(row,'rwx_bytes')==0),
                    'stack inventory/table scan/capability discrepancy')
                group['scans'].append({'owners':inventory,'row':row})
            elif kind=='FAULT':
                owner=group['owners'].get(number(row,'owner_slot'))
                E.require(owner is not None,'stack fault owner not inventoried')
                generation,address=fault(row,owner,plan,active,topology)
                E.require(generation==last_generation+1,'stack guard probe duplicate/missing generation');last_generation=generation
                group['faults'].append((owner['slot'],owner['generation'],address))
            elif kind=='CYCLE':group['cycles'].append(row)
            elif kind=='BASELINE':
                E.require(group['baseline'] is None,'stack duplicate baseline');group['baseline']=row
            elif kind=='CHURN':
                E.require(group['churn'] is None,'stack duplicate churn summary');group['churn']=row
            elif kind=='END':
                E.require(row['status']=='PASS' and not group['pending'],'stack incomplete/failed collection')
                groups.append(group);active_group=None
    E.require(active_group is None and groups,'stack collection missing terminal return')
    framed=[json.loads(line) for line in (Path(directory)/'framed.jsonl').read_text().splitlines()]
    selected=[r for r in framed if r['payload'] in ('stacktest','stacktest inventory','stacktest churn')]
    E.require(len(selected)==len(groups) and all(r['handler_status']==0 for r in selected),'stack command handler did not return success')
    if complete:E.require([g['mode'] for g in groups]==['inventory','churn'],'stack complete functional command plan')
    if complete:
        import low_address_evidence as L
        lines=(Path(directory)/'qemu-serial.log').read_text().splitlines()
        first=groups[0]['scans'][0]['owners']
        persistent={(v['cpu'],v['kind']):v for v in first.values() if v['kind'] in (2,3,4,5,6)}
        for row in L.records(lines,'[AP_ROOT] DEPART '):
            cpu=L.d(row,'slot');owner=persistent[(cpu,3)]
            E.require(L.h(row,'stack')==owner['base'] and owner['base']<=L.h(row,'rsp')<owner['end'] and
                all(L.h(row,'ist'+str(i))==persistent[(cpu,3+i)]['end'] for i in (1,2,3)),
                'stack AP departure differs from owned boot/IST views')
        for row in L.records(lines,'[PHYSICAL][CPU] '):
            owner=persistent[(L.d(row,'slot'),3)]
            E.require(n(row,'stack_top')==owner['end'] and n(row,'stack_physical')==owner['physical'] and
                n(row,'stack_bytes')==owner['capacity'],'stack AP inverse differs from exact backing owner')
    totals={'faults':0,'cycles':0,'scans':0,'migrated':0}
    retired={(number(row,'slot'),number(row,'generation')):row for kind,row in records if kind=='RETIRE'}
    for group in groups:
        totals['faults']+=len(group['faults']);totals['scans']+=len(group['scans'])
        if group['mode']=='inventory':
            E.require(len(group['scans'])==1 and not group['cycles'] and not group['churn'],'stack inventory command shape')
            owner_set=group['scans'][0]['owners'];faults=group['faults']
            expected={(slot,v['generation'],edge) for slot,v in owner_set.items() if v['kind'] in (2,3,4,5,6)
                for edge in (v['guard'],v['base']-1)}
            extra=set(faults)-expected
            E.require(len(faults)==8*len(topology)+2 and expected<=set(faults) and len(extra)==2 and
                len({(slot,generation) for slot,generation,_ in extra})==1 and
                all(owner_set[slot]['kind']==1 for slot,_,_ in extra),'stack persistent and healthy-task guard probe coverage')
        else:
            summary=group['churn'];baseline=group['baseline']
            E.require(summary is not None and baseline is not None and len(group['scans'])==2 and
                len(group['faults'])==2 and summary['status']=='PASS','stack churn baseline/peak/final proof')
            for key,value in {'warmup':8,'children':256,'creators':2,'reaped':258,'creator_peak':2,
                'allocated_delta':258,'retired_delta':258,'structural_tables':54,'quarantine':0}.items():
                E.require(number(summary,key)==value,'stack churn count mismatch: '+key)
            for key in ('live','free','owned_bytes','borrowed_bytes'):
                E.require(number(summary,key)==number(baseline,key),'stack net resource growth: '+key)
            for key in ('allocated','retired'):
                E.require(number(summary,key)-number(baseline,key)==258,'stack absolute lifetime counters')
            E.require(number(summary,'reaped_during_creation')>0,'stack creators/reaper did not overlap')
            cycles=group['cycles'];seen=set();migrated=0
            for row in cycles:
                phase=row['phase'];parent=number(row,'parent');index=number(row,'index');key=phase,parent,index
                E.require(key not in seen and phase in ('warmup','stress') and parent in (0,1),'stack repeated or unknown task cycle');seen.add(key)
                identity=number(row,'slot'),number(row,'generation');retirement=retired.get(identity)
                E.require(retirement is not None and row['state']=='REAPED' and number(row,'canary')==512 and
                    number(row,'task')==number(retirement,'task') and number(row,'lifecycle')==number(retirement,'lifecycle') and
                    number(row,'physical')==number(retirement,'physical') and number(row,'observed_generation')>=identity[1],
                    'stack cycle missing actual owner retirement')
                E.require(re.fullmatch('[0-9a-f]+',row['cpus']),'stack cycle CPU mask format')
                cpus=int(row['cpus'],16);E.require(cpus>0 and cpus&~sum(1<<cpu for cpu in topology)==0,'stack cycle invalid CPU execution set')
                if phase=='stress' and not parent:migrated+=cpus.bit_count()>1
            expected={('warmup',0,i) for i in range(8)}|{('stress',0,i) for i in range(256)}|{('stress',1,i) for i in range(2)}
            E.require(seen==expected and migrated==number(summary,'migrated') and (len(topology)==1 or migrated>0),
                'stack actual lifecycle/migration coverage')
            peak,final=(s['owners'] for s in group['scans'])
            E.require(sum(v['kind']==1 for v in peak.values())>=sum(v['kind']==1 for v in final.values())+16,
                'stack peak ownership did not retain concurrent creators/children')
            E.require(len(final)==number(summary,'live') and all(v['state']==5 for v in final.values()) and
                SLOTS-sum(slot<BSP_SLOT for slot in final)==number(summary,'free') and
                sum(v['capacity'] for v in final.values() if v['owned'])==number(summary,'owned_bytes') and
                sum(v['capacity'] for v in final.values() if not v['owned'])==number(summary,'borrowed_bytes'),
                'stack final live/backing accounting')
            totals['cycles']+=266;totals['migrated']+=migrated
    return {'status':'PASS','collections':len(groups),'online_cpus':len(topology),**totals,
        'guard_enforcement':'NON_PRESENT','execute_protection':'ENFORCED' if active['nx'] else 'UNAVAILABLE_NX'}
