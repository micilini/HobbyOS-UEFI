"""Independent diagnostic minimum reconstruction; never grants upper-bound acceptance."""
import irq_latency_evidence as V
import irq_latency_pairs as P

CONTRACT='continuous-mask-lock-body-v2'
CLOCK_CONTRACT='masked-clock-segment-v3'
TICK_CONTRACT='lapic-handler-body-v4'

def verify_tick(raw,cpus):
    clocks=P.clocks(raw,cpus);n=V.number
    states=V.index(raw['CPU'],('cpu',));rows=V.index(raw['TICK_BODY'],('cpu',));samples=V.index(raw['TICK_SAMPLE'],('cpu',))
    V.require(len(raw['TERMINAL'])==1 and n(raw['TERMINAL'][0],'invalid_boundaries')==0 and
        set(states)==set(rows)==set(samples)=={(c,) for c in range(cpus)} and all(n(r,'invalid')==0 for r in states.values()),
        'tick complete CPU/controller inventory')
    points={}
    for p in raw['TICK_BODY_POINT']:
        key=(n(p,'cpu'),p['endpoint'])
        V.require(key not in points and (key[0],) in rows and key[1] in ('begin','end'),'tick endpoint identity')
        points[key]=p
    result=[];expected=set()
    for (cpu,),r in rows.items():
        V.require(r['method']=='lapic-handler-body' and n(r,'version')==4 and n(r,'epoch')==1 and
            n(r,'vector')==34 and r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and
            r['below_budget']=='NOT_ESTABLISHED','tick method/authority')
        q=samples[(cpu,)]
        V.require(all(n(q,k)==n(r,k) for k in ('epoch','sequence','outer_sequence')),'tick sample identity')
        count,invalid,cycles=(n(r,k) for k in ('count','invalid','cycles'))
        upper=V.ceildiv(max(clocks[(role,cpu)]['upper_hz'] for role in (0,1))*1010000,1000000)
        minimum=cycles*1000000000//upper
        V.require(0<upper<=V.U64 and minimum<=V.U64 and n(r,'frequency_valid')==1 and
            n(r,'frequency_upper')==upper and n(r,'minimum_ns')==minimum,'tick minimum conversion')
        V.require(count+n(r,'without_memory')<=n(r,'retired')<=n(r,'started')<V.U64 and
            n(r,'pending')<=n(r,'started') and (invalid or count+n(r,'without_memory')==n(r,'retired')),
            'tick complete reservation accounting')
        witness=None;epoch_ok=False
        if count:
            keys=[(cpu,side) for side in ('begin','end')];expected.update(keys)
            V.require(all(k in points for k in keys),'tick missing endpoint')
            a,b=(points[k] for k in keys)
            stamps=[n(a,'tsc'),n(r,'sample_begin'),n(q,'counter_end'),n(r,'sample_end'),n(r,'after_clock'),n(r,'after_work'),n(b,'tsc')]
            V.require(0<stamps[0]<stamps[1]<stamps[2]<stamps[3]<stamps[4]<stamps[5]<stamps[6] and stamps[6]-stamps[0]==cycles and
                n(a,'flags')&512==n(b,'flags')&512==0,'tick stage timestamp/flags')
            for k in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock'):
                V.require(n(a,k)==n(b,k)<V.U64,'tick continuous identity: '+k)
            V.require(n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and n(a,'segment')>0 and
                n(a,'generation')>0 and n(a,'lock')==0 and 0<n(r,'sequence')<=n(r,'retired') and
                0<n(r,'outer_sequence')<V.U64 and 0<n(r,'paths')<1<<11,'tick source/outer identity')
            for k in ('queries','recorder_sequence'):
                V.require(n(a,k)<=n(b,k)<V.U64,'tick observer accounting')
            provider=n(q,'source');epoch=n(q,'source_epoch')
            hpet=provider==1 and epoch==1 and ((n(q,'mode')==1 and n(q,'retries')<=4) or
                (n(q,'mode')==2 and n(q,'retries')==0))
            kvm=provider==2 and epoch==2 and n(q,'mode')==0 and n(q,'retries')<4 and stamps[1]<n(q,'ticks')<stamps[2]
            V.require(n(q,'valid')==1 and n(q,'ticks')>0 and (hpet or kvm),'tick actual counter validity/mode')
            bounds=[];counter_bounds=[]
            for role in (0,1):
                local=[p for p in raw['REFERENCE'] if n(p,'role')==role and n(p,'cpu')==cpu and P.eligible(p,cpus)==0]
                V.require(local and (not hpet or all(n(p,'mode')==n(q,'mode') for p in local)),'tick calibration counter domain')
                bounds.append(max(n(p,'tsc_after') for p in local))
                if hpet:counter_bounds.append(max(n(p,'hpet') for p in local))
            epoch_ok=bounds[0]<=stamps[0]<stamps[6]<=bounds[1] and (kvm or counter_bounds[0]<=n(q,'ticks')<=counter_bounds[1])
            witness=dict(begin=a,end=b,sample_begin=stamps[1],counter_end=stamps[2],sample_end=stamps[3],after_clock=stamps[4],after_work=stamps[5],
                sample=q,counter_bounds=counter_bounds,stage_cycles=[stamps[i]-stamps[i-1] for i in range(1,7)],
                sequence=n(r,'sequence'),outer_sequence=n(r,'outer_sequence'),paths=n(r,'paths'),calibration_bounds=bounds)
        else:
            V.require(all(n(r,k)==0 for k in ('cycles','sequence','outer_sequence','paths','sample_begin','sample_end','after_clock','after_work')),
                'tick erased maximum')
            V.require(all(n(q,k)==0 for k in ('counter_end','ticks','mode','retries','valid','source','source_epoch')),'tick empty sample')
        complete=not invalid and n(r,'pending')==0 and n(r,'started')==n(r,'retired')
        authority=bool(complete and count and epoch_ok)
        result.append(dict(cpu=cpu,count=count,without_memory=n(r,'without_memory'),invalid=invalid,complete=complete,
            raw_cycles=cycles,frequency_upper=upper,minimum_ns=minimum,epoch_qualified=epoch_ok,
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if authority else 'UNAVAILABLE',
            observed_violation=authority and minimum>=50000,below_budget_acceptance=False,witness=witness))
    V.require(set(points)==expected,'unused tick endpoint')
    return dict(status='PASS',contract=TICK_CONTRACT,rows=result,implementation_acceptance=False,
        below_budget_acceptance=False,interpretation='SAME_EVENT_HANDLER_REGIONS_WITH_CLOCK_BOUNDARIES',
        unique_cause='NOT_ESTABLISHED')

