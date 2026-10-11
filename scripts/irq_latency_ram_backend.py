"""Explicit two-condition RAM diagnostic; aggregate host observations only."""
import json
import socket
import time
from pathlib import Path
import irq_latency_authority as A
import irq_latency_scheduling as S
from irq_latency_evidence import require

CONTRACT='anonymous-ram-preallocation-v1'
GUEST_BYTES=2*1024**3
PHASES=('host-started.json','host-running.json','host-after-activation.json','host-after-stress.json','host-terminal.json')

def contract(plan):
    value=plan.get('ram_backend')
    if value is None:return None
    require(plan['role']=='DIAGNOSTIC_NO_ACCEPTANCE' and plan['boots']==1 and plan['retries']==0 and
        plan['profile']==dict(machine='q35',accel='kvm',smp=8,memory='2G',placement='default') and
        plan.get('heap_operation_contract')=='heap-metadata-callsite-v1','RAM diagnostic role/profile/stage')
    require(value==dict(method=CONTRACT,prealloc=value.get('prealloc'),size=GUEST_BYTES,object='diagnostic.ram') and
        value['prealloc'] in ('off','on'),'RAM diagnostic backend declaration')
    return value

def launch(plan,row):
    value=contract(plan);require(value,'RAM backend plan missing')
    argv=row['argv']
    require(A.option(argv,'-machine')=='q35,memory-backend=diagnostic.ram' and
        A.option(argv,'-object')==f"memory-backend-ram,id=diagnostic.ram,size=2G,prealloc={value['prealloc']}" and
        A.option(argv,'-m')=='2G' and A.option(argv,'-accel')=='kvm' and
        A.option(argv,'-smp')=='8,sockets=1,cores=8,threads=1' and
        '-mem-prealloc' not in argv and '-mem-path' not in argv and '-numa' not in argv,
        'RAM actual launch property/topology differs')
    return value

def admit(snapshot):
    values={}
    for line in snapshot['meminfo'].splitlines():
        key,_,v=line.partition(':');fields=v.split()
        if len(fields)==2 and fields[1]=='kB':values[key]=int(fields[0])*1024
    # Guest RAM plus one GiB for QEMU/device/translation overhead; four GiB host reserve.
    demand=GUEST_BYTES+1024**3;reserve=4*1024**3
    require(values.get('MemAvailable',0)>=demand+reserve,'RAM diagnostic host resident headroom unavailable')
    limits=[]
    for row in snapshot['cgroup']['ancestors']:
        f=row['fields']
        for key in ('memory.max','memory.high'):
            if key not in f or f[key]=='max':continue
            require(f[key].isdecimal() and f.get('memory.current','').isdecimal(),'RAM cgroup headroom unavailable')
            free=int(f[key])-int(f['memory.current']);limits.append(dict(path=row['path'],limit=key,headroom=free))
            require(free>=demand+reserve,'RAM diagnostic cgroup resident headroom insufficient')
    return dict(status='ADMITTED',guest_bytes=GUEST_BYTES,qemu_overhead_provision=1024**3,
        host_reserve_bytes=reserve,mem_available=values['MemAvailable'],cgroup_headroom=limits,
        commitment={k:values.get(k) for k in ('CommitLimit','Committed_AS','SwapFree')},
        authority='RESIDENT_MEMORY_PREFLIGHT_NOT_EVENT_CAUSE')

def observe(argv):
    path=A.option(argv,'-qmp');require(path.startswith('unix:'),'RAM QMP socket')
    begin=time.monotonic_ns();transcript=[];values={}
    commands=[('capabilities',dict(execute='qmp_capabilities')),('uuid',dict(execute='query-uuid')),
        ('memory',dict(execute='query-memory-size-summary')),
        ('backend',dict(execute='qom-get',arguments=dict(path='/machine',property='memory-backend')))]
    for prop in ('size','prealloc','share','merge','dump','reserve','host-nodes','policy','prealloc-threads'):
        commands.append((prop,dict(execute='qom-get',arguments=dict(path='/objects/diagnostic.ram',property=prop))))
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
        client.settimeout(5);client.connect(path[5:].split(',')[0])
        with client.makefile('rwb',buffering=0) as stream:
            def receive():
                data=json.loads(stream.readline(1048576));transcript.append(dict(receive=data));return data
            require('QMP' in receive(),'RAM QMP greeting')
            for name,command in commands:
                command['id']='ram-'+name;transcript.append(dict(send=command));stream.write((json.dumps(command)+'\n').encode())
                for _ in range(256):
                    reply=receive()
                    if reply.get('id')==command['id']:
                        values[name]=dict(status='AVAILABLE',value=reply['return']) if 'return' in reply else dict(status='UNAVAILABLE',reason=reply.get('error'))
                        break
                else:raise ValueError('RAM QMP event limit')
    return dict(schema=1,begin_ns=begin,end_ns=time.monotonic_ns(),transcript=transcript,values=values)

