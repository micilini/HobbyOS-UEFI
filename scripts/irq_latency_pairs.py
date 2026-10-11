"""Independent integer reconstruction of the fixed local endpoint ledger.

Boot/candidate binding is the complete gate's responsibility, never inferred
from worker IDs or these frequency calculations. No global replay-mode switch.
"""
from collections import Counter, defaultdict
import irq_latency_evidence as V

METHOD = 'cpu-local-reference-pairs'
VERSION = 1
NIL = 2**32-1


def method(raw):
    V.require(len(raw['BEGIN']) == 1, 'pairing activation identity')
    begin = raw['BEGIN'][0]
    name = begin.get('calibration')
    if name is None:
        V.require(not any(raw[k] for k in ('PAIRING','PAIR','POINT_DISPOSITION')), 'legacy method with current pairing records')
        return None
    V.require(name == METHOD and V.number(begin,'calibration_version') == VERSION and
              V.number(begin,'epoch') == 1 and V.number(begin,'witness_cells') == 29 and
              begin.get('schema') == '2', 'unknown pairing method/version or downgraded witness')
    return name


def eligible(p, cpus):
    n = lambda k: V.number(p,k)
    V.require(n('acquired') == 1, 'point never acquired: incomplete ledger')
    reason = 0
    if not all(n(k) == 1 for k in ('acquired','sample_valid','initialized')): reason |= 1
    if not all(n(k) == 1 for k in ('if_enabled','if_after')): reason |= 2
    if not (n('identity_valid') == 1 and n('cpu') < cpus and n('cpu') == n('end_cpu') and
            n('switch_before') == n('switch_after')): reason |= 4
    if not ((n('mode') == 1 and n('bits') == 64 and n('retries') <= 4) or
            (n('mode') == 2 and n('bits') == 32 and n('retries') == 0)): reason |= 8
    if not (0 < n('tsc_before') < n('tsc_after')): reason |= 16
    V.require(n('valid') == int(reason == 0), 'point outcome differs from acquisition facts')
    return reason


def derive(points, cpus):
    """Pure expected-value implementation; takes every point in ID order."""
    groups = defaultdict(list); domain = set(); dispositions = []; pairs = []
    for ident, p in enumerate(points):
        reason = eligible(p,cpus)
        dispositions.append(dict(kind=1,reason=reason,pair=NIL))
        if reason: continue
        period = V.number(p,'period_fs')
        V.require(0 < period <= 100000000 and period == V.number(p,'period_after'), 'point period or arithmetic range')
        domain.add((period,V.number(p,'mode')))
        groups[V.number(p,'cpu')].append(ident)
    V.require(len(domain) <= 1, 'contradictory timebase domain')
    for cpu in sorted(groups):
        ids = sorted(groups[cpu],key=lambda i:(V.number(points[i],'tsc_before'),i))
        for a,b in zip(ids,ids[1:]):
            V.require(V.number(points[b],'tsc_before') > V.number(points[a],'tsc_after') and
                      V.number(points[b],'hpet') >= V.number(points[a],'hpet'), 'local point order/HPET monotonicity')
        position = 0
        while position < len(ids):
            start = ids[position]; a = points[start]; end_position = None
            target_ticks = V.ceildiv(20000000*1000000,V.number(a,'period_fs'))
            for j in range(position+1,len(ids)):
                b = points[ids[j]]
                if V.number(b,'hpet')-V.number(a,'hpet') >= target_ticks:
                    end_position = j; break
            if end_position is None:
                for ident in ids[position:]: dispositions[ident]['kind'] = 5
                break
            end = ids[end_position]
            try: bounds = V.interval(a,points[end]); valid = 1; reason = None
            except ValueError as error: bounds = None; valid = 0; reason = str(error)
            pair_id = len(pairs)
            pairs.append(dict(id=pair_id,begin=start,end=end,cpu=cpu,valid=valid,bounds=bounds,reason=reason))
            for j in range(position,end_position+1):
                dispositions[ids[j]] = dict(kind=2 if j==position else 3 if j==end_position else 4,reason=0,pair=pair_id)
            position = end_position+1
    return pairs,dispositions


