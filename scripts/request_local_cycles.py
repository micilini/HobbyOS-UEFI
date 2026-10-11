"""Independent source-bound auxiliary request cycles, never whole-window acceptance."""
import re
import irq_latency_evidence as V
import irq_latency_pairs as P

CONTRACT='request-local-cycles-v1'
COLUMNS=('generation target root local_generation local_root cpu apic epoch end_epoch status begin_flags end_flags '
         'handler_begin operation_begin operation_end handler_end send_cpu send_apic send_epoch send_end_epoch send_status '
         'send_begin_flags send_end_flags send_begin send_end').split()

def decode(row):
    V.require(set(row)=={'data'},'request-cycle record fields')
    cells=row['data'].split(':')
    V.require(len(cells)==len(COLUMNS) and all(re.fullmatch(r'0|[1-9][0-9]*',v) for v in cells),
              'request-cycle canonical column inventory')
    result=dict(zip(COLUMNS,map(int,cells)))
    V.require(all(v<=V.U64 for v in result.values()),'request-cycle integer overflow')
    return result

def validate(row,topology,root,generation,target,allow_unavailable=True,allow_clock_failure=False):
    r=decode(row)
    V.require(r['generation']==r['local_generation']==generation>0 and r['target']==r['cpu']==target in topology and
        r['root']==r['local_root']==root>0 and root%4096==0 and r['apic']==topology[target][0],
        'request-cycle generation/root/CPU identity')
    V.require(r['send_cpu'] in topology and r['send_apic']==topology[r['send_cpu']][0] and
        all(r[k] in (0,1) for k in ('epoch','end_epoch','send_epoch','send_end_epoch')),'request-cycle actor/epoch')
    for name in ('begin_flags','end_flags','send_begin_flags','send_end_flags'):
        V.require(r[name]==0,'request-cycle IRQ context')
    for status,endpoints,epoch in [('status',('handler_begin','operation_begin','operation_end','handler_end'),'epoch'),
                             ('send_status',('send_begin','send_end'),'send_epoch')]:
        state=r[status];values=[r[k] for k in endpoints]
        end_epoch='end_epoch' if epoch=='epoch' else 'send_end_epoch'
        ordered=0<values[0] and all(a<b for a,b in zip(values,values[1:]))
        derived=6 if r[epoch]!=r[end_epoch] else 1 if ordered else 4
        if state==2:
            V.require(allow_unavailable and not any(values),'request-cycle unavailable has invented endpoints')
        else:
            V.require(state==derived and (state==1 or allow_clock_failure and state in (4,6)),
                      'request-cycle missing/backward/non-nested endpoints or epoch')
    if r['cpu']==r['send_cpu'] and r['status']==r['send_status']==1:
        V.require(r['send_begin']<r['handler_begin']<r['handler_end']<r['send_end'],
                  'request-cycle local initiating section containment')
    r['operation_cycles']=r['operation_end']-r['operation_begin'] if r['status']==1 else None
    r['handler_cycles']=r['handler_end']-r['handler_begin'] if r['status']==1 else None
    r['send_cycles']=r['send_end']-r['send_begin'] if r['send_status']==1 else None
    return r

def upper_ns(cycles,hz):
    V.require(0<cycles<=V.U64 and 0<hz<=V.U64,'request-cycle conversion inputs')
    ns=V.ceildiv(cycles*1000000000,hz)
    V.require(ns<=V.U64,'request-cycle conversion overflow')
    return ns

