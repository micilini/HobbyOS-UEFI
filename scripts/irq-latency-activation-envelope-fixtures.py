#!/usr/bin/env python3
"""Connect actual controller activation records to the independent raw checker."""
import copy
import json
from collections import defaultdict
import memory_evidence as E
import irq_latency_evidence as V


def dataset(witness):
    raw=defaultdict(list)
    raw['BEGIN']=[dict(schema='2',witness_cells='29',primitive='ordered-tsc-irq-envelope')]
    raw['CPU']=[dict(cpu='0',epoch='1',sequence='8',failures='0',cutoff='1000')]
    cycles=witness[1]-witness[0]+witness[14]
    for path in range(14):
        active=path==11
        raw['MAXIMUM'].append(dict(cpu='0',path=str(path),count=str(int(active)),invalid='0',violations='0',
            maximum_cycles=str(cycles if active else 0),maximum_ns=str(cycles if active else 0),
            frequency_hz='1000000000',ns_available='1',measurement_qualified='1',
            status='EXCLUDED_CONTEXT' if path>=11 else 'INVALID_COVERAGE'))
        cells=witness if active else [0]*29
        for first in range(0,29,8):
            raw['WITNESS'].append(dict(cpu='0',kind='maximum',index=str(path),first=str(first),
                count=str(len(cells[first:first+8])),cells=','.join(map(str,cells[first:first+8]))))
    return raw


def run(hosted,output):
    output.mkdir(parents=True,exist_ok=False);cases=[];clock={(0,0):dict(overhead_max=20,conversion_hz=10**9),(1,0):dict(observer_errors=0)}
    ledger=json.loads((hosted/'irq-latency-host.json').read_text())
    actual=[r for r in ledger['executions'] if r['subject']=='activation-envelope']
    E.require({r['mode'] for r in actual}=={'normal','ubsan','asan'} and len(actual)==3,'actual activation execution inventory')
    for row in actual:
        path=hosted/row['execute']/'stdout.log'
        E.require(E.sha(path)==row['stdout_sha256'],'activation raw controller output changed')
        line=path.read_text();E.require(line.startswith('EARLY_WITNESS '),'activation actual record missing')
        w=json.loads(line.split(' ',1)[1]);V.witness_envelope(w,0,clock)
        E.require(V.maxima(dataset(w),1,clock)==[],'excluded activation credited to runtime path')
        cases.append(dict(name='actual-controller-'+row['mode'],status='PASS',witness=w,stdout_sha256=E.sha(path)))
    for name,changes in [('runtime-thread',{7:1}),('runtime-IRQ',{7:2}),('production-path',{4:1}),
                         ('later-sequence',{2:2,3:2}),('owner-borrowed',{23:7}),('owner-generation',{24:1}),
                         ('historical-downgrade',{}),('lost-allowance',{14:0,25:0}),('wrong-tail-CPU',{18:1}),
                         ('invented-return-end',{17:301}),('fault-vector',{6:14}),('retirement-claim',{26:1})]:
        changed=copy.deepcopy(w)
        for k,v in changes.items():changed[k]=v
        if name=='historical-downgrade':changed=changed[:25]
        try:V.witness_envelope(changed,0,clock)
        except ValueError as error:reason=str(error)
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED',reason=reason))
    raw=dataset(w)
    local=copy.deepcopy(raw);local['BEGIN'][0]['primitive']='intel-local-tsc-irq-envelope-v1'
    local['CAPABILITY']=[dict(role=str(r),cpu='0',seen='1',leaf1_edx='4000010',
        vendor_b='756e6547',vendor_d='49656e69',vendor_c='6c65746e') for r in (0,1)]
    assert V.maxima(local,1,clock,primitive='intel-local-tsc-irq-envelope-v1')==[]
    cases.append(dict(name='local-primitive-source-and-capability-bound',status='PASS'))
    for name,change in [
        ('local-missing-cpu-capability',lambda r:r['CAPABILITY'].pop()),
        ('local-unknown-vendor',lambda r:r['CAPABILITY'][0].update(vendor_b='0')),
        ('local-no-sse2',lambda r:r['CAPABILITY'][0].update(leaf1_edx='10')),
        ('local-no-tsc',lambda r:r['CAPABILITY'][0].update(leaf1_edx='4000000')),
        ('local-old-relabel',lambda r:r['BEGIN'][0].update(primitive='ordered-tsc-irq-envelope'))]:
        changed=copy.deepcopy(local);change(changed)
        try:V.maxima(changed,1,clock,primitive='intel-local-tsc-irq-envelope-v1')
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    try:V.maxima(local,1,clock)
    except ValueError:cases.append(dict(name='local-global-parser-fallback-forbidden',status='REJECTED'))
    else:raise AssertionError('new primitive entered historical parser')
    raw['MAXIMUM'][0],raw['MAXIMUM'][11]=raw['MAXIMUM'][11],raw['MAXIMUM'][0]
    raw['MAXIMUM'][0]['path']='0';raw['MAXIMUM'][11]['path']='11'
    for r in raw['WITNESS']:
        if r['index']=='11':r['index']='0'
        elif r['index']=='0':r['index']='11'
    try:V.maxima(raw,1,clock)
    except ValueError as error:reason=str(error)
    else:raise AssertionError('excluded interval promoted to runtime')
    cases.append(dict(name='actual-maxima-path-promotion',status='REJECTED',reason=reason))
    result=dict(status='PASS',cases=cases,authority='ACTUAL_CONTROLLER_AND_SYNTHETIC_CLOCK_ORACLE',timing_acceptance=False)
    E.write(output/'result.json',result);return result
