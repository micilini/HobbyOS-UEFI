"""Reconcile delayed immutable stack-lifetime rows with terminal cutoffs."""
import re,zlib
import irq_latency_evidence as V

CONTRACT='immutable-stack-lifetime-drain-v1'

def verify(serial):
    raw=V.records(serial);lines=serial.splitlines();position={}
    state=None;consumed=0;records=0;drains=0;cutoffs=[]
    active=False;late=[];requests=[]
    for index,line in enumerate(lines):
        if line.startswith('[IRQ_LATENCY][BEGIN] '):active=True
        if line.startswith('[IRQ_LATENCY][STACK_REPORT_BEGIN] '):
            row=V.records(line)['STACK_REPORT_BEGIN'][0]
            V.require(active and state is None and V.number(row,'first')==consumed and
                consumed<V.number(row,'limit')<=consumed+1024,'stack report drain identity/capacity')
            state=(consumed,V.number(row,'limit'),0)
        elif line.startswith(('[STACK_GUARD][ALLOC] ','[STACK_GUARD][RETIRE] ')) and state is not None:
            body,sep,crc=line.rpartition(' crc=')
            V.require(sep and re.fullmatch('[0-9a-f]{8}',crc) and zlib.crc32(body.encode())==int(crc,16),'stack report original CRC')
            state=(state[0],state[1],state[2]+1);records+=1
            if 'WORK_CUTOFF' in position:
                late.append((line.split(']')[1][1:],dict(t.split('=',1) for t in body.split()[1:])))
        elif active and line.startswith(('[STACK_GUARD][ALLOC] ','[STACK_GUARD][RETIRE] ')):
            raise ValueError('irq latency stack report outside declared drain')
        elif line.startswith('[IRQ_LATENCY][STACK_REPORT_END] '):
            row=V.records(line)['STACK_REPORT_END'][0]
            V.require(state is not None and V.number(row,'first')==state[0] and
                V.number(row,'after')==state[1] and V.number(row,'admitted')>=state[1] and
                state[2]==state[1]-state[0] and row['status']=='COMPLETE','stack report missing/extra/incomplete drain')
            consumed=state[1];state=None;drains+=1
        elif line.startswith('[IRQ_LATENCY][STACK_REPORT_CUTOFF] '):
            row=V.records(line)['STACK_REPORT_CUTOFF'][0]
            V.require(state is None and V.number(row,'admitted')==V.number(row,'consumed')==consumed and
                V.number(row,'complete')==1,'stack report incomplete cutoff')
            cutoffs.append((row['role'],index,consumed))
        for name in ('WORK_CUTOFF','AUDIT_END'):
            if line.startswith('[IRQ_LATENCY]['+name+'] '):position[name]=index
        if line.startswith('[IRQ_LATENCY][AUDIT_END] '):active=False
    V.require(state is None and len(cutoffs)==2 and cutoffs[0][0]=='work' and cutoffs[1][0]=='final' and
        cutoffs[0][2]<=cutoffs[1][2]==records==consumed and
        cutoffs[0][1]<position.get('WORK_CUTOFF',-1)<cutoffs[1][1]<position.get('AUDIT_END',-1),
        'stack report terminal chronology or late ordinary work')
    if raw['BEGIN'][0].get('closure')=='snapshot-before-post-calibration-v1':
        cpus=V.number(raw['BEGIN'][0],'cpus')
        workers=V.index([r for r in raw['CALIBRATION_WORKER'] if V.number(r,'role')==1],('index',))
        V.require(set(workers)=={(i,) for i in range(cpus)} and
            all(V.number(r,'done')==V.number(r,'joined')==1 for r in workers.values()),'post calibration worker inventory')
        owners={(V.number(r,'task'),V.number(r,'lifecycle')) for r in workers.values()}
        V.require(all(task>0 and lifecycle>0 for task,lifecycle in owners),'invalid post calibration worker owner')
        allocated={};retired=set();seen=set()
        for kind,r in late:
            request=V.number(r,'request')
            V.require(request>0 and request not in requests,'reused post calibration stack service');requests.append(request)
            key=(V.number(r,'slot'),V.number(r,'generation'))
            V.require(r['status']=='COMPLETE' and int(r['requested'],16)==int(r['acknowledged'],16)==(1<<cpus)-1,
                'post calibration stack service incomplete')
            if kind=='ALLOC':
                V.require(key not in allocated,'post calibration stack reused');allocated[key]=r
            else:
                owner=(V.number(r,'task'),V.number(r,'lifecycle'))
                V.require(kind=='RETIRE' and owner in owners and owner not in seen and key in allocated and
                    key not in retired and all(r[k]==allocated[key][k] for k in ('base','physical','bytes')),
                    'late ordinary stack work or wrong calibration owner')
                seen.add(owner);retired.add(key)
        V.require(len(late)==2*cpus and seen==owners and len(owners)==cpus and set(allocated)==retired,
            'post calibration stack lifecycle missing or extra')
    else:
        V.require(not late and cutoffs[0][2]==cutoffs[1][2],'late stack work in original closure')
    return dict(status='PASS',contract=CONTRACT,records=records,drains=drains,admitted=consumed,qualification_requests=requests)
