#!/usr/bin/env python3
"""Device ownership acceptance layered on the retained native memory predicates."""
import importlib.util
import hashlib
import json
from pathlib import Path
import re
import statistics
import struct
import sys
import memory_evidence as E
import device_mapping_evidence as D
import direct_map_evidence as RAM
import low_address_evidence as LOW
import kernel_image_evidence as WX
import stack_current as STACK

ROOT=Path(__file__).resolve().parent.parent
WORKLOAD_CAPTURE_COMMANDS=['phystest','mappingtest','tlbtest','cpustate','mappingtest framebuffer-copy',
          'mem','cpu','irq check','irq controllers','irq routes','irq boot',
          'tlbtest','mappingtest','cpustate','cpustate','phystest','mem','ptinspect samples','memmap']
COMMANDS=WORKLOAD_CAPTURE_COMMANDS[:11]+['mappingtest','tlbtest']+WORKLOAD_CAPTURE_COMMANDS[13:]
REFERENCE_COMMANDS=['mappingtest','mappingtest framebuffer-copy','cpustate','cpustate','cpustate',
                    'ptinspect samples','memmap','mem','cpu','irq check','irq controllers','irq routes','irq boot']
if RAM.enabled():
    COMMANDS=['rammap',*COMMANDS]
    REFERENCE_COMMANDS=['rammap',*REFERENCE_COMMANDS]
if LOW.enabled():
    COMMANDS=[COMMANDS[0],'lowmap','mem',*COMMANDS[1:]]
    REFERENCE_COMMANDS=[REFERENCE_COMMANDS[0],'lowmap','mem',*REFERENCE_COMMANDS[1:]]

if WX.enabled():
    COMMANDS=[item for command in COMMANDS for item in (['wxtest',command] if command=='tlbtest' else [command])]
if STACK.enabled():
    expanded=[];collection=0
    for command in COMMANDS:
        if command=='wxtest':
            expanded.append('stacktest' if collection==0 else 'stacktest churn');collection+=1
        expanded.append(command)
    COMMANDS=expanded

