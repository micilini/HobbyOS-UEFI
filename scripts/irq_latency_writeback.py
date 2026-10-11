"""Instruction-only diagnostic minima. Never an accepted whole-window bound."""
import irq_latency_evidence as V
import irq_latency_pairs as P

CONTRACT='continuous-mask-wbinvd-v1'

def verify(raw,cpus):
    clocks=P.clocks(raw,cpus)
    V.require(len(raw['BEGIN'])==1 and raw['BEGIN'][0].get('writeback_method')==CONTRACT,
              'writeback method not declared by this boot')
    states=V.index(raw['CPU'],('cpu',))
    V.require(len(raw['TERMINAL'])==1 and V.number(raw['TERMINAL'][0],'invalid_boundaries')==0 and
              set(states)=={(c,) for c in range(cpus)} and all(V.number(x,'invalid')==0 for x in states.values()),
              'writeback untracked controller transition')
    rows=V.index(raw['WRITEBACK'],('cpu',));callbacks=V.index(raw['CALLBACK'],('cpu','operation'))
    services=V.index(raw['SERVICE'],('generation',))
    V.require(set(rows)=={(c,) for c in range(cpus)},'writeback complete CPU inventory')
    witnesses={};points={}
    for r in raw['WRITEBACK_WITNESS']:
        k=(V.number(r,'cpu'),r['kind'])
        V.require(k not in witnesses and k[1] in ('first','maximum'),'writeback witness identity')
        witnesses[k]=r
    for r in raw['WRITEBACK_POINT']:
        k=(V.number(r,'cpu'),r['kind'],r['endpoint'])
        V.require(k not in points and k[1] in ('first','maximum') and k[2] in ('begin','end'),
                  'writeback point identity')
        points[k]=r
    result=[];expected=set()
    for (cpu,),r in rows.items():
        n=V.number
        V.require(r['method']=='continuous-mask-wbinvd' and n(r,'version')==1 and n(r,'epoch')==1 and
                  r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and r['below_budget']=='NOT_ESTABLISHED',
                  'writeback cannot become whole-window acceptance')
        cb=callbacks[(cpu,3)]
        count=n(r,'count');invalid=n(r,'invalid')
        V.require(count>0 and count==n(cb,'count') and not n(cb,'invalid') and not n(cb,'failures') and
                  n(r,'first_generation')==n(cb,'first_generation') and n(r,'last_generation')==n(cb,'last_generation'),
                  'writeback callback coverage')
        admitted=[s for (g,),s in services.items() if n(r,'first_generation')<=g<=n(r,'last_generation') and
                  n(s,'operation')==3 and int(s['targets'],0)&(1<<cpu)]
        V.require(len(admitted)==count,'writeback service coverage')
        upper=V.ceildiv(max(clocks[(r,cpu)]['upper_hz'] for r in (0,1))*1010000,1000000)
        bounds=[max(n(p,'tsc_after') for p in raw['REFERENCE'] if n(p,'role')==role and
                    n(p,'cpu')==cpu and P.eligible(p,cpus)==0) for role in (0,1)]
        for kind in ('first','maximum'):
            k=(cpu,kind);expected.add(k);w=witnesses[k]
            a,b=(points[(*k,e)] for e in ('begin','end'))
            cycles=n(b,'tsc')-n(a,'tsc');minimum=cycles*1000000000//upper
            V.require(n(a,'tsc')>0 and cycles>0 and n(w,'cycles')==cycles and n(a,'flags')&512==n(b,'flags')&512==0,
                      'writeback endpoint order/IF')
            for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock','recorder_sequence'):
                V.require(n(a,key)==n(b,key)<V.U64,'writeback continuity/observer work: '+key)
            V.require(n(a,'actual_cpu')==cpu and n(a,'epoch')==1 and n(a,'segment') and n(a,'generation') and not n(a,'lock'),
                      'writeback actual owner/CPU/segment')
            V.require(n(a,'queries')+1==n(b,'queries')<V.U64,'writeback exact outside-interval identity query')
            generation=n(w,'request_generation');service=services[(generation,)]
            V.require(n(r,'first_generation')<=generation<=n(r,'last_generation'),'writeback generation outside inventory')
            V.require(n(w,'operation')==n(service,'operation')==3 and n(w,'root')==int(service['root'],0)>0 and
                      n(w,'root')%4096==0 and int(service['targets'],0)&(1<<cpu) and
                      int(service['ack'],0)&(1<<cpu) and n(service,'status')==0,
                      'writeback request operation/root/ACK identity')
            V.require(0<upper<=V.U64 and minimum<=V.U64 and n(w,'frequency_valid')==1 and
                      n(w,'frequency_upper')==upper and n(w,'minimum_ns')==minimum,'writeback lower conversion')
            if kind=='maximum':V.require(cycles==n(r,'maximum_cycles'),'writeback erased maximum')
            else:V.require(generation==n(r,'first_generation'),'writeback erased first operation')
            epoch=bounds[0]<=n(a,'tsc')<n(b,'tsc')<=bounds[1]
            authority=epoch and invalid==0
            result.append(dict(cpu=cpu,kind=kind,count=count,invalid=invalid,request_generation=generation,
                root=n(w,'root'),raw_cycles=cycles,frequency_upper=upper,minimum_ns=minimum,
                calibration_bounds=bounds,epoch_qualified=epoch,begin=a,end=b,
                authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' if authority else 'UNAVAILABLE',
                observed_violation=authority and minimum>=50000,below_budget_acceptance=False))
        first=witnesses[(cpu,'first')];maximum=witnesses[(cpu,'maximum')]
        V.require(n(first,'cycles')<=n(maximum,'cycles'),'writeback maximum below first')
        if n(first,'request_generation')==n(maximum,'request_generation'):
            V.require({k:v for k,v in first.items() if k!='kind'}=={k:v for k,v in maximum.items() if k!='kind'} and
                all({k:v for k,v in points[(cpu,'first',e)].items() if k!='kind'}==
                    {k:v for k,v in points[(cpu,'maximum',e)].items() if k!='kind'} for e in ('begin','end')),
                'same writeback generation changed witness')
    V.require(set(witnesses)==expected and set(points)=={(*k,e) for k in expected for e in ('begin','end')},
              'writeback missing/extra witness')
    return dict(status='PASS',contract=CONTRACT,rows=result,implementation_acceptance=False,
                below_budget_acceptance=False,host_free_cost='NOT_ESTABLISHED')
