#!/usr/bin/env python3
"""Exercise real finally paths and reject incomplete/promoted gate inputs."""
import argparse
import importlib.util
import json
from pathlib import Path
from unittest.mock import patch
import memory_evidence as E


def load(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m


def run(output):
    output.mkdir(parents=True,exist_ok=False);X=load('irq-latency-pairing-experiment');cases=[]
    for name,body_fails,host_fails,pid in [('complete',False,False,123),('activation-refused',True,False,123),
        ('stress-failed',True,False,123),('terminal-denied',True,True,123),('pre-guest-transport',True,False,None)]:
        directory=output/name;directory.mkdir();events=[]
        def body():
            events.append('body')
            if body_fails:raise ValueError(name)
            return 42
        def snapshot(cpus,observed):
            events.append('snapshot');assert cpus==8 and observed==pid
            if host_fails:raise PermissionError('controlled proc denial')
            return dict(status='CAPTURED',pid=observed)
        def stop():
            assert (directory/'host-terminal.json').exists() or (directory/'host-terminal-unavailable.json').exists()
            events.append('stop')
        try:result=X.lifecycle(directory,body,lambda:pid,stop,snapshot)
        except ValueError as error:assert body_fails and str(error)==name
        else:assert not body_fails and result==42
        assert events[-1]=='stop' and events.count('stop')==1
        if host_fails or pid is None:
            row=json.loads((directory/'host-terminal-unavailable.json').read_text());assert row['status']=='UNAVAILABLE' and row['reason']
        else:assert json.loads((directory/'host-terminal.json').read_text())['pid']==pid
        cases.append(dict(name=name,result='PASS',events=events))
    stopped=[]
    with patch.object(X,'terminal',side_effect=OSError('controlled evidence write failure')):
        try:X.lifecycle(output,lambda:42,lambda:123,lambda:stopped.append(True))
        except OSError as error:assert 'evidence write failure' in str(error)
        else:raise AssertionError('terminal write failure hidden')
    assert stopped==[True]
    cases.append(dict(name='terminal-write-failure-still-stops',result='PASS'))
    G=load('irq-off-latency-gate')
    for role,boots in [('DIAGNOSTIC_NO_ACCEPTANCE',1),('DIAGNOSTIC_NO_ACCEPTANCE',3),('CURRENT_TIMING_QUALIFICATION',1)]:
        with patch.object(G.X,'bound_plan',return_value=dict(role=role,boots=boots,retries=0)):
            try:G.timing_proof(output)
            except ValueError as error:assert 'diagnostic/partial series' in str(error)
            else:raise AssertionError('diagnostic promoted')
        cases.append(dict(name='reject-'+role+'-'+str(boots),result='REJECTED'))
    with patch.object(G.X,'bound_plan',return_value=dict(role='CURRENT_TIMING_QUALIFICATION',boots=3,retries=0,candidate_id='old')):
        with patch.object(G,'read',return_value=dict(candidate='current')):
            try:G.timing_proof(output)
            except ValueError as error:assert 'unbooted/different candidate' in str(error)
            else:raise AssertionError('unbooted candidate promoted')
    cases.append(dict(name='current-unbooted-credited-prior',result='REJECTED'))
    with patch.object(G.X,'bound_plan',return_value=dict(role='CURRENT_TIMING_QUALIFICATION',boots=3,retries=0,candidate_id=E.identity({}))):
        with patch.object(G,'read',return_value={}):
            with patch.object(G.X,'analyze',return_value=dict(qualified=False,role='CURRENT_TIMING_QUALIFICATION')):
                try:G.timing_proof(output)
                except ValueError as error:assert 'qualified complete timing boot' in str(error)
                else:raise AssertionError('pre-calibration-only promoted')
    cases.append(dict(name='precalibration-only-series-reuse',result='REJECTED'))
    # A shell summary is not a source-bound stage or an actual raw oracle.
    E.write(output/'latency-static.json',dict(status='PASS',build_input_sha256='old',harness_sha256='old'))
    try:G.semantic(output,'static')
    except ValueError as error:assert 'current latency stage identity' in str(error)
    else:raise AssertionError('summary promoted')
    cases.append(dict(name='diagnostic-summary-as-current-stage',result='REJECTED'))
    assert not X.raw_qualified({'unavailable':'missing postcheck'}, {})
    cases.append(dict(name='missing-postcheck-not-zero',result='REJECTED'))
    analysis=output/'roundtrip-analysis.json'
    sample=dict(pre=dict(pairs=[dict(bounds=(2600000000,2600000010,2600000005))]),
                counts={0:32,7:191},qualified=False)
    first=X.seal_analysis(analysis,sample);sealed=analysis.read_bytes()
    assert first==X.seal_analysis(analysis,sample)==json.loads(sealed)
    assert analysis.read_bytes()==sealed
    cases.append(dict(name='sealed-analysis-json-roundtrip',result='PASS'))
    for name,changed in [('changed-maximum',dict(sample,maximum=1)),
                         ('diagnostic-promotion',dict(sample,qualified=True))]:
        try:X.seal_analysis(analysis,changed)
        except ValueError as error:assert 'sealed analysis changed' in str(error)
        else:raise AssertionError('changed sealed result accepted')
        assert analysis.read_bytes()==sealed
        cases.append(dict(name='sealed-analysis-'+name,result='REJECTED'))
    result=dict(status='PASS',cases=cases,runtime_measurement=False)
    E.write(output/'result.json',result);return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    print(json.dumps(run(a.output)))