def verify_backend(plan,launch_row,row):
    value=launch(plan,launch_row);require(0<row['begin_ns']<=row['end_ns'],'RAM QMP collection order')
    received=[r['receive'] for r in row['transcript'] if 'receive' in r and 'id' in r['receive']]
    replies={r['id']:r for r in received}
    require(len(replies)==len(received) and set(replies)=={'ram-'+k for k in row['values']},'RAM QMP duplicate/missing reply')
    for key,v in row['values'].items():
        reply=replies.get('ram-'+key,{})
        require((v['status']=='AVAILABLE' and reply.get('return')==v['value']) or
            (v['status']=='UNAVAILABLE' and reply.get('error')==v['reason']), 'RAM QMP raw property differs')
    def get(k):
        p=row['values'][k];require(p['status']=='AVAILABLE','RAM required QMP property unavailable: '+k);return p['value']
    require(get('uuid')=={'UUID':A.option(launch_row['argv'],'-uuid')} and
        get('backend')=='/objects/diagnostic.ram' and get('size')==GUEST_BYTES and
        get('prealloc') is (value['prealloc']=='on') and
        get('memory')=={'base-memory':GUEST_BYTES,'plugged-memory':0},'RAM backend identity/property not verified')
    return dict(status='PASS',properties=row['values'],timing_authority=False)

def stat(text):
    identity=S.identity(text);v=text.rpartition(') ')[2].split()
    require(len(v)>=22 and all(v[i].isdecimal() for i in (7,9,20,21)),'RAM process/thread stat counters')
    return dict(identity=identity,minor_faults=int(v[7]),major_faults=int(v[9]),virtual_bytes=int(v[20]),resident_pages=int(v[21]))

def host(rows):
    require(len(rows)==len(PHASES),'RAM host phase missing; unavailable is not zero')
    S.verify(rows)
    samples=[]
    for row in rows:
        process=stat(row['process_stat_end']);require(process['identity']==S.identity(row['process_stat_begin']),'RAM process lifetime')
        threads={str(t['tid']):stat(t['stat']) for t in row['threads']}
        for t in row['threads']:require(threads[str(t['tid'])]['identity']==S.identity(t['stat_begin']),'RAM thread lifetime')
        samples.append(dict(begin_ns=row['collection_begin_ns'],end_ns=row['collection_end_ns'],
            process=process,threads=threads,guest_cpus=row['guest_cpus'],qmp=row['qmp_identity']['cpus'],
            cgroup=row['cgroup'],meminfo=row['meminfo']))
    intervals=[]
    for a,b in zip(samples,samples[1:]):
        def changes(x,y):
            require(x['identity']==y['identity'],'RAM reused process/thread ID')
            values={k:y[k]-x[k] for k in ('minor_faults','major_faults')}
            require(all(v>=0 for v in values.values()),'RAM fault counter reversed')
            return dict(**values,resident_pages_before=x['resident_pages'],resident_pages_after=y['resident_pages'])
        common=a['threads'].keys()&b['threads'].keys()
        intervals.append(dict(process=changes(a['process'],b['process']),threads={t:changes(a['threads'][t],b['threads'][t]) for t in common},
            exited_threads=sorted(a['threads'].keys()-b['threads'].keys()),new_threads=sorted(b['threads'].keys()-a['threads'].keys())))
    return dict(status='PASS',phases=PHASES,samples=samples,intervals=intervals,
        authority='AGGREGATE_FAULT_AND_RESIDENCY_OBSERVATIONS_ONLY',individual_event_cause='NOT_ESTABLISHED',
        zero_fault_delta_excludes_interference=False)