def ledger(raw,role,cpus):
    V.require(method(raw) == METHOD, 'current pairing required')
    headers = V.index(raw['PAIRING'],('role',))
    V.require((role,) in headers, 'pairing role missing')
    header = headers[(role,)]
    fixed = dict(epoch=1,version=1,attempts=64*cpus,points=128*cpus,probes=256*cpus,
                 workers=cpus,target_ns=20000000,deadline_ms=120000,complete=1)
    V.require(header.get('method') == METHOD and all(V.number(header,k)==v for k,v in fixed.items()),
              'pairing plan or complete publication')
    select = lambda key: [r for r in raw[key] if V.number(r,'role') == role]
    attempts = V.index(select('WINDOW'),('index',))
    V.require(set(attempts) == {(i,) for i in range(64*cpus)}, 'complete acquisition ledger')
    refs = {}
    for p in select('REFERENCE'):
        V.require(p['endpoint'] in ('begin','end'), 'point endpoint name')
        ident = 2*V.number(p,'index') + int(p['endpoint']=='end')
        V.require(ident not in refs, 'duplicate point identity')
        refs[ident] = p
    V.require(set(refs) == set(range(128*cpus)), 'complete reference ledger')
    for (i,),a in attempts.items():
        V.require(V.number(a,'complete')==1 and V.number(a,'worker')<cpus and
                  V.number(a,'acquisition') in (1,2,3,4), 'acquisition worker/outcome')
        try: V.interval(refs[2*i],refs[2*i+1]); valid=1
        except ValueError: valid=0
        V.require(V.number(a,'valid')==valid, 'legacy attempt verdict changed')
    pairs,dispositions = derive([refs[i] for i in range(128*cpus)],cpus)
    V.require(V.number(header,'pairs') == len(pairs), 'pair count differs')
    claims = V.index(select('PAIR'),('id',)); ds = V.index(select('POINT_DISPOSITION'),('id',))
    V.require(set(claims)=={(i,) for i in range(len(pairs))}, 'complete pair ledger')
    V.require(set(ds)=={(i,) for i in refs}, 'complete disposition ledger')
    for p in pairs:
        V.require(all(V.number(claims[(p['id'],)],k)==p[k] for k in ('begin','end','cpu','valid')),
                  'deterministic earliest pair outcome')
    for i,d in enumerate(dispositions):
        V.require(all(V.number(ds[(i,)],k)==v for k,v in d.items()), 'point disposition differs')
    return refs,pairs,dispositions


