#!/usr/bin/env python3
"""Independent synthetic instruction witnesses; no boot or acceptance receipt."""
import copy,json
from pathlib import Path
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_pairs as P
import irq_latency_writeback as B

def run(interior_raw,output):
    output.mkdir(parents=True,exist_ok=False)
    raw=json.loads(interior_raw.read_text());clocks=P.clocks(raw,2)
    assert len(raw['BEGIN'])==1
    raw['BEGIN'][0]['writeback_method']=B.CONTRACT
    raw['WRITEBACK']=[];raw['WRITEBACK_WITNESS']=[];raw['WRITEBACK_POINT']=[];raw['CALLBACK']=[]
    raw['SERVICE']=[dict(generation='7',operation='3',targets='0x3',ack='0x3',root='0x1000',status='0')]
    for cpu in range(2):
        upper=V.ceildiv(max(clocks[(r,cpu)]['upper_hz'] for r in (0,1))*1010000,1000000)
        start=max(V.number(p,'tsc_after') for p in raw['REFERENCE'] if p['role']=='0' and int(p['cpu'])==cpu)+1000000
        cycles=V.ceildiv(50001*upper,1000000000)
        raw['WRITEBACK'].append(dict(cpu=str(cpu),method='continuous-mask-wbinvd',version='1',epoch='1',
            count='1',invalid='0',maximum_cycles=str(cycles),first_generation='7',last_generation='7',
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',below_budget='NOT_ESTABLISHED'))
        raw['CALLBACK'].append(dict(cpu=str(cpu),operation='3',count='1',invalid='0',failures='0',first_generation='7',last_generation='7'))
        for kind in ('first','maximum'):
            raw['WRITEBACK_WITNESS'].append(dict(cpu=str(cpu),kind=kind,operation='3',request_generation='7',root='4096',
                cycles=str(cycles),frequency_upper=str(upper),minimum_ns=str(cycles*1000000000//upper),frequency_valid='1'))
            for end in range(2):
                raw['WRITEBACK_POINT'].append(dict(cpu=str(cpu),kind=kind,endpoint='end' if end else 'begin',
                    tsc=str(start+end*cycles),flags='2',actual_cpu=str(cpu),epoch='1',segment='4',switches='3',entries='7',
                    faults='0',owner='112',generation='2',depth='0',lock='0',queries=str(20+end),recorder_sequence='40'))
    positive=B.verify(raw,2);assert all(r['observed_violation'] and not r['below_budget_acceptance'] for r in positive['rows'])
    cases=[dict(name='fully-bound-instruction-minimum',status='PASS')]
    mutations=[('missing-ledger',lambda r:r['WRITEBACK'].pop()),('missing-endpoint',lambda r:r['WRITEBACK_POINT'].pop()),
        ('duplicate-endpoint',lambda r:r['WRITEBACK_POINT'].append(r['WRITEBACK_POINT'][0])),
        ('missing-service',lambda r:r['SERVICE'].clear()),('wrong-operation',lambda r:r['SERVICE'][0].update(operation='1')),
        ('wrong-root',lambda r:r['SERVICE'][0].update(root='0x2000')),('missing-ack',lambda r:r['SERVICE'][0].update(ack='0')),
        ('unknown-method',lambda r:r['BEGIN'][0].update(writeback_method='other')),
        ('erased-count',lambda r:r['WRITEBACK'][0].update(count='0')),
        ('hidden-callback',lambda r:r['CALLBACK'][0].update(count='2')),
        ('promoted-upper-bound',lambda r:r['WRITEBACK'][0].update(below_budget='PASS')),
        ('enclosing-relabel',lambda r:r['WRITEBACK'][0].update(method='ordered-tsc-irq-envelope')),
        ('wrong-frequency',lambda r:r['WRITEBACK_WITNESS'][0].update(frequency_upper='1')),
        ('wrong-rounding',lambda r:r['WRITEBACK_WITNESS'][0].update(minimum_ns='50000')),
        ('erased-maximum',lambda r:r['WRITEBACK'][0].update(maximum_cycles='0')),
        ('untracked-transition',lambda r:r['TERMINAL'][0].update(invalid_boundaries='1')),
        ('clock-failure',lambda r:r['TIMEBASE'][0].update(clock_errors='1'))]
    for field,value in [('flags','512'),('actual_cpu','1'),('epoch','2'),('segment','5'),('switches','4'),
        ('entries','8'),('faults','1'),('owner','113'),('generation','3'),('depth','1'),('lock','4096'),
        ('queries','22'),('recorder_sequence','41'),('tsc','0')]:
        mutations.append(('continuity-'+field,lambda r,f=field,v=value:r['WRITEBACK_POINT'][1].update({f:v})))
    for name,change in mutations:
        altered=copy.deepcopy(raw);change(altered)
        try:B.verify(altered,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    altered=copy.deepcopy(raw);altered['WRITEBACK'][0]['invalid']='1'
    assert not any(r['observed_violation'] for r in B.verify(altered,2)['rows'] if r['cpu']==0)
    cases.append(dict(name='invalid-is-unavailable',status='PASS'))
    for threshold in (49999,50000,50001):
        altered=copy.deepcopy(raw)
        for w in altered['WRITEBACK_WITNESS']:
            cycles=V.ceildiv(threshold*int(w['frequency_upper']),1000000000)
            w.update(cycles=str(cycles),minimum_ns=str(threshold))
            cpu=w['cpu'];kind=w['kind'];altered['WRITEBACK'][int(cpu)]['maximum_cycles']=str(cycles)
            a=next(p for p in altered['WRITEBACK_POINT'] if p['cpu']==cpu and p['kind']==kind and p['endpoint']=='begin')
            b=next(p for p in altered['WRITEBACK_POINT'] if p['cpu']==cpu and p['kind']==kind and p['endpoint']=='end')
            b['tsc']=str(int(a['tsc'])+cycles)
        rows=B.verify(altered,2)['rows'];assert all(r['observed_violation']==(threshold>=50000) and not r['below_budget_acceptance'] for r in rows)
        cases.append(dict(name='strict-minimum-'+str(threshold),status='PASS'))
    E.write(output/'raw.json',raw);E.write(output/'positive.json',positive)
    result=dict(status='PASS',cases=cases,authority='HOSTED_SYNTHETIC_ONLY',implementation_acceptance=False)
    E.write(output/'result.json',result);return result
