#!/usr/bin/env python3
"""Independent synthetic operation brackets; never guest timing receipts."""
import copy,json
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_pairs as P
import irq_latency_heap as H

def run(heap_raw,output):
    output.mkdir(parents=True,exist_ok=False)
    raw=json.loads(heap_raw.read_text());raw['BEGIN'][0]['heap_operation_method']=H.OPERATION_CONTRACT
    raw['HEAP_OPERATION']=[];raw['HEAP_OPERATION_POINT']=[];clocks=P.clocks(raw,2)
    for r in raw['HEAP_STAGE']:r.update(metadata_count='0',interpretation='OBSERVER_INCLUSIVE_STAGE')
    for cpu in range(2):
        upper=V.ceildiv(max(clocks[(role,cpu)]['upper_hz'] for role in (0,1))*1010000,1000000)
        cycles=V.ceildiv(50001*upper,10**9)
        parent=next(r for r in raw['HEAP_STAGE'] if r['cpu']==str(cpu) and r['operation']=='1')
        templates=[r for r in raw['HEAP_POINT'] if r['cpu']==str(cpu) and r['operation']=='3']
        start=int(templates[0]['tsc']);end=start+cycles+100
        parent.update(count='1',sequence='1',last_sequence='1',marks='2',metadata_count='1',
            cycles=str(end-start),minimum_ns=str((end-start)*10**9//upper),wait_start=str(start-100),acquired=str(start-50))
        for p,t in zip(templates,(start,end)):
            raw['HEAP_POINT'].append(dict(p,operation='1',tsc=str(t)))
        for i,(site,t) in enumerate(((2,start+10),(6,end-10))):
            raw['HEAP_MARK'].append(dict(cpu=str(cpu),operation='1',sequence='1',index=str(i),site=str(site),tsc=str(t),work='1',bytes='32' if site==6 else '0'))
        raw['HEAP_OPERATION'].append(dict(cpu=str(cpu),method='heap-metadata-callsite',version='1',site='6',epoch='1',
            boundary='operation-plus-declared-clock-boundaries',count='1',expected_count='1',invalid='0',last_sequence='1',
            sequence='1',operation='1',cycles=str(cycles),prepared=str(start+20),validated=str(end-20),previous_mark=str(start+10),
            next_mark=str(end-10),header_bytes='32',frequency_upper=str(upper),minimum_ns=str(cycles*10**9//upper),frequency_valid='1',
            authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',below_budget='NOT_ESTABLISHED'))
        for side,t in (('parent_begin',start),('begin',start+30),('end',start+30+cycles),('parent_end',end)):
            raw['HEAP_OPERATION_POINT'].append(dict(templates[0],sequence='1',endpoint=side,tsc=str(t)))
    result=H.verify_operation(raw,2);assert all(r['observed_violation'] for r in result['rows'])
    cases=[dict(name='isolated-operation-positive',status='PASS')]
    mutations=[('legacy-relabel',lambda r:r['BEGIN'][0].update(heap_operation_method=H.CONTRACT)),
        ('unknown-version',lambda r:r['HEAP_OPERATION'][0].update(version='2')),
        ('wrong-boundary',lambda r:r['HEAP_OPERATION'][0].update(boundary='pure-instruction-time')),
        ('missing-operation',lambda r:r['HEAP_OPERATION'].pop()),
        ('missing-end',lambda r:r['HEAP_OPERATION_POINT'].pop(2)),
        ('duplicate-point',lambda r:r['HEAP_OPERATION_POINT'].append(r['HEAP_OPERATION_POINT'][0])),
        ('erased-maximum',lambda r:r['HEAP_OPERATION'][0].update(cycles='0')),
        ('missing-operation-count',lambda r:r['HEAP_OPERATION'][0].update(count='0')),
        ('wrong-count',lambda r:r['HEAP_OPERATION'][0].update(expected_count='2')),
        ('prior-marker-tail-included',lambda r:r['HEAP_OPERATION'][0].update(prepared=r['HEAP_OPERATION_POINT'][1]['tsc'])),
        ('publication-included',lambda r:r['HEAP_OPERATION'][0].update(validated=r['HEAP_OPERATION_POINT'][2]['tsc'])),
        ('invented-header-bytes',lambda r:r['HEAP_OPERATION'][0].update(header_bytes='64')),
        ('wrong-frequency-direction',lambda r:r['HEAP_OPERATION'][0].update(frequency_upper='1')),
        ('old-body-exclusive',lambda r:r['HEAP_STAGE'][0].update(interpretation='EXCLUSIVE')),
        ('minimum-promoted',lambda r:r['HEAP_OPERATION'][0].update(below_budget='PASS')),
        ('host-time-subtracted',lambda r:r['HEAP_OPERATION'][0].update(cycles=str(int(r['HEAP_OPERATION'][0]['cycles'])-1)))]
    for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock','flags'):
        mutations.append(('changed-'+key,lambda r,k=key:r['HEAP_OPERATION_POINT'][2].update({k:str(int(r['HEAP_OPERATION_POINT'][2][k])+ (512 if k=='flags' else 1))})))
    for name,change in mutations:
        r=copy.deepcopy(raw);change(r)
        try:H.verify_operation(r,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for ns in (49999,50000,50001):
        r=copy.deepcopy(raw)
        for cpu in range(2):
            row=r['HEAP_OPERATION'][cpu];cycles=V.ceildiv(ns*int(row['frequency_upper']),10**9)
            pts=[p for p in r['HEAP_OPERATION_POINT'] if p['cpu']==str(cpu)]
            pts[2]['tsc']=str(int(pts[1]['tsc'])+cycles);row.update(cycles=str(cycles),minimum_ns=str(cycles*10**9//int(row['frequency_upper'])))
        outcome=H.verify_operation(r,2)
        assert all(v['observed_violation']==(ns>=50000) and not v['below_budget_acceptance'] for v in outcome['rows'])
        assert not outcome['implementation_acceptance'];cases.append(dict(name='strict-diagnostic-threshold-'+str(ns),status='PASS'))
    E.write(output/'raw.json',raw);E.write(output/'positive.json',result)
    receipt=dict(status='PASS',authority='HOSTED_SYNTHETIC_ONLY',cases=cases,implementation_acceptance=False)
    E.write(output/'result.json',receipt);return receipt