def module(name):
    spec=importlib.util.spec_from_file_location('device_verify_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def complete_command_order(commands,high=False,*,stack_peak=False):
    expected=['rammap high',*COMMANDS[1:]] if high else COMMANDS
    if stack_peak:
        import stack_guard_evidence as SG
        E.require(STACK.enabled() and not high,'stack peak requires its ordinary current profile')
        expected=SG.taskman_peak_commands(expected)
    E.require(commands==expected,'final mapping maintenance follows the last service audit')

def terminal_chronology(audits, mapping_cycles, view_records, root, targets, protection=None,stack_services=None):
    """Bind both owned lifetimes to real pre-admission and terminal service facts.

    The fixture's independent pre-admission snapshot partitions maintenance from
    fixture requests. Runtime device adoption is derived from the retained PCI
    views, not an assumed starting generation. The fixed remaining commands are
    read-only service consumers under the pinned current source contract.
    """
    T=module('verify-tlb-shootdown');n=T.n
    E.require(len(audits)==len(mapping_cycles)==2,'terminal audit/workload coverage')
    previous=0;maintenance=0;fixture=0
    for index,(data,cycles) in enumerate(zip(audits,mapping_cycles)):
        E.require(len(data['AUDIT'])==1,'terminal service checkpoint missing')
        checkpoint=data['AUDIT'][0]
        before,after=n(checkpoint,'before'),n(checkpoint,'after')
        E.require(n(checkpoint,'root')==root and n(checkpoint,'emitted_before')==previous and
            n(checkpoint,'completed_before')==before and previous<=before<after,
            'terminal service checkpoint identity/completion')
        services=data['SERVICE']
        E.require([n(r,'generation') for r in services]==list(range(previous+1,after+1)),
            'terminal service sequence incomplete/duplicate')
        for row in services:
            E.require(n(row,'root')==root and n(row,'targets')==n(row,'ack')==targets and
                not any(n(row,k) for k in ('status','missing','failed','send_failed')),
                'terminal service unsuccessful completion')
        admitted={n(r,'generation') for r in data['REQUEST']}
        stack_generations=set(stack_services or {}) & set(range(before+1,after+1))
        E.require(admitted.isdisjoint(stack_generations) and admitted|stack_generations==set(range(before+1,after+1)),
            'terminal audit has unexplained concurrent request or missing workload tail')
        expected=[]
        if index==0:
            # ECAM and BAR discovery runs in the runtime device owner after SMP
            # release. Earlier framebuffer/controller/firmware views use the
            # separately pinned boot-local path and cannot stand in for these.
            device_views=sorted((r for r in view_records if D.number(r,'origin') in (6,7)),
                                key=lambda r:D.number(r,'generation'))
            for view in device_views:
                physical=D.number(view,'physical');size=D.number(view,'bytes')
                pages=((physical&4095)+size+4095)//4096
                expected.extend([(1,physical&~4095,pages),
                                 (1,D.number(view,'va')&~4095,pages),(3,0,1)])
        E.require(len(cycles)==8 and [D.number(r,'cycle') for r in cycles]==list(range(8)),
            'terminal mapping lifetime cycle coverage')
        for cycle in cycles:
            E.require(D.number(cycle,'generation')==index+1 and D.number(cycle,'freed')==1 and
                D.number(cycle,'recovered')==1,'terminal mapping lifetime not completed')
            interval=(1,D.number(cycle,'va')&~4095,D.number(cycle,'backing')//4096)
            expected.extend([interval,interval])
        preceding=[r for r in services if n(r,'generation')<=before and n(r,'generation') not in (stack_services or {})]
        actual=[(n(r,'operation'),n(r,'first'),n(r,'pages')) for r in preceding]
        probes=[]
        if WX.enabled():
            E.require(protection is not None and len(protection['collections'])==2,'terminal protection proof missing')
            proof=protection['collections'][index];probes=[r for r in preceding if n(r,'operation')==4]
            E.require(len(probes)==1 and n(probes[0],'generation')==proof['generation'] and n(probes[0],'mapping')==0 and
                n(probes[0],'ack')==int(proof['service']['acknowledged'],0) and
                n(probes[0],'notifications')==int(proof['service']['notifications'],0),'terminal protection generation/ACK binding')
            expected.append((4,0,0))
        E.require(actual==expected and all(n(r,'mapping')==(0 if n(r,'operation')==4 else 1) for r in preceding),
            'mapping lifetimes or device prelude missing from terminal audit')
        E.require(all(n(r,'mapping')==0 for r in services if n(r,'generation')>before and n(r,'generation') not in stack_generations),
            'unexplained maintenance after workload admission')
        maintenance+=len(preceding);fixture+=len(admitted);previous=after
    return {'status':'PASS','first_generation':1,'last_generation':previous,
            'maintenance_requests':maintenance,'fixture_requests':fixture,
            'mapping_lifetimes':16,'checkpoint_source':'actual pre-admission snapshots and terminal audit'}

def production(symbols,strings):
    for name in ('mmio_mapping_acquire','mmio_origin_resolve','mmio_origin_valid','firmware_mapping_acquire',
        'owned_mapping_release','dma_buffer_allocate','pmm_alloc_constrained_frames','xhci_control_transfer',
        'usb_ep0_event','usb_hotplug_finish_root','kernel_cache_writeback','initialize_runtime_devices'):
        E.require(re.search(r'\b'+name+r'$',symbols,re.M),'production device service missing: '+name)
    for name in ('mapping_probe','mapping_fixture','cmd_mappingtest','mmio_framebuffer_reference',
        'WB_REFERENCE','[MAPPING_TRACE]','[FRAMEBUFFER_COPY]','usb_host_control_events','cpu_request_audit_'):
        E.require(name not in symbols and name not in strings,'production mapping test payload: '+name)

def benchmark(lines,reference=False):
    rows=[D.fields(l,'[FRAMEBUFFER_COPY][SAMPLE] ') for l in lines if l.startswith('[FRAMEBUFFER_COPY][SAMPLE] ')]
    E.require(len(rows)==8,'copy benchmark sample count')
    invariant=None
    for i,r in enumerate(rows):
        E.require(set(r)==set('selection warmup samples sample bytes width height stride ns checksum valid timer scope'.split()),
                  'copy benchmark field set')
        E.require(r['selection']==('WB_REFERENCE' if reference else 'WC') and
            D.number(r,'sample')==i and D.number(r,'warmup')==3 and D.number(r,'samples')==8 and
            D.number(r,'valid')==1 and D.number(r,'ns')>0 and
            r['timer']=='monotonic_hpet' and r['scope']=='swap_buffers_plus_sfence','copy benchmark execution boundary')
        width,height,stride,size=(D.number(r,k) for k in ('width','height','stride','bytes'))
        E.require(0<width<=stride and height>0 and stride*height*4==size<=16*1024*1024,'copy benchmark geometry')
        selected={k:r[k] for k in ('bytes','width','height','stride','checksum')}
        if invariant is None:invariant=selected
        E.require(selected==invariant,'copy benchmark inconsistent contents/extent')
    samples=[D.number(r,'ns') for r in rows]
    return {'selection':rows[0]['selection'],'geometry':invariant,'warmup':3,'samples_ns':samples,
            'median_ns':statistics.median(samples),'timing':'Guest monotonic HPET around unchanged swap_buffers plus SFENCE; scheduling and emulation uncertainty unquantified.'}

def topology(lines):
    result={}
    for line in lines:
        match=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if match:
            slot=int(match[1]);E.require(slot not in result,'duplicate online mapping target');result[slot]=(int(match[2]),match[3])
    return result

def independent(directory,inventory):
    proof=json.loads((directory/'device-readback.json').read_text());cfg=json.loads((directory/'capture-config.json').read_text())
    result=json.loads((directory/'result.json').read_text())
    ram=RAM.activation((directory/'qemu-serial.log').read_text().splitlines()) if RAM.enabled() else None
    E.require(proof['candidate_id']==result['candidate_id'] and proof['vm_id']==result['vm_id'] and
        proof['root']==json.loads((directory/'table-snapshot.json').read_text())['active_root'],
        'device readback candidate/root/VM')
    expected={'schema':1,'view':{'view':0,'range':40,'first_page':72,'flags':80,'source':88,'cache':96,'state':100,'origin':104,'size':112},
        'dma':{'buffer':0,'state':64,'allocated':68,'preparing':69,'size':72},
        'controller':{'dma_capabilities':16,'dma_maximum':24,'dcbaa':80,'cmd_ring_base':88,'event_ring':128,'erst':120,
            'slot_ep0_rings':928,'slot_kbd_rings':2016,'slot_lifecycle':160,'slot_retiring':672,'size':145376}}
    E.require(proof['layout']==cfg['device_layout']==expected==json.loads((directory/'device-layout.stdout.log').read_text()),
              'device actual C/private layout')
    commands=[json.loads(l) for l in (directory/'commands.jsonl').read_text().splitlines()]
    selected={r['name']:r for r in commands if r['name'] in ('device-layout-build','device-layout')}
    E.require(set(selected)=={'device-layout-build','device-layout'} and all(r['exit_status']==0 for r in selected.values()),
              'device layout compile/execution missing')
    binary=selected['device-layout-build']['argv'][-1]
    E.require(selected['device-layout-build']['argv']==['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie','scripts/device-readback-layout.c','-o',binary] and
        selected['device-layout']['argv']==[binary],'device actual layout command')
    line='DEVICE_MEMORY_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':'))
    E.require((directory/'page-readback.stdout.log').read_text().splitlines().count(line)==1,'device raw debugger result missing')
    sizes={'g_views':112*512,'g_dma':72*2048,'xhci_driver':145376};raw={}
    E.require(set(proof['objects'])==set(sizes),'device readback object coverage')
    for name,size in sizes.items():
        row=proof['objects'][name];raw[name]=(directory/(name+'-readback.bin')).read_bytes()
        E.require(row['address']==cfg['symbols'][name] and row['size']==len(raw[name])==size and
                  row['sha256']==E.sha(directory/(name+'-readback.bin')),'device raw object binding')
    table=D.walk_snapshot(directory)
    views=[];buffers={}
    for slot in range(512):
        b=raw['g_views'][slot*112:(slot+1)*112];state,origin=struct.unpack_from('<II',b,100)
        if not state:continue
        generation,index,domain,va,physical,size=struct.unpack_from('<QIIQQQ',b)
        flags=struct.unpack_from('<Q',b,80)[0]
        E.require(state==2 and index==slot and domain==1 and generation>0 and size>0,'device debugger view ownership')
        for offset in sorted({0,size-1,*range(4096-(va&4095),size,4096)}):
            E.require(table(va+offset)==physical+offset,'device debugger full mapping continuity')
        views.append({'slot':slot,'generation':generation,'virtual':va,'physical':physical,'bytes':size,'flags':flags,'origin':origin})
    for slot in range(2048):
        b=raw['g_dma'][slot*72:(slot+1)*72];state=struct.unpack_from('<I',b,64)[0]
        if not b[68]:continue
        generation,index,domain,cpu,pa,maximum,requested,size,alignment=struct.unpack_from('<QII6Q',b)
        E.require(not b[69] and state in (0,1,2,3) and index==slot and domain==2 and generation>0 and
            0<requested<=size and size%4096==0 and pa+size-1<=maximum and pa+size<=(ram['ceiling'] if ram else 1<<32) and
            cpu==pa+(RAM.BASE if ram else 0) and
            alignment>0 and alignment&(alignment-1)==0 and pa%max(4096,alignment)==0,'device debugger DMA ownership/limit')
        E.require(table(cpu)==pa and table(cpu+size-1)==pa+size-1,'device debugger CPU/bus roundtrip')
        buffers[cpu]=(pa,size,state)
    control=raw['xhci_driver'];cap=struct.unpack_from('<I',control,16)[0];maximum=struct.unpack_from('<Q',control,24)[0]
    E.require(maximum==((1<<64)-1 if cap&1 else 0xffffffff),'device debugger AC64 constraint')
    for key in ('dcbaa','cmd_ring_base','event_ring','erst'):
        pointer=struct.unpack_from('<Q',control,expected['controller'][key])[0]
        E.require(pointer in buffers and buffers[pointer][2]==3,'controller persistent DMA object missing: '+key)
    E.require(len(views)==inventory['views'] and buffers,'device debugger live extent coverage')
    observed={D.number(r,'slot'):r for r in inventory['view_records']}
    for row in views:
        old=observed.get(row['slot'])
        E.require(old and all(row[key]==D.number(old,'va' if key=='virtual' else key)
            for key in ('slot','generation','virtual','physical','bytes','flags','origin')),
            'stopped device ownership differs from actual running inventory')
    return {'views':views,'dma_objects':len(buffers),'controller_capabilities':cap,'maximum':maximum,
        'authority':'Stopped read-only address/ownership/table corroboration; no TLB-content inference.'}

def verify_run(directory,reference=False,*,stack_peak=False):
    directory=Path(directory);lines=(directory/'qemu-serial.log').read_text().splitlines()
    current=json.loads((directory/'candidate.json').read_text());result=json.loads((directory/'result.json').read_text())
    is_reference='FRAMEBUFFER_REFERENCE=1' in current['profile']
    ram=RAM.activation(lines) if RAM.enabled() else None
    if LOW.enabled():LOW.verify_run(directory)
    E.require(is_reference==reference and not(reference and result.get('production')),'framebuffer reference role mislabeled')
    inherited=module('verify-cpu-control-state').verify_run(directory) if reference else module('verify-physical-translation').verify_run(directory)
    rows=[json.loads(l) for l in (directory/'framed.jsonl').read_text().splitlines()]
    offsets=[];offset=0
    for raw in (directory/'framed.jsonl').read_bytes().splitlines(keepends=True):
        offsets.append((offset,raw));offset+=len(raw)
    outputs=[r for r in map(json.loads,(directory/'commands.jsonl').read_text().splitlines())
             if re.fullmatch(r'command-[1-9][0-9]*',r['name'])]
    E.require(len(outputs)==len(rows),'command stdout coverage')
    for index,(command,(offset,raw)) in enumerate(zip(outputs,offsets),1):
        E.require(command['name']==f'command-{index}','command stdout order')
        path=directory/(command['name']+'.stdout.log')
        if 'stdout_evidence' in command:
            E.require(command['stdout_evidence']=={'codec':'exact-framed-range','path':'framed.jsonl',
                'offset':offset,'size':len(raw),'sha256':hashlib.sha256(raw).hexdigest()} and
                0<len(raw)<=64*1024*1024 and not path.exists(),'canonical command stdout reference')
        else:E.require(path.read_bytes()==raw,'original command stdout/transaction differs')
    if result.get('production'):
        production((directory/'symbols.txt').read_text(),(directory/'compiled-strings.txt').read_text())
        E.require(any(r['payload']=='mappingtest' and r['handler_status']==127 for r in rows),'production mapping command rejection')
        owners=re.findall(r'^\[BOOT\]\[DEVICE_OWNER\] id=([1-9][0-9]*) generation=([1-9][0-9]*)$',
            '\n'.join(lines),re.M)
        E.require(len(owners)==1,'production device owner record missing/duplicate')
        owner,lifecycle=owners[0]
        retired=f'[REAPER] FREE id={owner}'
        E.require(len(re.findall(r'^'+re.escape(retired)+r' generation=[1-9][0-9]* reason=NORMAL stack_bytes=16384$',
            '\n'.join(lines),re.M))==1,'production initialization worker retirement missing/duplicate')
        wait=json.loads((directory/'device-worker-retired.json').read_text())
        commands=[json.loads(l) for l in (directory/'commands.jsonl').read_text().splitlines()]
        terminal=[r for r in commands if r['name']=='device-worker-retired']
        E.require(wait['status']=='PASS' and wait['marker']==retired and len(terminal)==1 and
            terminal[0]['exit_status']==0 and terminal[0]['end_ns']<=outputs[0]['start_ns'],
            'production commands precede device worker retirement')
        import device_production_readback
        return {'status':'PASS','production':True,'inherited':inherited,
                'readback':device_production_readback.verify(directory)}
    commands=[r['payload'] for r in rows]
    expected=['rammap high',*COMMANDS[1:]] if RAM.enabled() and result.get('ram_profile','normal')!='normal' else COMMANDS
    if stack_peak:
        import stack_guard_evidence as SG
        E.require(STACK.enabled() and not reference and result['profile'] in
            ('q35-tcg-smp4','q35-kvm-smp4'),'stack TASKMAN peak profile')
        expected=SG.taskman_peak_commands(expected)
    E.require(commands==REFERENCE_COMMANDS if reference else commands in ((expected,) if RAM.enabled() else (COMMANDS,WORKLOAD_CAPTURE_COMMANDS)),
        'current mapping workload commands')
    O=module('verify-memory-observation');blocks=D.split_traces((directory/'qemu-debugcon.log').read_bytes())['MAPPING']
    E.require(len(blocks)==(1 if reference else 2),'mapping repeated inventory coverage')
    root=inherited['root'] if reference else inherited['invalidation']['policy']['root']
    mapped=[];samples=None
    for row in rows:
        if row['payload'] not in ('mappingtest','mappingtest framebuffer-copy'):continue
        segment=O.FOUNDATION.transaction_segment(lines,row)
        if row['payload']=='mappingtest':
            symbols={s[2]:int(s[0],16) for l in (directory/'symbols.txt').read_text().splitlines()
                     if len(s:=l.split())==3 and s[2] in ('mapping_probe_fault','mapping_probe_recovery')}
            E.require(set(symbols)=={'mapping_probe_fault','mapping_probe_recovery'},'exact probe ELF sites missing')
            for line in segment:
                if line.startswith('[MAPPING][UNMAP] '):
                    probe=D.fields(line,'[MAPPING][UNMAP] ')
                    E.require(D.number(probe,'site')==symbols['mapping_probe_fault'] and
                        D.number(probe,'recovery')==symbols['mapping_probe_recovery'],'probe site differs from current ELF')
            mapped.append(D.verify_mapping(blocks[len(mapped)],segment,root,topology(lines),reference,
                usb_controller=not result['profile'].startswith('pc-'),ram=ram))
        else:samples=benchmark(segment,reference)
    if not reference:
        E.require(mapped[1]['generation']==mapped[0]['generation']+1 and
            mapped[0]['views']==mapped[1]['views'] and mapped[0]['pages']==mapped[1]['pages'] and
            mapped[0]['roots']==mapped[1]['roots'] and mapped[0]['hubs']==mapped[1]['hubs'] and
            mapped[0]['children']==mapped[1]['children'],'mapping/USB stable ownership across traffic')
    hub=result.get('device_profile')=='hub-keyboard'
    if hub:
        monitor=(directory/'usb-topology.stdout.log').read_text()
        E.require(len(re.findall(r'Port 1, Speed 12 Mb/s, Product QEMU USB Hub, ID: testhub',monitor))==1 and
            len(re.findall(r'Port 1\.1, Speed 12 Mb/s, Product QEMU USB Keyboard',monitor))==1 and
            len(re.findall(r'Product QEMU USB Keyboard',monitor))==1,'actual sole hub keyboard monitor topology')
        for m in mapped:
            E.require(len(m['roots'])==len(m['hubs'])==len(m['children'])==1 and
                m['roots'][0]['slot']==m['hubs'][0]['slot']==m['children'][0]['parent'] and
                m['roots'][0]['slot']!=m['children'][0]['slot'],'hub root/child owner overwritten')
        E.require(D.number(mapped[-1]['usb'],'polls')>D.number(mapped[0]['usb'],'polls')>0,
                  'hub polling did not naturally run between real input observations')
    ram_result=RAM.verify_directory(directory,mapped) if RAM.enabled() else None
    independent_result=independent(directory,mapped[-1]) if result['readback'] else None
    return {'status':'PASS','production':False,'reference':reference,'inherited':inherited,'mapping':mapped,
        'ram_direct_map':ram_result,'benchmark':samples,'hub_keyboard':hub,'independent':independent_result,
        'request_audit_coverage':'ORDERED_WORKLOAD_TERMINAL_UNVERIFIED' if commands==COMMANDS else
            'REFERENCE_MAPPING_ONLY' if reference else 'FINAL_MAPPING_TAIL_NOT_CAPTURED'}

def verify_complete_run(directory,reference=False,*,stack_peak=False):
    result=verify_run(directory,reference,stack_peak=stack_peak)
    if not reference and not result['production']:
        commands=[json.loads(line)['payload'] for line in (directory/'framed.jsonl').read_text().splitlines()]
        raw=json.loads((directory/'result.json').read_text())
        complete_command_order(commands,RAM.enabled() and raw.get('ram_profile','normal')!='normal',stack_peak=stack_peak)
        T=module('verify-tlb-shootdown');O=module('verify-memory-observation')
        serial=(directory/'qemu-serial.log').read_text().splitlines()
        transactions=[json.loads(line) for line in (directory/'framed.jsonl').read_text().splitlines()]
        blocks=T.trace_blocks(directory);audits=[];cycles=[]
        for transaction in transactions:
            if transaction['payload']=='mappingtest':
                segment=O.FOUNDATION.transaction_segment(serial,transaction)
                cycles.append([D.fields(line,'[MAPPING][UNMAP] ') for line in segment
                               if line.startswith('[MAPPING][UNMAP] ')])
            if transaction['payload']=='tlbtest':
                segment=O.FOUNDATION.transaction_segment(serial,transaction)
                audits.append(T.rows(T.bind_trace(segment,blocks[len(audits)])))
        if WX.enabled():
            result['kernel_image']=WX.verify_directory(directory,result['ram_direct_map'])
            if raw['readback']:result['kernel_image']['independent']=WX.physical_tables(directory,RAM.activation(serial),result['ram_direct_map']['coverage'])
        stack_services=None
        if STACK.enabled():
            import stack_guard_evidence as SG
            active=RAM.activation(serial);cpus=WX.ready_topology(serial);plan=WX.elf_plan(directory)
            result['guarded_stacks']=SG.verify_directory(directory,plan,active,result['ram_direct_map']['coverage'],cpus)
            if stack_peak:
                result['guarded_stacks']['taskman_peak']=SG.taskman_peak(directory,plan,
                    result['ram_direct_map']['coverage'],cpus)
            stack_services=SG.maintenance(directory,audits,active['root'],cpus)
            result['guarded_stacks']['maintenance_requests']=len(stack_services)
            if raw['readback']:
                import stack_readback
                result['guarded_stacks']['independent_retirement']=stack_readback.verify(
                    directory,plan,active,cpus,result['ram_direct_map']['coverage'])
        result['terminal_audit']=terminal_chronology(audits,cycles,result['mapping'][0]['view_records'],
            result['inherited']['invalidation']['policy']['root'],sum(1<<slot for slot in topology(serial)),
            result.get('kernel_image'),stack_services)
        if WX.enabled():
            result['kernel_image']['live_scans']=WX.live_scans(directory,result['ram_direct_map'],audits)
        result['request_audit_coverage']='ALL_WORKLOAD_MAINTENANCE'
    return result

if __name__=='__main__':print(json.dumps(verify_run(Path(sys.argv[1])),sort_keys=True))