def controlled_pair(plans,launches,backends):
    require(len(plans)==len(launches)==len(backends)==2,'RAM exactly two conditions')
    require([contract(p)['prealloc'] for p in plans]==['off','on'],'RAM condition order')
    commands=[p['commands'] for p in plans]
    if any('proof_output' in p for p in plans):
        import tlb_proof_evidence as proof
        policies=[proof.contract(p) for p in plans]
        require({k:v for k,v in policies[0].items() if k!='session'}==
                {k:v for k,v in policies[1].items() if k!='session'},'RAM proof policies differ')
        commands=[['irqlatency proof '+p['policy']+' <session> '+str(p['invocations']),*c[1:]]
                  for p,c in zip(policies,commands)]
    require(commands[0]==commands[1],'RAM controlled pair differs: commands')
    for key in ('candidate_id','elf_sha256','image_sha256','build_identity','harness_identity','profile',
                'detail_policy','pairing','observer_contract_sha256','heap_operation_contract'):
        require(plans[0][key]==plans[1][key],'RAM controlled pair differs: '+key)
    for key in ('qemu_version','firmware'):
        a,b=launches[0][key],launches[1][key]
        if key=='firmware':a,b=([{'sha256':v['sha256'],'size':v['size']} for v in x] for x in (a,b))
        require(a==b,'RAM executor/firmware changed')
    require(normalized_argv(launches[0]['argv'])==normalized_argv(launches[1]['argv']),'RAM undeclared launch contrast')
    require(plans[0]['vm_ids'][0]!=plans[1]['vm_ids'][0],'RAM distinct VM identity')
    for p,l,b in zip(plans,launches,backends):verify_backend(p,l,b)
    props=[{k:v for k,v in b['values'].items() if k not in ('prealloc','uuid','capabilities')} for b in backends]
    require(props[0]==props[1],'RAM backend has extra changed properties')
    return dict(status='PASS',method=CONTRACT,changed_property='prealloc',diagnostic_only=True,
        implementation_acceptance=False,timing_successes=0)

def normalized_argv(argv):
    """Remove only per-run transport paths/UUID and the declared boolean contrast."""
    import re
    result=[]
    paths={'-serial','-debugcon','-D','-monitor','-qmp','-pidfile'}
    for i,arg in enumerate(argv):
        prior=argv[i-1] if i else None
        if prior=='-uuid':arg='<distinct-vm>'
        elif prior in paths:
            parts=arg.split(',');prefix=parts[0].split(':',1)
            arg=(prefix[0]+':' if len(prefix)==2 else '')+'<owned-path>'+(','+','.join(parts[1:]) if len(parts)>1 else '')
        elif prior=='-drive':arg=re.sub(r'(?<=file=)[^,]+','<identified-image-or-firmware>',arg)
        elif prior=='-object':arg=arg.replace('prealloc=off','prealloc=<condition>').replace('prealloc=on','prealloc=<condition>')
        result.append(arg)
    return result

def guest_inventory(serial):
    import re
    def rows(prefix):
        result=[]
        for line in serial.splitlines():
            if line.startswith(prefix):
                result.append(dict(re.findall(r'([a-zA-Z0-9_]+)=([^ ]+)',line)))
        return result
    cpus=rows('[CPU_STATE][READY]');online=rows('[SMP][CPU] ONLINE')
    require(len(cpus)==len(online)==8 and {int(r['slot']) for r in cpus}==set(range(8)) and
        {int(r['slot']) for r in online}==set(range(8)),'RAM guest CPU topology')
    require({(r['slot'],r['apic'],r['role']) for r in cpus}=={(r['slot'],r['apic'],r['role']) for r in online},'RAM guest CPU identity')
    for row in cpus:
        require(int(row['cr0'],16)==0x80010033 and int(row['cr4'],16)==0x20 and
            int(row['efer'],16)==0xd00 and int(row['pat'],16)==0x0007010600070406,'RAM guest control/feature contract')
    preload=rows('[RAM_MAP] PRELOAD');active=rows('[RAM_MAP] ACTIVE')
    require(len(preload)==len(active)==1 and preload[0]['verified']=='1' and preload[0]['nx']=='1' and
        active[0]['identity_retained']=='0' and active[0]['nx']=='ENABLED' and
        int(active[0]['ceiling'],16)==GUEST_BYTES and int(active[0]['base'],16)==0xffff800000000000 and
        int(active[0]['span'],16)==0x400000000000,'RAM guest direct-map geometry')
    return dict(status='PASS',cpus=sorted(cpus,key=lambda r:int(r['slot'])),preload=preload[0],active=active[0])
