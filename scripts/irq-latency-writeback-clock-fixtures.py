#!/usr/bin/env python3
"""Synthetic clock-call discrimination and current-method binding; no boot."""
import copy,json
from pathlib import Path
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_writeback_clock as H

def run(writeback_raw,output):
    output.mkdir(parents=True,exist_ok=False)
    raw=json.loads(writeback_raw.read_text());raw['BEGIN'][0]['writeback_clock_method']=H.CONTRACT
    raw['CLOCK_CALL']=[];raw['CLOCK_CALL_POINT']=[]
    for wb in raw['WRITEBACK_WITNESS']:
        if wb['kind']!='first':continue
        cpu=int(wb['cpu']);upper=int(wb['frequency_upper']);cycles=V.ceildiv(50001*upper,1000000000)
        points=[p for p in raw['WRITEBACK_POINT'] if int(p['cpu'])==cpu and p['kind']=='first']
        a=next(p for p in points if p['endpoint']=='begin');b=next(p for p in points if p['endpoint']=='end')
        period=next(int(p['period_fs']) for p in raw['REFERENCE'] if int(p['cpu'])==cpu and p['valid']=='1')
        for site in range(3):
            start=int(a['tsc'])-cycles-10 if site==0 else int(b['tsc'])+10+(site-1)*(cycles+10)
            ticks=(site+1)*100
            raw['CLOCK_CALL'].append(dict(cpu=str(cpu),site=str(site),method='continuous-mask-writeback-clock',version='1',epoch='1',
                count='1',invalid='0',operation='3',request_generation=wb['request_generation'],root=wb['root'],cycles=str(cycles),
                result='1',ticks=str(ticks),ns=str(ticks*1000000000//(1000000000000000//period)),mode='1',retries='0',sample_valid='1',
                frequency_upper=str(upper),minimum_ns='50001',frequency_valid='1',authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',below_budget='NOT_ESTABLISHED'))
            for end in range(2):
                p={k:v for k,v in a.items() if k not in ('kind','endpoint')}
                p.update(site=str(site),endpoint='end' if end else 'begin',tsc=str(start+end*cycles),queries=str(30+site*2+end))
                raw['CLOCK_CALL_POINT'].append(p)
    positive=H.verify(raw,2);assert all(r['observed_violation'] and not r['below_budget_acceptance'] for r in positive['rows'])
    cases=[dict(name='all-bound-clock-call-sites',status='PASS')]
    changes=[('missing-site',lambda r:r['CLOCK_CALL'].pop()),('missing-point',lambda r:r['CLOCK_CALL_POINT'].pop()),
        ('duplicate-site',lambda r:r['CLOCK_CALL'].append(r['CLOCK_CALL'][0])),
        ('duplicate-point',lambda r:r['CLOCK_CALL_POINT'].append(r['CLOCK_CALL_POINT'][0])),
        ('unknown-method',lambda r:r['BEGIN'][0].update(writeback_clock_method='unknown')),
        ('hidden-extra-call',lambda r:r['CLOCK_CALL'][0].update(count='2')),
        ('wrong-service',lambda r:r['CLOCK_CALL'][0].update(request_generation='8')),
        ('wrong-root',lambda r:r['CLOCK_CALL'][0].update(root='8192')),
        ('wrong-operation',lambda r:r['CLOCK_CALL'][0].update(operation='1')),
        ('wrong-hpet-result',lambda r:r['CLOCK_CALL'][0].update(ns='1')),
        ('excess-retries',lambda r:r['CLOCK_CALL'][0].update(retries='5')),
        ('hidden-failed-read',lambda r:r['CLOCK_CALL'][0].update(result='0')),
        ('invented-validity',lambda r:r['CLOCK_CALL'][0].update(sample_valid='0')),
        ('false-upper-pass',lambda r:r['CLOCK_CALL'][0].update(below_budget='PASS')),
        ('wrong-frequency-direction',lambda r:r['CLOCK_CALL'][0].update(frequency_upper='1')),
        ('upward-rounding',lambda r:r['CLOCK_CALL'][0].update(minimum_ns='50002')),
        ('inconsistent-invalid',lambda r:r['CLOCK_CALL'][0].update(invalid='1')),
        ('wrong-site-order',lambda r:[p.update(site=str(1-int(p['site']))) for p in r['CLOCK_CALL_POINT'] if int(p['site'])<2])]
    for key in ('actual_cpu','epoch','segment','switches','entries','faults','owner','generation','depth','lock','queries','recorder_sequence','flags'):
        changes.append(('continuity-'+key,lambda r,k=key:r['CLOCK_CALL_POINT'][1].update({k:'512' if k=='flags' else str(int(r['CLOCK_CALL_POINT'][1][k])+1)})))
    for name,change in changes:
        altered=copy.deepcopy(raw);change(altered)
        try:H.verify(altered,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for key in ('segment','recorder_sequence'):
        altered=copy.deepcopy(raw);altered['CLOCK_CALL_POINT'][1][key]=str(int(altered['CLOCK_CALL_POINT'][1][key])+1)
        altered['CLOCK_CALL'][0]['invalid']='1'
        assert H.verify(altered,2)['rows'][0]['authority']=='UNAVAILABLE'
        cases.append(dict(name='retained-invalid-'+key,status='PASS'))
    altered=copy.deepcopy(raw);altered['CLOCK_CALL'][0].update(result='0',sample_valid='0',invalid='1')
    assert H.verify(altered,2)['rows'][0]['authority']=='UNAVAILABLE'
    cases.append(dict(name='retained-hpet-failure',status='PASS'))
    for threshold in (49999,50000,50001):
        altered=copy.deepcopy(raw)
        for r in altered['CLOCK_CALL']:
            cycles=V.ceildiv(threshold*int(r['frequency_upper']),1000000000)
            r.update(cycles=str(cycles),minimum_ns=str(threshold))
            ps=[p for p in altered['CLOCK_CALL_POINT'] if p['cpu']==r['cpu'] and p['site']==r['site']]
            a=next(p for p in ps if p['endpoint']=='begin');b=next(p for p in ps if p['endpoint']=='end')
            b['tsc']=str(int(a['tsc'])+cycles)
        assert all(r['observed_violation']==(threshold>=50000) and not r['below_budget_acceptance'] for r in H.verify(altered,2)['rows'])
        cases.append(dict(name='minimum-threshold-'+str(threshold),status='PASS'))
    E.write(output/'raw.json',raw);E.write(output/'positive.json',positive)
    result=dict(status='PASS',cases=cases,authority='HOSTED_SYNTHETIC_ONLY',implementation_acceptance=False)
    E.write(output/'result.json',result);return result
