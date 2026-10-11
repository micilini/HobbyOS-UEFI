"""Three original clock calls, independently checked diagnostic minima only."""
import irq_latency_evidence as V
import irq_latency_pairs as P
import irq_latency_writeback as B

CONTRACT='continuous-mask-writeback-clock-v1'

def verify(raw,cpus):
    clocks=P.clocks(raw,cpus);instruction=B.verify(raw,cpus)
    V.require(raw['BEGIN'][0].get('writeback_clock_method')==CONTRACT,'clock-call method binding')
    claims=V.index(raw['CLOCK_CALL'],('cpu','site'));points={}
    V.require(set(claims)=={(c,s) for c in range(cpus) for s in range(3)},'clock-call complete site inventory')
    for p in raw['CLOCK_CALL_POINT']:
        key=(V.number(p,'cpu'),V.number(p,'site'),p['endpoint'])
        V.require(key not in points and key[2] in ('begin','end'),'clock-call duplicate/unknown point')
        points[key]=p
    V.require(set(points)=={(*k,e) for k in claims for e in ('begin','end')},'clock-call complete endpoint inventory')
    result=[];n=V.number
    same=('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock','recorder_sequence')
    for (cpu,site),r in claims.items():
        wb=next(w for w in instruction['rows'] if w['cpu']==cpu and w['kind']=='first')
        V.require(r['method']=='continuous-mask-writeback-clock' and n(r,'version')==1 and n(r,'epoch')==1 and
            r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and r['below_budget']=='NOT_ESTABLISHED',
            'clock-call cannot become accepted upper bound')
        V.require(n(r,'count')==wb['count']==1 and n(r,'operation')==3 and
            n(r,'request_generation')==wb['request_generation'] and n(r,'root')==wb['root'],
            'clock-call actual callback/service inventory')
        a,b=(points[(cpu,site,e)] for e in ('begin','end'))
        cycles=max(0,n(b,'tsc')-n(a,'tsc'))
        continuity=(n(a,'tsc')>0 and cycles>0 and not ((n(a,'flags')|n(b,'flags'))&512) and
            all(n(a,k)==n(b,k)<V.U64 for k in same) and n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and
            n(a,'segment')>0 and n(a,'generation')>0 and not n(a,'lock') and
            n(a,'queries')+1==n(b,'queries')<V.U64)
        sample=(n(r,'result')==n(r,'sample_valid')==1 and
            (n(r,'mode')==1 and n(r,'retries')<=4 or n(r,'mode')==2 and not n(r,'retries')))
        V.require(n(r,'result') in (0,1) and n(r,'sample_valid') in (0,1) and
                  n(r,'invalid')==int(not (continuity and sample)),'clock-call raw eligibility claim')
        upper=V.ceildiv(max(clocks[(role,cpu)]['upper_hz'] for role in (0,1))*1010000,1000000)
        minimum=cycles*1000000000//upper
        V.require(0<upper<=V.U64 and minimum<=V.U64 and n(r,'frequency_valid')==1 and
            n(r,'cycles')==cycles and n(r,'frequency_upper')==upper and n(r,'minimum_ns')==minimum,
            'clock-call conservative downward conversion')
        if sample:
            domains={(n(p,'mode'),n(p,'period_fs')) for p in raw['REFERENCE'] if P.eligible(p,cpus)==0}
            V.require(len(domains)==1,'clock-call reference domain')
            mode,period=next(iter(domains));frequency=1000000000000000//period
            V.require(n(r,'mode')==mode and n(r,'ns')==n(r,'ticks')*1000000000//frequency<V.U64,
                'clock-call actual HPET result conversion/domain')
        epoch=wb['calibration_bounds'][0]<=n(a,'tsc')<n(b,'tsc')<=wb['calibration_bounds'][1]
        authority=continuity and sample and epoch and wb['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY'
        result.append(dict(cpu=cpu,site=site,request_generation=n(r,'request_generation'),raw_cycles=cycles,
            frequency_upper=upper,minimum_ns=minimum,begin=a,end=b,ticks=n(r,'ticks'),ns=n(r,'ns'),
            mode=n(r,'mode'),retries=n(r,'retries'),result=n(r,'result'),invalid=n(r,'invalid'),
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if authority else 'UNAVAILABLE',
            observed_violation=authority and minimum>=50000,below_budget_acceptance=False))
    for cpu in range(cpus):
        stages=[next(r for r in result if r['cpu']==cpu and r['site']==s) for s in range(3)]
        wb=next(w for w in instruction['rows'] if w['cpu']==cpu and w['kind']=='first')
        if all(r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' for r in stages):
            V.require(n(stages[0]['end'],'tsc')<n(wb['begin'],'tsc')<n(wb['end'],'tsc')<n(stages[1]['begin'],'tsc') and
                n(stages[1]['end'],'tsc')<n(stages[2]['begin'],'tsc'),'clock-call original site order')
            V.require(all(n(r['begin'],k)==n(wb['begin'],k) for r in stages for k in same if k!='recorder_sequence'),
                'clock-call whole callback continuity identity')
            V.require(all(stages[i]['ticks']<=stages[i+1]['ticks'] for i in range(2)),
                'clock-call HPET chronological order')
    return dict(status='PASS',contract=CONTRACT,rows=result,implementation_acceptance=False,
                below_budget_acceptance=False,host_free_cost='NOT_ESTABLISHED')
