"""Read-only terminal accounting for native workloads of the local HobbyOS kernel.

QEMU is stopped only after the native workload has finished. Physical reads use
pmemsave, with linked symbols translated by their actual ELF load segments. The
actual main page tables independently corroborate every captured image object.
No guest call, PTE write, control change or service request is introduced.
"""
import argparse
import binascii
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import uuid
import memory_evidence as E
import memory_elf_proof as ELF
import qemu_hmp as H

PREFIX='guarded-native-service'
LAYOUT={'audit':{'mapping':248,'result':184,'size':256,'spec':0},'capacity':8192,
    'cpu':{'apic_id':4,'is_bsp':12,'size':48,'slot':0,'state':16},
    'request':{'active':440,'next_generation':6592,'owner':6600,'quarantined':6604,'size':6608},
    'result':{'acknowledged':8,'elapsed_ns':48,'failed':16,'generation':40,'missing':12,
        'notifications':32,'requested':4,'send_failed':24,'sent':20,'size':64,'status':0,'timing_schema':56}}
def role_layout(testing,telemetry):
    result=copy.deepcopy(LAYOUT)
    if not testing:result.update(audit=None,capacity=0)
    if not telemetry:result['request']={'active':440,'next_generation':1472,'owner':1480,'quarantined':1484,'size':1488}
    return result
NAMES=('g_kernel_root_physical','g_request','g_request_root','g_audit_admitted',
       'g_audit_completed','g_audit_overflow','g_audit','g_panic_in_progress','g_cpu_count','g_cpus')
def read(path):return json.loads(Path(path).read_text())
def u(data,offset=0,size=8):return int.from_bytes(data[offset:offset+size],'little')
def enabled():
    return os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks' and bool(os.environ.get('HOBBYOS_STACK_NATIVE_AUDIT'))
def reference(directory,location):
    directory=Path(directory);path=Path(location)/(PREFIX+'.json')
    row={'directory':str(directory),'start_sha256':E.sha(directory/'start.json')}
    # Runtime directories are reused by inherited collectors; the append-only
    # launch stream retains every old identity independently of that pointer.
    temporary=path.with_suffix('.pending')
    E.write(temporary,row);os.replace(temporary,path)
def extent(elf,name):
    symbol=elf.symbol(name);va,size=symbol['value'],symbol['size']
    loads=[p for p in elf.programs if p['type']==1 and p['vaddr']<=va and va+size<=p['vaddr']+p['memsz']]
    E.require(len(loads)==1 and 0<size<=8192*256 and va>=0xffffffff80000000,
        'native audit symbol outside one bounded image load')
    p=loads[0]
    return {'virtual':va,'physical':p['paddr']+va-p['vaddr'],'size':size}
def prepare(kernel,image,pid):
    root=Path(os.environ['HOBBYOS_STACK_NATIVE_AUDIT']).resolve();root.mkdir(parents=True,exist_ok=True)
    directory=root/str(uuid.uuid4());directory.mkdir()
    harness=E.manifest(harness=True);harness_id=E.identity(harness)
    inputs=root/'inputs'/harness_id;inputs.mkdir(parents=True,exist_ok=True)
    manifest=inputs/'harness.json'
    if manifest.exists():E.require(read(manifest)==harness,'native audit preserved harness changed')
    else:
        E.write(manifest,harness)
        (inputs/'reader.py').write_bytes(Path(__file__).read_bytes())
        (inputs/'layout.c').write_bytes((E.ROOT/'scripts/stack-service-layout.c').read_bytes())
    proof=ELF.capture(Path(kernel));elf=ELF.replay(proof)
    symbols={r['name'] for r in elf.symbols()};testing='g_audit' in symbols
    telemetry='g_tsc_capability' in symbols
    with tempfile.TemporaryDirectory(prefix='stack-service-layout-') as temp:
        binary=Path(temp)/'layout'
        argv=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
            *([] if testing else ['-DHOBBYOS_REQUEST_LAYOUT_NO_SELFTEST=1']),
            '-DHOBBYOS_DEBUG_ASSERT='+str(int(telemetry)),str(E.ROOT/'scripts/stack-service-layout.c'),'-o',str(binary)]
        compiled=subprocess.run(argv,text=True,capture_output=True)
        (directory/'layout-compile.log').write_text(compiled.stdout+compiled.stderr)
        E.require(compiled.returncode==0,'native audit actual layout compile')
        result=subprocess.run([str(binary)],text=True,capture_output=True)
        (directory/'layout.stdout.log').write_text(result.stdout+result.stderr)
        E.require(result.returncode==0,'native audit actual layout execution')
        layout=json.loads(result.stdout)
        E.require(layout==role_layout(testing,telemetry),'native audit actual structure layout changed')
        E.require(elf.symbol('g_request')['size']==layout['request']['size'],'native audit actual request object layout')
    names=NAMES if testing else ('g_kernel_root_physical','g_request','g_request_root','g_panic_in_progress','g_cpu_count','g_cpus')
    E.write(directory/'elf.json',proof)
    E.write(directory/'start.json',{'schema':1,'identity':directory.name,'pid':pid,
        'process_start_ticks':Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19],
        'qemu_argv':Path(f'/proc/{pid}/cmdline').read_bytes().decode().rstrip('\0').split('\0'),
        'kernel_sha256':E.sha(kernel),'image_sha256':E.sha(image),'selftest':testing,'local_cycles':telemetry,
        'build_input_sha256':E.identity(E.manifest()),'harness_sha256':harness_id,
        'objects':{name:extent(elf,name) for name in names},'layout':layout,
        'layout_source_sha256':E.sha(E.ROOT/'scripts/stack-service-layout.c'),
        'reader_sha256':E.sha(Path(__file__)),'started_ns':time.monotonic_ns()})
    with (directory.parent/'launches.jsonl').open('ab') as stream:
        stream.write(E.canonical({'identity':directory.name,'start_sha256':E.sha(directory/'start.json')}))
        stream.flush();os.fsync(stream.fileno())
    return directory