def verify(raw,topology,root):
    V.require(len(raw['BEGIN'])==1 and raw['BEGIN'][0].get('request_timing')==CONTRACT and
        not raw.get('CLOCK_CALL') and not raw.get('CLOCK_CALL_POINT') and
        'writeback_clock_method' not in raw['BEGIN'][0],'request-cycle current/historical method')
    services={int(s['generation']):s for s in raw['SERVICE']}
    V.require(len(services)==len(raw['SERVICE']) and services,'request-cycle service binding')
    expected={(g,c) for g,s in services.items() for c in topology if int(s['targets'],0)&(1<<c)}
    records={};previous={};maxima={c:{'operation_cycles':None,'handler_cycles':None,'send_cycles':None} for c in topology}
    for row in raw['REQUEST_CYCLES']:
        r=decode(row);key=(r['generation'],r['target'])
        V.require(key in expected and key not in records,'request-cycle missing/duplicate/foreign target')
        service=services[key[0]]
        V.require(int(service['root'],0)==root and int(service['ack'],0)&(1<<key[1]),
                  'request-cycle uncompleted target')
        r=validate(row,topology,root,*key,allow_clock_failure=True)
        c=r['cpu']
        if r['status']==1:
            V.require(r['handler_begin']>previous.get(c,0),'request-cycle reused/out-of-order target timestamp')
            previous[c]=r['handler_end']
        for field,actor in (('operation_cycles',c),('handler_cycles',c),('send_cycles',r['send_cpu'])):
            if r[field] is not None and (maxima[actor][field] is None or r[field]>maxima[actor][field]['cycles']):
                maxima[actor][field]=dict(cycles=r[field],generation=r['generation'],target=r['target'],record=r.copy())
        records[key]=r
    V.require(set(records)==expected and list(records)==sorted(records),'request-cycle complete ordered ledger')
    callbacks=V.index(raw['CALLBACK'],('cpu','operation'))
    measured=[]
    for r in records.values():
        cb=callbacks.get((r['cpu'],int(services[r['generation']]['operation'])))
        if cb and V.number(cb,'count') and V.number(cb,'first_generation')<=r['generation']<=V.number(cb,'last_generation'):
            V.require(r['epoch']==1 or r['status']==6 and r['end_epoch']==1,'request-cycle epoch contradicts actual callback')
            measured.append(r)
    clock_status='PASS';clock_reason=None;clocks=None
    try:clocks=P.clocks(raw,len(topology))
    except (ValueError,KeyError) as e:clock_status='UNAVAILABLE';clock_reason=str(e)
    for cpu,values in maxima.items():
        for field,witness in values.items():
            if witness is None:continue
            r=witness['record'];is_send=field=='send_cycles'
            begin=r['send_begin' if is_send else 'handler_begin'];end=r['send_end' if is_send else 'handler_end']
            witness.update(upper_ns=None,frequency_hz=None,conversion_status='UNAVAILABLE',whole_window_acceptance=False)
            if not clocks or r['send_epoch' if is_send else 'epoch']!=1:continue
            bounds=[max(V.number(p,'tsc_after') for p in raw['REFERENCE'] if V.number(p,'role')==role and
                V.number(p,'cpu')==cpu and P.eligible(p,len(topology))==0) for role in (0,1)]
            if not bounds[0]<=begin<end<=bounds[1]:continue
            hz=clocks[(0,cpu)]['conversion_hz']
            witness.update(upper_ns=upper_ns(witness['cycles'],hz),frequency_hz=hz,
                           conversion_status='CALIBRATED_AUXILIARY_UPPER_ESTIMATE')
    return dict(status='PASS',contract=CONTRACT,records=len(records),generations=len(services),
        valid_local=sum(r['status']==1 for r in records.values()),valid_send=sum(r['send_status']==1 for r in records.values()),
        required_local_records=len(measured),required_local_valid=set(r['cpu'] for r in measured)==set(topology) and
            all(r['status']==1 and r['epoch']==1 for r in measured),
        invalid_local=sum(r['status'] not in (1,2) for r in records.values()),
        invalid_send=sum(r['send_status'] not in (1,2) for r in records.values()),
        cycles_only=sum(r['epoch']==0 for r in records.values()),per_cpu=maxima,
        frequency_status=clock_status,frequency_reason=clock_reason,implementation_acceptance=False,
        whole_window_acceptance=False,authority='AUXILIARY_LOCAL_CYCLES_ONLY')
