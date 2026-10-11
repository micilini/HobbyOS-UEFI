"""Independent observer-inclusive heap stages and isolated call-site operation; minima never grant acceptance."""
import irq_latency_evidence as V
import irq_latency_pairs as P

CONTRACT='continuous-mask-heap-stages-v1'
OPERATIONS={1:'allocation',2:'allocation-recheck',3:'free',4:'statistics',5:'integrity',
            6:'initialization',7:'initial-region-publication'}
SITES={1:'region-validation',2:'first-fit',3:'containment-validation',4:'pointer-accounting',
       5:'coalescence',6:'allocation-metadata',7:'region-publication',8:'statistics',
       9:'full-integrity',10:'initialization'}

def verify(raw,cpus):
    n=V.number;need=V.require
    need(len(raw['BEGIN'])==1 and raw['BEGIN'][0].get('heap_method')==CONTRACT,'heap method binding')
    clocks=P.clocks(raw,cpus)
    need(len(raw['TERMINAL'])==1 and n(raw['TERMINAL'][0],'invalid_boundaries')==0 and
         raw['TERMINAL'][0]['transport']=='COMPLETE','heap untracked transition/terminal')
    states=V.index(raw['CPU'],('cpu',))
    need(set(states)=={(c,) for c in range(cpus)} and all(n(r,'invalid')==0 for r in states.values()),'heap CPU state')
    rows=V.index(raw['HEAP_STAGE'],('cpu','operation'))
    need(set(rows)=={(c,o) for c in range(cpus) for o in OPERATIONS},'heap complete operation inventory')
    points={};marks=V.index(raw['HEAP_MARK'],('cpu','operation','sequence','index'))
    for p in raw['HEAP_POINT']:
        key=(n(p,'cpu'),n(p,'operation'),n(p,'sequence'),p['endpoint'])
        need(key not in points and key[3] in ('begin','end'),'heap duplicate/unknown endpoint');points[key]=p
    used_points=set();used_marks=set();result=[]
    for (cpu,op),r in sorted(rows.items()):
        need(r['method']=='continuous-mask-heap-stages' and n(r,'version')==1 and n(r,'epoch')==1 and
             r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and r['below_budget']=='NOT_ESTABLISHED',
             'heap method/authority cannot become accepted upper bound')
        count=n(r,'count');seq=n(r,'sequence');cycles=n(r,'cycles');invalid=n(r,'invalid');size=n(r,'marks')
        upper=V.ceildiv(max(clocks[(0,cpu)]['upper_hz'],clocks[(1,cpu)]['upper_hz'])*1010000,1000000)
        minimum=cycles*1000000000//upper
        need(0<upper<=V.U64 and minimum<=V.U64 and n(r,'frequency_valid')==1 and
             n(r,'frequency_upper')==upper and n(r,'minimum_ns')==minimum,'heap downward frequency conversion')
        stages=[];epoch_ok=False;witness=None
        if count:
            need(0<seq<=n(r,'last_sequence')<V.U64 and 0<size<=8 and cycles>0 and
                 n(r,'pending_begin')<=64 and n(r,'pending_end')<=64,'heap bounded mandatory state')
            keys=[(cpu,op,seq,e) for e in ('begin','end')]
            need(all(k in points for k in keys),'heap missing event endpoint');used_points.update(keys)
            a,b=(points[k] for k in keys)
            need(n(a,'tsc')>0 and n(b,'tsc')-n(a,'tsc')==cycles and
                 n(a,'flags')&512==n(b,'flags')&512==0,'heap IF/order')
            for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock'):
                need(n(a,key)==n(b,key)<V.U64,'heap continuous identity: '+key)
            need(n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and
                 all(n(a,k)>0 for k in ('segment','generation','depth','lock')),'heap owner/lock')
            for key in ('queries','recorder_sequence'):
                need(n(a,key)<=n(b,key)<V.U64,'heap observer work')
            need(0<n(r,'wait_start')<n(r,'acquired')<n(a,'tsc'),'heap same-event acquisition order')
            previous=n(a,'tsc');previous_site=0;sites=[]
            for i in range(size):
                key=(cpu,op,seq,i);need(key in marks,'heap missing stage');used_marks.add(key);m=marks[key]
                end=n(m,'tsc');site=n(m,'site');work=n(m,'work');byte_count=n(m,'bytes')
                need(previous<end<n(b,'tsc') and site in SITES and site!=previous_site and
                     work<V.U64 and byte_count<V.U64,'heap stage order/site/work')
                stages.append(dict(site=site,name=SITES[site],begin=previous,end=end,raw_cycles=end-previous,
                    minimum_ns=(end-previous)*1000000000//upper,work=work,bytes=byte_count))
                sites.append(site);previous=end;previous_site=site
            legal={1:([2],[2,6]),2:([2,6],[2,7],[2,7,2],[2,7,2,6]),
                   3:([1],[1,3],[1,3,4,5]),4:([8],),5:([9],),6:([10],),7:([7],)}
            need(sites in legal[op],'heap actual operation/stage plan')
            stages.append(dict(site=0,name='observer-return-residual',begin=previous,end=n(b,'tsc'),
                raw_cycles=n(b,'tsc')-previous,minimum_ns=(n(b,'tsc')-previous)*1000000000//upper,work=None,bytes=None))
            bounds=[]
            for role in (0,1):
                local=[p for p in raw['REFERENCE'] if n(p,'role')==role and n(p,'cpu')==cpu and P.eligible(p,cpus)==0]
                need(local,'heap calibration epoch');bounds.append(max(n(p,'tsc_after') for p in local))
            epoch_ok=bounds[0]<=n(a,'tsc')<n(b,'tsc')<=bounds[1]
            witness=dict(begin=a,end=b,calibration_bounds=bounds,identity_queries=n(b,'queries')-n(a,'queries'),
                recorder_sequence_delta=n(b,'recorder_sequence')-n(a,'recorder_sequence'),
                wait_cycles=n(r,'acquired')-n(r,'wait_start'),wait_authority='UNQUALIFIED_COMPONENT_ONLY')
        else:
            need(cycles==size==seq==n(r,'last_sequence')==0,'heap erased empty maximum')
        authority=bool(count and not invalid and epoch_ok)
        result.append(dict(cpu=cpu,operation=op,name=OPERATIONS[op],count=count,invalid=invalid,
            raw_cycles=cycles,minimum_ns=minimum,frequency_upper=upper,epoch_qualified=epoch_ok,
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if authority else 'UNAVAILABLE',
            observed_violation=authority and minimum>=50000,below_budget_acceptance=False,
            witness=witness,stages=stages))
    need(set(points)==used_points and set(marks)==used_marks,'heap unused/invented point or stage')
    return dict(status='PASS',contract=CONTRACT,rows=result,implementation_acceptance=False,
                below_budget_acceptance=False,unique_cause='SOURCE_AND_WORK_ASSESSMENT_REQUIRED')

OPERATION_CONTRACT='heap-metadata-callsite-v1'

def verify_operation(raw,cpus):
    n=V.number;need=V.require
    need(len(raw['BEGIN'])==1 and raw['BEGIN'][0].get('heap_operation_method')==OPERATION_CONTRACT,
         'isolated heap operation method/source binding')
    verify(raw,cpus)
    clocks=P.clocks(raw,cpus)
    parents=V.index(raw['HEAP_STAGE'],('cpu','operation'))
    rows=V.index(raw['HEAP_OPERATION'],('cpu',))
    need(set(rows)=={(c,) for c in range(cpus)},'isolated heap complete CPU inventory')
    points={}
    for p in raw['HEAP_OPERATION_POINT']:
        key=(n(p,'cpu'),n(p,'sequence'),p['endpoint'])
        need(key not in points and key[2] in ('parent_begin','begin','end','parent_end'),
             'isolated heap duplicate/unknown point')
        points[key]=p
    used=set();result=[]
    for (cpu,),r in sorted(rows.items()):
        need(r['method']=='heap-metadata-callsite' and n(r,'version')==1 and n(r,'site')==6 and n(r,'epoch')==1 and
             r['boundary']=='operation-plus-declared-clock-boundaries' and
             r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and r['below_budget']=='NOT_ESTABLISHED',
             'isolated heap method/clock/authority')
        count=n(r,'count');seq=n(r,'sequence');cycles=n(r,'cycles');invalid=n(r,'invalid')
        expected=0
        for op in OPERATIONS:
            parent=parents[(cpu,op)]
            need(parent.get('interpretation')=='OBSERVER_INCLUSIVE_STAGE','old heap stage interpretation')
            meta=n(parent,'metadata_count')
            need(meta<=n(parent,'count') and (op in (1,2) or meta==0),'isolated heap metadata count')
            expected+=meta
        need(count==n(r,'expected_count')==expected and expected<=V.U64,'isolated heap missing operation/count')
        upper=V.ceildiv(max(clocks[(role,cpu)]['upper_hz'] for role in (0,1))*1010000,1000000)
        minimum=cycles*10**9//upper
        need(0<upper<=V.U64 and minimum<=V.U64 and n(r,'frequency_valid')==1 and
             n(r,'frequency_upper')==upper and n(r,'minimum_ns')==minimum,'isolated heap lower duration conversion')
        witness=None;epoch=False
        if count:
            need(0<count<=n(r,'last_sequence')<V.U64 and 0<seq<=n(r,'last_sequence') and cycles>0 and
                 n(r,'operation') in (1,2) and n(r,'header_bytes') in (0,32),'isolated heap mandatory maximum')
            keys=[(cpu,seq,e) for e in ('parent_begin','begin','end','parent_end')]
            need(all(k in points for k in keys),'isolated heap missing endpoint');used.update(keys)
            ps=[points[k] for k in keys];a,b,c,d=ps
            need(0<n(a,'tsc')<n(r,'previous_mark')<n(r,'prepared')<n(b,'tsc')<n(c,'tsc')<
                 n(r,'validated')<n(r,'next_mark')<n(d,'tsc') and n(c,'tsc')-n(b,'tsc')==cycles,
                 'isolated heap operation/observer/parent order')
            for p in ps:
                need(n(p,'flags')&512==0,'isolated heap IF state')
                for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock'):
                    need(n(p,key)==n(a,key)<V.U64,'isolated heap continuity: '+key)
            need(n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and
                 all(n(a,k)>0 for k in ('segment','generation','depth','lock')),'isolated heap owner/lock')
            for key in ('queries','recorder_sequence'):
                vals=[n(p,key) for p in ps]
                need(vals==sorted(vals) and vals[-1]<V.U64,'isolated heap hidden observer work')
            bounds=[]
            for role in (0,1):
                local=[p for p in raw['REFERENCE'] if n(p,'role')==role and n(p,'cpu')==cpu and P.eligible(p,cpus)==0]
                need(local,'isolated heap calibration domain');bounds.append(max(n(p,'tsc_after') for p in local))
            epoch=bounds[0]<=n(a,'tsc')<n(d,'tsc')<=bounds[1]
            witness=dict(points=ps,calibration_bounds=bounds,sequence=seq,operation=n(r,'operation'),
                parent_cycles=n(d,'tsc')-n(a,'tsc'),preceding_observer_cycles=n(b,'tsc')-n(r,'previous_mark'),
                following_observer_cycles=n(r,'next_mark')-n(c,'tsc'),header_bytes=n(r,'header_bytes'),
                header_counter_authority='NEW_HEADER_ONLY_NOT_ALL_STORES_OR_DURATION')
        else:
            need(all(n(r,k)==0 for k in ('cycles','sequence','last_sequence','operation','prepared','validated',
                 'previous_mark','next_mark','header_bytes')),'isolated heap erased maximum')
        valid=bool(count and invalid==0 and epoch)
        result.append(dict(cpu=cpu,count=count,invalid=invalid,raw_cycles=cycles,minimum_ns=minimum,
            frequency_upper=upper,epoch_qualified=epoch,witness=witness,
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if valid else 'UNAVAILABLE',
            observed_violation=valid and minimum>=50000,below_budget_acceptance=False))
    need(set(points)==used,'isolated heap unused/invented endpoints')
    return dict(status='PASS',contract=OPERATION_CONTRACT,rows=result,
        interpretation='OPERATION_PLUS_DECLARED_CLOCK_BOUNDARIES',parent_interpretation='OBSERVER_INCLUSIVE_STAGE',
        unique_cause='NOT_ESTABLISHED',implementation_acceptance=False,below_budget_acceptance=False)