def decode(start,objects,serial):
    """Independently reconcile every immutable absolute generation and stack use."""
    layout=start['layout'];capacity=layout['capacity'];request=objects['g_request']
    admitted=u(objects['g_audit_admitted']);completed=u(objects['g_audit_completed'])
    E.require(layout==LAYOUT,
        'native audit changed finite layout/capacity')
    E.require(not u(objects['g_audit_overflow']) and 0<=completed<=admitted<=capacity,
        'native audit overflow or missing absolute prefix')
    E.require(len(objects['g_audit'])==admitted*256,'native audit missing or extra chunk')
    root=u(objects['g_kernel_root_physical']);count=u(objects['g_cpu_count'])
    panic=u(objects['g_panic_in_progress'],0,4)
    if panic and not admitted and not u(objects['g_request_root']):
        E.require('[PANIC]' in serial and not re.search(r'^\[STACK_GUARD\]\[(ALLOC|RETIRE)\]',serial,re.M),
            'native early abort unexpectedly published stack services')
        return {'requests':0,'admitted':0,'completed':0,'active':0,'owner':0,'panic':panic,'root':root,
            'acknowledgements':0,'notifications':0,'operations':{},'stack_requests':0,
            'capability':'FATAL_BEFORE_SERVICE_INITIALIZATION'}
    E.require(0<count<=32 and root==u(objects['g_request_root']) and root%4096==0 and root,
        'native audit root/topology not ready')
    cpus={};cl=layout['cpu']
    for slot in range(count):
        data=objects['g_cpus'][slot*cl['size']:(slot+1)*cl['size']]
        E.require(u(data,cl['slot'],4)==slot and u(data,cl['state'],4)==3,
            'native audit actual CPU state')
        cpus[slot]=u(data,cl['apic_id'],4)
    E.require(len(set(cpus.values()))==count,'native audit duplicate local APIC identity')
    targets=(1<<count)-1;services={};totals={'requests':completed,'acknowledgements':0,'notifications':0,'operations':{}}
    for index in range(admitted):
        generation=index+1;b=objects['g_audit'][index*256:(index+1)*256]
        E.require(u(b)==generation and u(b,8)==root and u(b,48,4)==targets and
            all(u(b,52+slot*4,4)==apic for slot,apic in cpus.items()),
            'native audit missing generation, wrong root or target identity')
        if generation>completed:continue
        r=b[184:248];operation=u(b,180,4);mapping=b[248]
        E.require(u(r,40)==generation and u(r,56)==1 and operation in (1,2,3,4) and mapping in (0,1),
            'native audit completed identity/operation')
        first,last,pages,strategy=u(b,16),u(b,24),u(b,32),u(b,40,4)
        canonical=lambda address:address<=0x7fffffffffff or address>=0xffff800000000000
        E.require((not pages and first==last==strategy==0 or pages and
            first%4096==last%4096==0 and first<=last and canonical(first) and canonical(last) and
            not (first^last)&(1<<47) and pages==(last-first)//4096+1 and strategy==(1 if pages<=64 else 2)) and
            (operation!=4 or not pages and not mapping),'native audit malformed typed operation interval')
        E.require(u(r,4,4)==targets and u(r,32,4)==u(r,20,4).bit_count() and not u(r,20,4)&~targets and
            (u(r,0,4)==0 and u(r,8,4)==targets and all(u(r,at,4)==0 for at in (12,16,24)) and
             u(r,48)<1000000000 or panic and u(r,0,4) in (4,6,7,8)),
            'native audit unsuccessful completion or notification count')
        services[generation]={'operation':operation,'mapping':mapping,'first':u(b,16),
            'last':u(b,24),'pages':u(b,32),'strategy':u(b,40,4)}
        totals['acknowledgements']+=u(r,8,4).bit_count();totals['notifications']+=u(r,32,4)
        totals['operations'][str(operation)]=totals['operations'].get(str(operation),0)+1
    import stack_guard_evidence as SG
    import low_address_evidence as L
    events={};owners={}
    for line in serial.splitlines():
        match=re.match(r'^\[STACK_GUARD\]\[(ALLOC|RETIRE)\] ',line)
        if not match:continue
        body,separator,crc=line.rpartition(' crc=')
        E.require(separator and re.fullmatch('[0-9a-f]{8}',crc) and
            binascii.crc32(body.encode())==int(crc,16),'native stack event CRC')
        row=L.fields(body,'[STACK_GUARD]['+match[1]+'] ');n=SG.serial_number
        generation=n(row,'request');slot=n(row,'slot');identity=n(row,'generation')
        E.require(generation not in events and generation in services and 128<=slot<1664 and identity>0 and
            n(row,'base')==SG.BASE+slot*SG.PITCH+4096 and n(row,'bytes')==16384 and
            n(row,'requested')==n(row,'acknowledged')==targets and row['status']=='COMPLETE',
            'native stack event missing completed service or owner')
        E.require(services[generation]=={'operation':1,'mapping':1,'first':n(row,'base'),
            'last':n(row,'base')+12288,'pages':4,'strategy':1},'native stack extent differs from raw request')
        events[generation]={'kind':match[1],'slot':slot,'generation':identity,'row':row}
    actual={generation for generation,row in services.items() if row['operation']==1 and
        row['mapping']==1 and SG.BASE<=row['first']<SG.END}
    E.require(set(events)<=actual and (panic or set(events)==actual),
        'native audit missing stack prefix, middle or tail')
    for generation,event in sorted(events.items()):
        slot=event['slot'];old=owners.get(slot)
        E.require(old is None or old['kind']!=event['kind'] and
            (old['generation']==event['generation'] if event['kind']=='RETIRE' else old['generation']<event['generation']),
            'native stack owner reused before completed retirement')
        if old and event['kind']=='RETIRE':E.require(old['row']['physical']==event['row']['physical'],'native stack backing changes during lifetime')
        owners[slot]=event
    rl=layout['request'];active=u(request,rl['active']);owner=u(request,rl['owner'],4)
    E.require(u(request,rl['next_generation'])==admitted and (panic or not request[rl['quarantined']]),
        'native audit active generation differs or quarantined')
    totals.update(stack_requests=len(events),admitted=admitted,completed=completed,
        active=active,owner=owner,panic=panic,root=root,
        capability='FATAL_COMPLETED_PREFIX' if panic else 'SELFTEST_IMMUTABLE_ABSOLUTE_GENERATIONS')
    return totals
def panic_boundary(registers,serial,site,action):
    """An exact stopped fatal action boundary, never post-reset RAM."""
    expected={'restart':1,'shutdown':2}
    def register(name):
        values=re.findall(r'\b'+name+r'=([0-9a-fA-F]+)\b',registers)
        E.require(len(values)==1,'native panic boundary register missing')
        return int(values[0],16)
    E.require(action in expected and register('RIP')==site and
        register('RDI')==expected[action] and not register('RFL')&0x200,
        'native panic boundary wrong site, action or IF')
    E.require('[PANIC][DUMP_END] status=complete channel=serial' in serial and
        re.search(r'^\[PANIC\]\[COUNTDOWN\] state=(?:expired|fallback-expired) ',serial,re.M) and
        '[PANIC][ACTION]' not in serial,'native panic boundary before dump or after action')
    return {'site':site,'action':action,'rip':register('RIP'),'rdi':register('RDI'),'if_clear':True}

def capture(directory,hmp,serial,pid,*,panic_action=None):
    directory=Path(directory);start=read(directory/'start.json')
    E.require(start['pid']==pid and start['process_start_ticks']==
        Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[19],'native audit wrong live VM')
    if not start['selftest']:
        E.write(directory/'result.json',{'status':'PRODUCTION_AUDIT_ABSENT','start_sha256':E.sha(directory/'start.json')})
        return
    commands=[]
    def command(value):
        output=H.command(str(hmp),value,10);commands.append({'command':value,'response':output});return output
    for attempt in range(16):
        command('stop');state=command('info status')
        E.require('paused' in state or 'shutdown' in state,'native audit VM did not stop')
        sample=directory/f'sample-{attempt:02d}';sample.mkdir();objects={};chunks={}
        def physical(pa,size,name):
            E.require(0<=pa<1<<52 and 0<size<=8192*256 and pa+size<=1<<52,'native audit physical read bound')
            path=sample/(name+'.bin')
            command(f'pmemsave 0x{pa:x} {size} '+json.dumps(str(path),ensure_ascii=False))
            raw=path.read_bytes();E.require(len(raw)==size,'native audit physical chunk length')
            chunks[name]={'physical':pa,'size':size,'sha256':E.sha(path)};return raw
        for name,row in start['objects'].items():
            if name=='g_audit':continue
            objects[name]=physical(row['physical'],row['size'],name)
        admitted=u(objects['g_audit_admitted'])
        E.require('shutdown' not in state or u(objects['g_panic_in_progress'],0,4),
            'native audit shutdown without panic ownership')
        E.require(admitted<=8192,'native audit bounded capture overflow')
        objects['g_audit']=physical(start['objects']['g_audit']['physical'],admitted*256,'g_audit') if admitted else b''
        serial_bytes=Path(serial).read_bytes();(sample/'serial.log').write_bytes(serial_bytes)
        # Capture controlling table paths for all symbol pages actually read.
        root=u(objects['g_kernel_root_physical']);frames={}
        early_abort=not root and u(objects['g_panic_in_progress'],0,4) and not admitted
        E.require(early_abort or root and not root&4095,'native audit main root unavailable')
        for name,data in (() if early_abort else objects.items()):
            obj=start['objects'][name];begin=obj['virtual']
            for va in range(begin&~4095,(begin+len(data)+4095)&~4095,4096):
                table=root
                for level in (4,3,2,1):
                    if table not in frames:frames[table]=physical(table,4096,'table-'+str(table))
                    value=u(frames[table],((va>>(12+9*(level-1)))&511)*8)
                    E.require(value&1 and not value&4 and (level==1 or not value&128),
                        'native audit object page path is absent, user or large')
                    table=value&0x000ffffffffff000
                E.require(table==obj['physical']-begin+va,'native audit actual table disagrees with ELF load')
        E.require(early_abort or frames[root][:2048]==bytes(2048),'native audit main root has a low mapping')
        registers=command('info registers');(sample/'registers.log').write_text(registers)
        E.require(early_abort or re.search(r'CR3=0*'+format(root,'x')+r'\b',registers,re.I),'native audit current CR3 differs')
        proof={'schema':1,'start_sha256':E.sha(directory/'start.json'),'objects':chunks,
            'table_frames':sorted(frames),'serial_sha256':E.sha(sample/'serial.log'),
            'registers_sha256':E.sha(sample/'registers.log'),'sample':sample.name,'commands':commands,
            'early_abort_without_main_root':bool(early_abort)}
        if panic_action is not None:
            site=ELF.replay(read(directory/'elf.json')).symbol('panic_do_action')['value']
            E.require(u(objects['g_panic_in_progress'],0,4),'native panic boundary lacks fatal ownership')
            proof['panic_boundary']=panic_boundary(registers,serial_bytes.decode(errors='replace'),site,panic_action)
            proof['captured_monotonic']=time.monotonic()
        E.write(sample/'capture.json',proof)
        try:totals=decode(start,objects,serial_bytes.decode(errors='replace'))
        except ValueError as error:
            if u(objects['g_panic_in_progress'],0,4):raise
            E.write(sample/'pending.json',{'reason':str(error),'authority':'Unaccepted stopped observation; all bytes retained.'})
        else:
            if totals['panic'] or totals['admitted']==totals['completed'] and not totals['active'] and not totals['owner']:
                E.write(directory/'result.json',{'status':'FATAL_PREFIX' if totals['panic'] else 'COMPLETE','start_sha256':proof['start_sha256'],
                    'sample':sample.name,'capture_sha256':E.sha(sample/'capture.json'),'totals':totals})
                return
            E.write(sample/'pending.json',{'totals':totals,'authority':'Incomplete service state; not a completed audit.'})
        command('cont');time.sleep(.05)
    raise ValueError('native terminal service audit did not reach completed ownership')
def verify(directory):
    directory=Path(directory);start=read(directory/'start.json');result=read(directory/'result.json')
    E.require(re.fullmatch('[0-9a-f]{64}',start['harness_sha256']),'native audit unsafe input identity')
    inputs=directory.parent/'inputs'/start['harness_sha256'];harness=read(inputs/'harness.json')
    captured={r['path']:r for r in harness['files']}
    E.require(start['schema']==1 and result['start_sha256']==E.sha(directory/'start.json') and
        start['build_input_sha256']==E.identity(E.manifest()) and
        E.identity(harness)==start['harness_sha256'] and
        start['reader_sha256']==E.sha(inputs/'reader.py')==captured['scripts/stack_native_audit.py']['sha256'] and
        start['layout_source_sha256']==E.sha(inputs/'layout.c')==captured['scripts/stack-service-layout.c']['sha256'],
        'native audit source/start binding')
    proof=read(directory/'elf.json');elf=ELF.replay(proof)
    E.require(proof['elf_sha256']==start['kernel_sha256'] and
        start['objects']=={name:extent(elf,name) for name in start['objects']} and
        start['layout']==json.loads((directory/'layout.stdout.log').read_text()),
        'native audit actual ELF/layout identity')
    symbols={s['name'] for s in elf.symbols()}
    E.require(start.get('local_cycles')==('g_tsc_capability' in symbols) and
        start['selftest']==('g_audit' in symbols) and start['layout']==role_layout(start['selftest'],start['local_cycles']) and
        elf.symbol('g_request')['size']==start['layout']['request']['size'], 'native audit source-bound telemetry layout')
    if not start['selftest']:
        E.require(result['status']=='PRODUCTION_AUDIT_ABSENT' and
            'g_audit' not in {s['name'] for s in elf.symbols()},'native production contains test audit')
        return {'status':'PRODUCTION_AUDIT_ABSENT'}
    E.require(result['status'] in ('COMPLETE','FATAL_PREFIX') and re.fullmatch(r'sample-\d{2}',result['sample']),
        'native audit incomplete terminal capture')
    sample=directory/result['sample'];capture=read(sample/'capture.json');objects={};frames={}
    E.require(capture['start_sha256']==E.sha(directory/'start.json') and
        E.sha(sample/'capture.json')==result['capture_sha256'] and
        E.sha(sample/'serial.log')==capture['serial_sha256'] and
        E.sha(sample/'registers.log')==capture['registers_sha256'],'native audit raw binding')
    for name,row in capture['objects'].items():
        E.safe(name);path=sample/(name+'.bin');data=path.read_bytes()
        E.require(len(data)==row['size'] and E.sha(path)==row['sha256'],'native audit missing or altered physical chunk')
        if name.startswith('table-'):
            pa=int(name[6:]);E.require(row['physical']==pa and len(data)==4096,'native audit malformed table chunk')
            frames[pa]=data
        else:
            E.require(name in start['objects'] and row['physical']==start['objects'][name]['physical'] and
                (row['size']==start['objects'][name]['size'] if name!='g_audit' else row['size']<=8192*256),
                'native audit wrong object extent')
            objects[name]=data
    if 'g_audit' not in objects and u(objects.get('g_audit_admitted',b''))==0:objects['g_audit']=b''
    E.require(set(objects)==set(start['objects']) and sorted(frames)==capture['table_frames'],
        'native audit missing object/table coverage')
    root=u(objects['g_kernel_root_physical']);visited=set()
    early_abort=not root and u(objects['g_panic_in_progress'],0,4) and not u(objects['g_audit_admitted'])
    E.require(capture['early_abort_without_main_root']==bool(early_abort),'native audit false early-abort exemption')
    for name,data in (() if early_abort else objects.items()):
        obj=start['objects'][name];begin=obj['virtual']
        for va in range(begin&~4095,(begin+len(data)+4095)&~4095,4096):
            table=root
            for level in (4,3,2,1):
                E.require(table in frames,'native audit missing table path')
                visited.add(table);entry=u(frames[table],((va>>(12+9*(level-1)))&511)*8)
                E.require(entry&1 and not entry&4 and (level==1 or not entry&128),
                    'native audit absent/user/large object path')
                table=entry&0x000ffffffffff000
            E.require(table==obj['physical']-begin+va,'native audit object aliases wrong physical frame')
    E.require(visited==set(frames) and (early_abort or re.search(r'CR3=0*'+format(root,'x')+r'\b',
        (sample/'registers.log').read_text(),re.I)),'native audit root/path mismatch')
    E.require(early_abort or frames[root][:2048]==bytes(2048),'native audit main root has a low mapping')
    if 'panic_boundary' in capture:
        site=elf.symbol('panic_do_action')['value'];boundary=capture['panic_boundary']
        E.require(u(objects['g_panic_in_progress'],0,4) and capture['captured_monotonic']>0 and
            boundary==panic_boundary((sample/'registers.log').read_text(),(sample/'serial.log').read_text(),
                site,boundary['action']),'native panic boundary raw identity differs')
    totals=decode(start,objects,(sample/'serial.log').read_text())
    E.require(totals==result['totals'] and (totals['panic'] and result['status']=='FATAL_PREFIX' or
        not totals['panic'] and result['status']=='COMPLETE' and totals['admitted']==totals['completed'] and
        not totals['active'] and not totals['owner']),'native audit terminal totals differ or unfinished')
    return totals

def verify_panic_action(directory,run):
    start=read(directory/'start.json');result=read(directory/'result.json')
    E.require(start['selftest'] and result['status']=='FATAL_PREFIX','native panic action lacks fatal audit')
    capture=read(directory/result['sample']/'capture.json')
    E.require(capture.get('panic_boundary',{}).get('action')==run['action'] and
        run['trigger_monotonic']<capture['captured_monotonic']<
        run['terminal_evidence']['event_monotonic']<run['cleanup']['started'],
        'native panic audit is missing or outside pre-action boundary')
def verify_family(directory,required=False):
    directory=Path(directory);root=directory/'service-audits'
    if not root.exists():
        E.require(not required,'native service audit launch stream missing')
        return {'launches':0,'results':[]}
    launches=[json.loads(line) for line in (root/'launches.jsonl').read_text().splitlines()]
    expected={row['identity']:row['start_sha256'] for row in launches}
    E.require(len(expected)==len(launches) and (launches or not required),
        'native service audit duplicate or missing launch')
    references=list(directory.rglob(PREFIX+'.json'));seen=set();results=[]
    for path in references:
        reference=read(path);name=Path(reference['directory']).name
        E.require(re.fullmatch(r'[0-9a-f-]{36}',name),'native audit unsafe VM identity')
        target=root/name
        E.require(E.sha(target/'start.json')==reference['start_sha256'],'native audit cross-boot start substitution')
        E.require(name in expected and reference['start_sha256']==expected[name],
            'native audit reference outside absolute launch stream')
        run_path=path.parent/'result.json'
        if run_path.is_file():
            run=read(run_path)
            if run.get('action') in ('restart','shutdown') and not run.get('production',False):
                verify_panic_action(target,run)
    for name,digest in expected.items():
        E.require(re.fullmatch(r'[0-9a-f-]{36}',name) and E.sha(root/name/'start.json')==digest,
            'native audit launch identity or missing start')
        seen.add(name);results.append({'identity':name,'facts':verify(root/name)})
    E.require(seen=={p.name for p in root.iterdir() if p.is_dir() and p.name!='inputs'},'native audit missing VM/reference/tail')
    return {'launches':len(launches),'launches_sha256':E.sha(root/'launches.jsonl'),'results':results}
def host_cases(directory):
    """Mutate actual hosted C request-state output; this is not a QEMU run."""
    directory=Path(directory);original={name:(directory/(name+'.bin')).read_bytes() for name in NAMES}
    def line(kind,request,generation=1):
        body=f'[STACK_GUARD][{kind}] slot=128 generation={generation} base=ffffe00000801000 physical=0000000000220000 bytes=16384 request={request} requested=3 acknowledged=3 status=COMPLETE'
        return body+f' crc={binascii.crc32(body.encode()):08x}\n'
    serial=line('ALLOC',1)+line('RETIRE',2);start={'layout':copy.deepcopy(LAYOUT)}
    expected=decode(start,original,serial)
    E.require(expected['requests']==2 and expected['stack_requests']==2 and
        expected['acknowledgements']==4 and expected['notifications']==2 and not expected['active'] and
        not expected['owner'] and expected['operations']=={'1':2},'native audit actual hosted C positive')
    cases=[]
    def change(objects,name,at,value,size=8):
        raw=bytearray(objects[name]);raw[at:at+size]=value.to_bytes(size,'little');objects[name]=bytes(raw)
    mutations=[
        ('missing-prefix',lambda o:change(o,'g_audit',0,2),'missing generation'),
        ('missing-tail',lambda o:o.update(g_audit=o['g_audit'][:-256]),'missing or extra chunk'),
        ('missing-middle',lambda o:change(o,'g_audit',256,3),'missing generation'),
        ('duplicate-generation',lambda o:change(o,'g_audit',256,1),'missing generation'),
        ('one-ack-missing',lambda o:change(o,'g_audit',192,1,4),'unsuccessful completion'),
        ('queued-completion',lambda o:change(o,'g_audit',184,1,4),'unsuccessful completion'),
        ('wrong-root',lambda o:change(o,'g_audit',8,0),'wrong root'),
        ('wrong-cpu',lambda o:change(o,'g_audit',52,9,4),'target identity'),
        ('wrong-interval',lambda o:change(o,'g_audit',32,5),'malformed typed operation'),
        ('wrong-notifications',lambda o:change(o,'g_audit',216,2,4),'notification count'),
        ('legacy-duration-as-current-schema',lambda o:change(o,'g_audit',240,100),'completed identity'),
        ('audit-overflow',lambda o:change(o,'g_audit_overflow',0,1,1),'overflow'),
        ('capacity-exhausted',lambda o:change(o,'g_audit_admitted',0,8193),'overflow'),
        ('generation-reused',lambda o:change(o,'g_request',LAYOUT['request']['next_generation'],1),'active generation differs'),
        ('quarantine-reused',lambda o:change(o,'g_request',LAYOUT['request']['quarantined'],1,1),'quarantined'),
    ]
    for name,mutate,reason in mutations:
        changed=copy.deepcopy(original);mutate(changed)
        try:decode(start,changed,serial)
        except ValueError as error:
            E.require(reason in str(error),'native audit host wrong boundary: '+name+': '+str(error))
            cases.append({'name':name,'reason':reason})
        else:raise ValueError('native audit hosted contradiction accepted: '+name)
        E.require(decode(start,original,serial)==expected,'native audit restored hosted positive')
    for name,changed,reason in (
        ('missing-stack-prefix',line('RETIRE',2),'missing stack prefix'),
        ('missing-stack-tail',line('ALLOC',1),'missing stack prefix'),
        ('wrong-owner',line('ALLOC',1)+line('RETIRE',2,2),'owner reused'),
        ('double-allocation',line('ALLOC',1)+line('ALLOC',2),'owner reused'),
        ('duplicate-event',line('ALLOC',1)+line('RETIRE',1),'missing completed service'),
        ('event-crc',serial.replace('crc=','crc=0',1),'event CRC')):
        try:decode(start,original,changed)
        except ValueError as error:
            E.require(reason in str(error),'native audit serial host wrong boundary: '+name+': '+str(error))
            cases.append({'name':name,'reason':reason})
        else:raise ValueError('native audit serial contradiction accepted: '+name)
    E.require(decode(start,original,serial)==expected,'native audit final hosted positive')
    site=0xffffffff8200d730
    registers=f'RIP={site:x} RDI=0000000000000001 RFL=00000046'
    fatal='[PANIC][DUMP_END] status=complete channel=serial\n[PANIC][COUNTDOWN] state=fallback-expired polls=1000000\n'
    positive=panic_boundary(registers,fatal,site,'restart')
    for name,regs,log,action,reason in (
        ('panic-reset-rip',registers.replace(f'{site:x}','fff0'),fatal,'restart','wrong site'),
        ('panic-wrong-action',registers,fatal,'shutdown','wrong site'),
        ('panic-if-enabled',registers.replace('00000046','00000246'),fatal,'restart','wrong site'),
        ('panic-before-dump',registers,'','restart','before dump'),
        ('panic-after-action',registers,fatal+'[PANIC][ACTION] requested=restart\n','restart','after action'),
        ('panic-missing-register',registers.replace('RDI=','NONE='),fatal,'restart','register missing')):
        try:panic_boundary(regs,log,site,action)
        except ValueError as error:
            E.require(reason in str(error),'native panic host wrong rejection: '+name)
            cases.append({'name':name,'reason':reason})
        else:raise ValueError('native panic boundary contradiction accepted: '+name)
        E.require(panic_boundary(registers,fatal,site,'restart')==positive,'native panic restored positive')
    return {'status':'PASS','positive':expected,'negative':cases,
        'bytes':{name:hashlib.sha256(data).hexdigest() for name,data in original.items()},
        'authority':'ACTUAL_HOSTED_REQUEST_STATE_WITH_SYNTHETIC_CPU_AND_SERIAL_INPUTS; NOT_GUEST_EXECUTION'}

NATIVE_NEGATIVES={
    **{'native-missing-'+where:'native audit missing generation, wrong root or target identity'
        for where in ('prefix','middle','tail')},
    'native-overflow':'native audit overflow or missing absolute prefix',
    'native-missing-ack':'native audit unsuccessful completion or notification count',
    'native-active-terminal':'native audit terminal totals differ or unfinished',
    'native-wrong-apic':'native audit missing generation, wrong root or target identity',
    **{'native-event-'+where:'native audit missing stack prefix, middle or tail'
        for where in ('prefix','middle','tail')},
    'native-table-chunk-missing':'native audit missing table path',
    'native-table-wrong-frame':'native audit object aliases wrong physical frame',
    'native-cross-boot-elf':'native audit actual ELF/layout identity',
    'native-duplicate-launch':'native service audit duplicate or missing launch',
    'native-launch-tail-removed':'native audit terminal launch inventory differs',
    'native-launch-cross-boot':'native audit launch identity or missing start',
}

def verify_fixture_receipt(row):
    E.require(row['status']=='PASS' and row['authority']=='SYNTHETIC_MUTATIONS_OF_CURRENT_NATIVE_PHYSICAL_CAPTURES' and
        row['base'] and row['originals_rechecked']>0 and row['restored_positives']==len(NATIVE_NEGATIVES) and
        {v['name']:v['expected_boundary'] for v in row['cases']}==NATIVE_NEGATIVES and
        len(row['cases'])==len(NATIVE_NEGATIVES),'native audit fixture inventory incomplete')
    for case in row['cases']:
        E.require(case['synthetic'] and case['actual_exit']==1 and
            case['expected_boundary'] in case['actual_reason'],'native audit fixture rejected at unrelated boundary')

def fixture_cases(campaign):
    """Mutate bounded copies after actual native families have been collected."""
    selected=read(campaign/'regression-selection.json');bases=[]
    for relative in selected.values():
        root=campaign/relative/'evidence/service-audits'
        if not root.exists():continue
        for item in read_lines(root/'launches.jsonl'):
            directory=root/item['identity'];result=read(directory/'result.json')
            if result['status']=='COMPLETE' and result['totals']['stack_requests']>=3:
                verify(directory);bases.append(directory)
                if len(bases)==2:break
        if len(bases)==2:break
    E.require(len(bases)==2,'native audit fixtures need two actual completed stack workloads')
    originals={p:(E.sha(p),p.stat().st_size,p.stat().st_mode&0o777) for base in bases
        for p in [*base.rglob('*'),*(base.parent/'inputs'/read(base/'start.json')['harness_sha256']).rglob('*')] if p.is_file()}
    cases=[]
    with tempfile.TemporaryDirectory(prefix='stack-native-negative-') as temporary:
        family=Path(temporary);root=family/'service-audits';root.mkdir();launches=[]
        for base in bases:
            shutil.copytree(base,root/base.name)
            identity=read(base/'start.json')['harness_sha256'];target=root/'inputs'/identity
            if not target.exists():shutil.copytree(base.parent/'inputs'/identity,target)
            launches.append({'identity':base.name,'start_sha256':E.sha(base/'start.json')})
        (root/'launches.jsonl').write_bytes(b''.join(E.canonical(row) for row in launches))
        expected=verify_family(family,True);work=root/bases[0].name;positive=verify(work)
        saved={p.relative_to(family):p.read_bytes() for p in family.rglob('*') if p.is_file()}
        def restore():
            for name,data in saved.items():
                p=family/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        def reject(name,operation):
            try:operation()
            except (ValueError,KeyError) as error:
                E.require(NATIVE_NEGATIVES[name] in str(error),'native audit negative wrong boundary: '+name+': '+str(error))
                cases.append({'name':name,'synthetic':True,'actual_exit':1,'expected_boundary':NATIVE_NEGATIVES[name],
                    'actual_reason':str(error),'rebinding':'SYNTHETIC_OUTER_HASHES_ONLY'})
            else:raise ValueError('native audit semantic contradiction accepted: '+name)
            restore();E.require(verify(work)==positive and verify_family(family,True)==expected,'native audit restored positive differs')
        result=read(work/'result.json');sample=work/result['sample'];capture=read(sample/'capture.json');start=read(work/'start.json')
        def rebind():
            cap=read(sample/'capture.json')
            for name,row in cap['objects'].items():
                path=sample/(name+'.bin');row.update(size=path.stat().st_size,sha256=E.sha(path))
            cap['serial_sha256']=E.sha(sample/'serial.log');cap['start_sha256']=E.sha(work/'start.json')
            (sample/'capture.json').write_bytes(E.canonical(cap));row=read(work/'result.json')
            row.update(start_sha256=cap['start_sha256'],capture_sha256=E.sha(sample/'capture.json'))
            (work/'result.json').write_bytes(E.canonical(row))
        def patch(name,offset,value,size=8):
            path=sample/(name+'.bin');data=bytearray(path.read_bytes());data[offset:offset+size]=value.to_bytes(size,'little');path.write_bytes(data)
        admitted=result['totals']['admitted']
        for label,index in (('prefix',0),('middle',admitted//2),('tail',admitted-1)):
            patch('g_audit',index*256,0);rebind();reject('native-missing-'+label,lambda:verify(work))
        for name,obj,offset,value,size in (
            ('native-overflow','g_audit_overflow',0,1,len((sample/'g_audit_overflow.bin').read_bytes())),
            ('native-missing-ack','g_audit',184+8,0,4),
            ('native-active-terminal','g_request',LAYOUT['request']['active'],1,8),
            ('native-wrong-apic','g_cpus',LAYOUT['cpu']['apic_id'],0xf00,4)):
            patch(obj,offset,value,size);rebind();reject(name,lambda:verify(work))
        serial=(sample/'serial.log').read_text().splitlines(keepends=True)
        events=[i for i,line in enumerate(serial) if re.match(r'^\[STACK_GUARD\]\[(ALLOC|RETIRE)\] ',line)]
        for label,index in (('prefix',0),('middle',len(events)//2),('tail',len(events)-1)):
            (sample/'serial.log').write_text(''.join(line for i,line in enumerate(serial) if i!=events[index]))
            rebind();reject('native-event-'+label,lambda:verify(work))
        frame=capture['table_frames'][-1];cap=copy.deepcopy(capture);del cap['objects']['table-'+str(frame)]
        cap['table_frames'].remove(frame);(sample/'capture.json').write_bytes(E.canonical(cap));rebind()
        reject('native-table-chunk-missing',lambda:verify(work))
        table=result['totals']['root'];va=start['objects']['g_request']['virtual']
        for level in (4,3,2,1):
            path=sample/('table-'+str(table)+'.bin');offset=((va>>(12+9*(level-1)))&511)*8
            value=u(path.read_bytes(),offset)
            if level==1:patch('table-'+str(table),offset,value^4096)
            else:table=value&0x000ffffffffff000
        rebind();reject('native-table-wrong-frame',lambda:verify(work))
        changed=copy.deepcopy(start);changed['kernel_sha256']='0'*64;(work/'start.json').write_bytes(E.canonical(changed));rebind()
        reject('native-cross-boot-elf',lambda:verify(work))
        stream=root/'launches.jsonl';stream.write_bytes(stream.read_bytes()+E.canonical(launches[-1]))
        reject('native-duplicate-launch',lambda:verify_family(family,True))
        stream.write_bytes(E.canonical(launches[0]));shutil.rmtree(root/bases[-1].name)
        def terminal():E.require(verify_family(family,True)==expected,'native audit terminal launch inventory differs')
        reject('native-launch-tail-removed',terminal)
        changed=copy.deepcopy(launches);changed[1]['start_sha256']=changed[0]['start_sha256']
        stream.write_bytes(b''.join(E.canonical(row) for row in changed))
        reject('native-launch-cross-boot',lambda:verify_family(family,True))
    E.require(all((E.sha(p),p.stat().st_size,p.stat().st_mode&0o777)==facts for p,facts in originals.items()),
        'native audit fixtures changed retained originals')
    result={'status':'PASS','authority':'SYNTHETIC_MUTATIONS_OF_CURRENT_NATIVE_PHYSICAL_CAPTURES',
        'base':[{'path':p.relative_to(campaign).as_posix(),'start_sha256':E.sha(p/'start.json'),
                 'result_sha256':E.sha(p/'result.json')} for p in bases],
        'cases':cases,'originals_rechecked':len(originals),'restored_positives':len(cases)}
    verify_fixture_receipt(result);return result

def read_lines(path):return [json.loads(line) for line in Path(path).read_text().splitlines()]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('prepare','capture','capture-panic'))
    p.add_argument('--runtime',type=Path);p.add_argument('--kernel',type=Path);p.add_argument('--image',type=Path)
    p.add_argument('--directory',type=Path);p.add_argument('--hmp',type=Path);p.add_argument('--serial',type=Path)
    p.add_argument('--pid',type=int);p.add_argument('--action',choices=('restart','shutdown'))
    a=p.parse_args()
    if a.mode=='capture-panic':
        E.require(all((a.directory,a.hmp,a.serial,a.pid,a.action)),'native panic capture arguments missing')
        capture(a.directory,a.hmp,a.serial,a.pid,panic_action=a.action);verify(a.directory);return
    E.require(a.runtime is not None,'native audit runtime missing')
    runtime=a.runtime.resolve();pid=int((runtime/'qemu.pid').read_text())
    record=runtime/(PREFIX+'.json')
    if a.mode=='prepare':
        directory=prepare(a.kernel.resolve(),a.image.resolve(),pid)
        reference(directory,runtime)
    else:
        row=read(record);directory=Path(row['directory'])
        E.require(E.sha(directory/'start.json')==row['start_sha256'],'native audit start reference differs')
        capture(directory,runtime/'hmp.sock',runtime/'qemu-serial.log',pid)
if __name__=='__main__':main()
