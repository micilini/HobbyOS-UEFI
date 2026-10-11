#!/usr/bin/env python3
"""Actual-C/independent Python comparison; historical pooling is test-only."""
import argparse
import copy
import json
import subprocess
from pathlib import Path
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_pairs as P

KEYS='tsc_before hpet tsc_after switch_before switch_after cpu end_cpu mode bits retries valid initialized if_enabled period_fs period_after acquired sample_valid identity_valid if_after'.split()


def run(serial,output):
    output.mkdir(parents=True,exist_ok=False)
    raw=V.records(serial.read_text());legacy=V.calibration_diagnostic(raw)
    E.require(legacy['begin_status']=='UNAVAILABLE_TIMEBASE','historical failure remains rejected')
    rows=sorted(raw['REFERENCE'],key=lambda r:(int(r['index']),r['endpoint']=='end'))
    points=[]
    for r in rows:
        p=dict(r,period_after=r['period_fs'],acquired='1',sample_valid=r['valid'],identity_valid='1',if_after=r['if_enabled'])
        points.append(p)
    cases=[('retrospective-design-only',points,8)]
    # Fixed scalar variations; no favorable-window selection or runtime receipt.
    for name,i,changes in [('earliest-uncertain',0,{'tsc_after':str(int(points[0]['tsc_after'])+1000000)}),
        ('period-overflow',0,{'period_fs':str(V.U64),'period_after':str(V.U64)}),
        ('timestamp-equal',0,{'tsc_after':points[0]['tsc_before']}),
        ('endpoint-migration',0,{'end_cpu':'999','valid':'0'}),
        ('period-change',0,{'period_fs':'99999999','period_after':'99999999'})]:
        changed=copy.deepcopy(points);changed[i].update(changes);cases.append((name,changed,8))
    expected=[]
    for name,ps,cpus in cases:
        try:answer=P.derive(ps,cpus)
        except ValueError:answer=None
        expected.append(answer)
    receipts=[]
    for mode,flags in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        binary=output/('pair-stream-'+mode)
        argv=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',*flags,
            'scripts/irq-latency-pairing-stream-host.c','kernel/src/core/irq_latency_pairs.c',
            'kernel/src/core/irq_latency_time.c','-o',str(binary)]
        compiled=subprocess.run(argv,capture_output=True);E.require(compiled.returncode==0,compiled.stderr.decode())
        for (name,ps,cpus),answer in zip(cases,expected):
            payload=f'{len(ps)} {cpus}\n'+'\n'.join(' '.join([*(p[k] for k in KEYS),'1','1']) for p in ps)+'\n'
            ran=subprocess.run([str(binary)],input=payload,text=True,capture_output=True)
            E.require(ran.returncode==0,'actual pair stream '+mode+': '+ran.stderr)
            lines=ran.stdout.splitlines();header=list(map(int,lines[0].split()))
            if answer is None:E.require(header==[0,0] and len(lines)==1,'failure published partial pair output')
            else:
                pairs,ds=answer
                expected_lines=[f'1 {len(pairs)}']+['P '+ ' '.join(str(p[k]) for k in ('begin','end','cpu','valid')) for p in pairs]+[
                    'D '+' '.join(str(d[k]) for k in ('kind','reason','pair')) for d in ds]
                E.require(lines==expected_lines,'independent C pair/disposition mismatch: '+name)
            receipts.append(dict(case=name,mode=mode,exit=ran.returncode,valid=bool(header[0]),pairs=header[1]))
    pairs,_=expected[0]
    result=dict(status='PASS',authority='RETROSPECTIVE_OFFLINE_DESIGN_FIXTURE_ONLY',timing_acceptance=False,
        historical_status=legacy['begin_status'],serial_sha256=E.sha(serial),executions=receipts,
        per_cpu=[dict(cpu=c,pairs=sum(p['cpu']==c for p in pairs),eligible=sum(p['cpu']==c and p['valid'] for p in pairs)) for c in range(8)])
    E.write(output/'result.json',result);return result
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--serial',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);a=p.parse_args();print(json.dumps(run(a.serial,a.output)))