def role_result(raw,role,cpus,before=None):
    refs,pairs,dispositions = ledger(raw,role,cpus)
    select = lambda key: [r for r in raw[key] if V.number(r,'role') == role]
    probes = V.index(select('OVERHEAD'),('index','probe'))
    V.require(set(probes)=={(i,j) for i in range(64*cpus) for j in range(4)}, 'complete original probe ledger')
    costs=defaultdict(list)
    for (i,j),p in probes.items():
        a,b,c,d=(V.number(p,k) for k in ('outer_begin','inner_begin','inner_end','outer_end'))
        good=(a<=b<c<=d and V.number(p,'cpu')<cpus and p['cpu']==p['end_cpu'] and
              p['switch_before']==p['switch_after'] and
              V.number(p,'sequence_after')==V.number(p,'sequence_before')+1 and
              V.number(p,'irq_after')==V.number(p,'irq_before')+1)
        V.require(V.number(p,'valid')==int(good), 'original probe eligibility')
        if good: costs[V.number(p,'cpu')].append(dict(index=i,probe=j,leading=b-a,interior=c-b,trailing=d-c,
            envelope=b-a+d-c,outer_begin=a,inner_begin=b,inner_end=c,outer_end=d))
    caps=V.index(select('CAPABILITY'),('cpu',)); reported=V.index(select('TIMEBASE'),('cpu',))
    V.require(set(caps)==set(reported)=={(c,) for c in range(cpus)}, 'independent topology coverage')
    result={}; details=[]
    for cpu in range(cpus):
        cap=caps[(cpu,)]; candidates=[p for p in pairs if p['cpu']==cpu and p['valid']]
        chosen=candidates[:3]; errors=0
        if not (V.number(cap,'seen')==1 and int(cap['basic'],16)>=1 and int(cap['leaf1_edx'],16)&16): errors|=1
        if len(chosen)<3: errors|=2
        lower=upper=estimate=conversion=declared=0
        if not errors:
            lo=max(p['bounds'][0] for p in chosen); hi=min(p['bounds'][1] for p in chosen)
            dec=0
            if int(cap['basic'],16)>=0x15 and all(V.number(cap,k) for k in ('denominator','numerator','crystal_hz')):
                dec=V.number(cap,'crystal_hz')*V.number(cap,'numerator')//V.number(cap,'denominator')
            if lo>hi or (dec and not lo<=dec<=hi): errors|=4
            else:
                lower,upper,declared=lo,hi,dec; estimate=lo+(hi-lo)//2;conversion=lo*990000//1000000
        if not errors:
            for p in candidates:
                lo,hi,middle=p['bounds']
                if lo<conversion or (declared and not lo<=declared<=hi): errors|=8;break
                if V.ceildiv(abs(middle-estimate)*1000000,estimate)>10000: errors|=16;break
        if role and not errors:
            V.require(before is not None, 'missing pre clock')
            pre=before[cpu]
            if (declared!=pre['declared_hz'] or lower<pre['conversion_hz'] or not pre['estimate_hz'] or
                V.ceildiv(abs(estimate-pre['estimate_hz'])*1000000,pre['estimate_hz'])>10000):errors|=16
        worst=max(costs[cpu],key=lambda p:p['envelope']) if costs[cpu] else None
        overhead=worst['envelope'] if worst else 0
        observer=(1 if len(costs[cpu])<32 else 0) | (2 if not 0<overhead<=V.U64//2 else 0)
        if role and overhead>2*before[cpu]['overhead_max']:observer|=4
        row=dict(lower_hz=lower,upper_hz=upper,estimate_hz=estimate,conversion_hz=conversion,declared_hz=declared,
                 overhead_max=overhead,windows=len(candidates),probes=len(costs[cpu]),clock_errors=errors,
                 observer_errors=observer,**{'selected'+str(i):chosen[i]['id'] if i<len(chosen) else 0 for i in range(3)})
        V.require(all(V.number(reported[(cpu,)],k)==v for k,v in row.items()), 'current clock/probe summary differs')
        result[cpu]=row
        details.append(dict(cpu=cpu,locally_valid_points=sum(eligible(p,cpus)==0 and V.number(p,'cpu')==cpu for p in refs.values()),
            pairs=sum(p['cpu']==cpu for p in pairs),eligible_pairs=len(candidates),eligible_probes=len(costs[cpu]),
            worst_probe=worst,clock_errors=errors,observer_errors=observer,selected=[p['id'] for p in chosen]))
    valid=not any(r['clock_errors'] or r['observer_errors'] for r in result.values())
    V.require(all(V.number(p,'series_valid')==int(valid) for p in reported.values()), 'current aggregate qualification differs')
    return result,dict(role=role,valid=valid,cpus=details,pairs=pairs,
                      dispositions=dict(Counter(d['kind'] for d in dispositions)),timing_acceptance=False)


def clocks(raw,cpus):
    V.require({V.number(r,'role') for r in raw['PAIRING']}=={0,1}, 'pre/post pair roles')
    before,_=role_result(raw,0,cpus);after,_=role_result(raw,1,cpus,before)
    V.require(not any(r['clock_errors'] for group in (before,after) for r in group.values()), 'current frequency qualification failed')
    return {(role,cpu):row for role,group in enumerate((before,after)) for cpu,row in group.items()}
