#!/usr/bin/env python3
"""Heap-stage verifier controls using synthetic clocks; no new guest evidence."""
import copy,json
from pathlib import Path
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_pairs as P
import irq_latency_heap as H

def run(interior_raw,output):
    output.mkdir(parents=True,exist_ok=False)
    raw=json.loads(interior_raw.read_text());raw['BEGIN'][0]['heap_method']=H.CONTRACT
    raw['TERMINAL'][0]['transport']='COMPLETE'
    raw['HEAP_STAGE']=[];raw['HEAP_POINT']=[];raw['HEAP_MARK']=[]
    clocks=P.clocks(raw,2)
    for cpu in range(2):
        upper=V.ceildiv(max(clocks[(r,cpu)]['upper_hz'] for r in (0,1))*1010000,1000000)
        source=[p for p in raw['INTERIOR_POINT'] if int(p['cpu'])==cpu and p['path']=='0']
        for op in range(1,8):
            active=op==3;start=int(source[0]['tsc']);cycles=V.ceildiv(50001*upper,10**9) if active else 0
            raw['HEAP_STAGE'].append(dict(cpu=str(cpu),operation=str(op),method='continuous-mask-heap-stages',
                version='1',epoch='1',count=str(int(active)),invalid='0',last_sequence=str(int(active)),
                sequence=str(int(active)),cycles=str(cycles),marks='4' if active else '0',
                wait_start=str(start-100) if active else '0',acquired=str(start-50) if active else '0',
                pending_begin='0',pending_end='0',requested_pages='0',frequency_upper=str(upper),
                minimum_ns=str(cycles*10**9//upper),frequency_valid='1',
                authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',below_budget='NOT_ESTABLISHED'))
            if not active:continue
            for i,p in enumerate(source):
                q={k:v for k,v in p.items() if k!='path'};q.update(operation=str(op),sequence='1',tsc=str(start+i*cycles))
                raw['HEAP_POINT'].append(q)
            for i,site in enumerate((1,3,4,5)):
                raw['HEAP_MARK'].append(dict(cpu=str(cpu),operation=str(op),sequence='1',index=str(i),
                    site=str(site),tsc=str(start+cycles*(i+1)//5),work=str(100 if site==3 else 1),bytes='0'))
    result=H.verify(raw,2)
    assert all(r['observed_violation'] for r in result['rows'] if r['operation']==3)
    assert not result['implementation_acceptance'] and not result['below_budget_acceptance']
    cases=[dict(name='bound-synthetic-heap-minimum',status='PASS')]
    for name,change in (
        ('unknown-method',lambda r:r['BEGIN'][0].update(heap_method='legacy')),
        ('old-enclosing-witness',lambda r:r['HEAP_STAGE'][2].update(method='ordered-tsc-irq-envelope')),
        ('promoted-small-subset',lambda r:r['HEAP_STAGE'][2].update(below_budget='PASS')),
        ('frequency-lower-divisor',lambda r:r['HEAP_STAGE'][2].update(frequency_upper='1')),
        ('missing-operation',lambda r:r['HEAP_STAGE'].pop()),
        ('missing-endpoint',lambda r:r['HEAP_POINT'].pop()),
        ('missing-mark',lambda r:r['HEAP_MARK'].pop()),
        ('duplicate-mark',lambda r:r['HEAP_MARK'].append(r['HEAP_MARK'][0])),
        ('foreign-event',lambda r:r['HEAP_MARK'][0].update(sequence='2')),
        ('wrong-stage',lambda r:r['HEAP_MARK'][0].update(site='2')),
        ('stage-after-unlock',lambda r:r['HEAP_MARK'][0].update(tsc=r['HEAP_POINT'][1]['tsc'])),
        ('migration',lambda r:r['HEAP_POINT'][1].update(actual_cpu='1')),
        ('stale-owner',lambda r:r['HEAP_POINT'][1].update(generation='3')),
        ('hidden-enable',lambda r:r['HEAP_POINT'][1].update(segment='5')),
        ('untracked-transition',lambda r:r['TERMINAL'][0].update(invalid_boundaries='1')),
        ('hidden-observer-work',lambda r:r['HEAP_POINT'][1].update(queries='0')),
        ('mandatory-buffer-exhaustion',lambda r:r['HEAP_STAGE'][2].update(marks='9')),
        ('erased-maximum',lambda r:r['HEAP_STAGE'][2].update(count='0')),
        ('lost-terminal',lambda r:r['TERMINAL'][0].update(transport='INCOMPLETE'))):
        altered=copy.deepcopy(raw);change(altered)
        try:H.verify(altered,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    altered=copy.deepcopy(raw);altered['HEAP_STAGE'][2]['invalid']='1'
    assert not H.verify(altered,2)['rows'][2]['observed_violation']
    cases.append(dict(name='invalid-continuity-unavailable',status='PASS'))
    E.write(output/'raw.json',raw);E.write(output/'positive.json',result)
    receipt=dict(status='PASS',authority='HOSTED_SYNTHETIC_ONLY',cases=cases,raw_sha256=E.sha(output/'raw.json'),
        implementation_acceptance=False)
    E.write(output/'result.json',receipt);return receipt