def verify_clock(raw,cpus):
    clocks=P.clocks(raw,cpus)
    states=V.index(raw['CPU'],('cpu',))
    V.require(len(raw['TERMINAL'])==1 and V.number(raw['TERMINAL'][0],'invalid_boundaries')==0 and
        set(states)=={(c,) for c in range(cpus)} and all(V.number(r,'invalid')==0 for r in states.values()),
        'clock body untracked controller transition')
    rows=V.index(raw['CLOCK_BODY'],('cpu',));points={}
    V.require(set(rows)=={(c,) for c in range(cpus)},'complete clock body inventory')
    for point in raw['CLOCK_BODY_POINT']:
        key=(V.number(point,'cpu'),point['endpoint'])
        V.require(key not in points and (key[0],) in rows and key[1] in ('begin','end'),'clock endpoint identity')
        points[key]=point
    result=[];expected=set()
    for (cpu,),r in rows.items():
        n=lambda p,k:V.number(p,k)
        V.require(r['method']=='masked-clock-segment' and n(r,'version')==3 and n(r,'epoch')==1 and
            r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and r['below_budget']=='NOT_ESTABLISHED',
            'clock body method/authority')
        count=n(r,'count');invalid=n(r,'invalid');cycles=n(r,'cycles')
        upper=V.ceildiv(max(clocks[(role,cpu)]['upper_hz'] for role in (0,1))*1010000,1000000)
        minimum=cycles*1000000000//upper
        V.require(0<upper<=V.U64 and minimum<=V.U64 and n(r,'frequency_valid')==1 and
            n(r,'frequency_upper')==upper and n(r,'minimum_ns')==minimum,'clock minimum conversion')
        V.require(count<=n(r,'retired')<=n(r,'started')<V.U64 and n(r,'pending')<=n(r,'started'),
            'clock reservation/retirement accounting')
        V.require(invalid or count==n(r,'retired'),'clock missing valid maximum count')
        witness=None;epoch_ok=False
        if count:
            keys=[(cpu,side) for side in ('begin','end')];expected.update(keys)
            V.require(all(key in points for key in keys),'clock missing endpoint')
            a,b=(points[key] for key in keys)
            V.require(n(a,'tsc')>0 and n(b,'tsc')-n(a,'tsc')==cycles>0 and
                n(a,'flags')&512==n(b,'flags')&512==0,'clock endpoint flags/order')
            for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock'):
                V.require(n(a,key)==n(b,key)<V.U64,'clock continuity: '+key)
            V.require(n(r,'phase') in (1,2,3,4) and (n(r,'phase') in (3,4) or n(r,'incoming_flags')&512) and
                n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and n(a,'segment')>0 and
                n(a,'generation')>0 and n(a,'lock')==0 and 0<n(r,'sequence')<=n(r,'retired') and
                0<n(r,'paths')<1<<11 and n(r,'site')>=0xffffffff80000000,'clock source/owner identity')
            for key in ('queries','recorder_sequence'):
                V.require(n(a,key)<=n(b,key)<V.U64,'clock observer work accounting')
            bounds=[]
            for role in (0,1):
                local=[p for p in raw['REFERENCE'] if n(p,'role')==role and n(p,'cpu')==cpu and P.eligible(p,cpus)==0]
                V.require(local,'clock local calibration epoch missing')
                bounds.append(max(n(p,'tsc_after') for p in local))
            epoch_ok=bounds[0]<=n(a,'tsc')<n(b,'tsc')<=bounds[1]
            witness=dict(begin=a,end=b,site=n(r,'site'),paths=n(r,'paths'),phase=n(r,'phase'),incoming_flags=n(r,'incoming_flags'),
                sequence=n(r,'sequence'),identity_queries=n(b,'queries')-n(a,'queries'),
                recorder_sequence_delta=n(b,'recorder_sequence')-n(a,'recorder_sequence'),calibration_bounds=bounds)
        else:
            V.require(all(n(r,k)==0 for k in ('cycles','sequence','site','paths','phase','incoming_flags')),'erased clock maximum')
        complete=n(r,'started')==n(r,'retired') and n(r,'pending')==0 and invalid==0
        authority=bool(count and complete and epoch_ok)
        result.append(dict(cpu=cpu,count=count,invalid=invalid,complete=complete,raw_cycles=cycles,
            frequency_upper=upper,minimum_ns=minimum,epoch_qualified=epoch_ok,
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if authority else 'UNAVAILABLE',
            observed_violation=authority and minimum>=50000,below_budget_acceptance=False,witness=witness))
    V.require(set(points)==expected,'unused clock endpoint')
    return dict(status='PASS',contract=CLOCK_CONTRACT,rows=result,implementation_acceptance=False,
        below_budget_acceptance=False,unique_cause='CLOCK_BODY_WITH_BOUNDARIES_OBSERVER_AND_INTERFERENCE')

def verify(raw,cpus):
    clocks=P.clocks(raw,cpus)
    terminal=raw['TERMINAL'];states=V.index(raw['CPU'],('cpu',))
    V.require(len(terminal)==1 and V.number(terminal[0],'invalid_boundaries')==0 and
        set(states)=={(c,) for c in range(cpus)} and all(V.number(r,'invalid')==0 for r in states.values()),
        'interior untracked controller transition')
    rows=V.index(raw['INTERIOR'],('cpu','path'))
    V.require(set(rows)=={(c,p) for c in range(cpus) for p in range(11)},'complete interior maximum inventory')
    points={}
    for p in raw['INTERIOR_POINT']:
        key=(V.number(p,'cpu'),V.number(p,'path'),p['endpoint'])
        V.require(key not in points and key[:2] in rows and key[2] in ('begin','end'),'interior endpoint identity')
        points[key]=p
    result=[];expected=set()
    for (cpu,path),r in rows.items():
        V.require(r['method']=='continuous-mask-lock-body' and V.number(r,'version')==2 and
            V.number(r,'epoch')==1 and r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and
            r['below_budget']=='NOT_ESTABLISHED','interior cannot become upper acceptance')
        upper=V.ceildiv(max(clocks[(0,cpu)]['upper_hz'],clocks[(1,cpu)]['upper_hz'])*1010000,1000000)
        cycles=V.number(r,'cycles');minimum=cycles*1000000000//upper
        V.require(0<upper<=V.U64 and minimum<=V.U64 and V.number(r,'frequency_valid')==1 and
            V.number(r,'frequency_upper')==upper and V.number(r,'minimum_ns')==minimum,'minimum downward conversion')
        count=V.number(r,'count');invalid=V.number(r,'invalid');epoch_ok=False;witness=None
        if count:
            keys=[(cpu,path,e) for e in ('begin','end')];expected.update(keys)
            V.require(all(k in points for k in keys),'interior missing endpoint')
            a,b=(points[k] for k in keys);n=lambda p,k:V.number(p,k)
            V.require(n(a,'tsc')>0 and n(b,'tsc')-n(a,'tsc')==cycles>0 and
                n(a,'flags')&512==n(b,'flags')&512==0,'interior endpoint flags/order')
            for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock'):
                V.require(n(a,key)==n(b,key) and n(a,key)!=V.U64,'interior continuity identity: '+key)
            V.require(n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and all(n(a,k)>0 for k in ('segment','generation','depth','lock')),
                'interior owner/segment identity')
            for key in ('queries','recorder_sequence'):
                V.require(n(a,key)<=n(b,key)<V.U64,'interior observer work accounting')
            V.require(V.number(r,'paths')&(1<<path) and V.number(r,'paths')<1<<11,'interior path attribution')
            bounds=[]
            for role in (0,1):
                local=[p for p in raw['REFERENCE'] if V.number(p,'role')==role and V.number(p,'cpu')==cpu and P.eligible(p,cpus)==0]
                V.require(local,'interior local calibration epoch missing')
                bounds.append(max(V.number(p,'tsc_after') for p in local))
            epoch_ok=bounds[0]<=n(a,'tsc')<n(b,'tsc')<=bounds[1]
            witness=dict(begin=a,end=b,identity_queries=n(b,'queries')-n(a,'queries'),
                         recorder_sequence_delta=n(b,'recorder_sequence')-n(a,'recorder_sequence'),calibration_bounds=bounds)
        else:V.require(cycles==0 and V.number(r,'paths')==0,'erased interior maximum count')
        authority=bool(count and not invalid and epoch_ok)
        result.append(dict(cpu=cpu,path=path,count=count,invalid=invalid,raw_cycles=cycles,frequency_upper=upper,
            minimum_ns=minimum,epoch_qualified=epoch_ok,authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if authority else 'UNAVAILABLE',
            observed_violation=authority and minimum>=50000,below_budget_acceptance=False,witness=witness))
    V.require(set(points)==expected,'unused/duplicate interior endpoint')
    return dict(status='PASS',contract=CONTRACT,rows=result,implementation_acceptance=False,
                below_budget_acceptance=False,unique_memory_cause='REQUIRES_SEPARATE_SOURCE_AND_WORK_ANALYSIS')
