#!/usr/bin/env python3
"""Actual current coverage verifier controls with synthetic, CRC-bound records."""
import copy, json, zlib
from pathlib import Path
import irq_latency_workload as W
from irq_latency_evidence import require


def run(output):
    output.mkdir(parents=True, exist_ok=False)
    raw = {'STACK_COVERAGE_METHOD': [{'version':'1','cpus':'2','epoch':'1'}], 'STACK_COVERAGE': [],
           'MAXIMUM': [], 'STACK_COVERAGE_FAILURE': []}
    lines, services = [], {}
    for cpu in range(2):
        row = dict(version='1', cpu=str(cpu), before=str(cpu), after=str(cpu), child_cpu=str(cpu),
                   worker_retired='1', child_retired='1', status='PASS')
        for offset, role in enumerate(('worker', 'child')):
            task = 10+2*cpu+offset; slot = 128+2*cpu+offset; request = 1+2*cpu+offset
            row.update({role: str(task), role+'_life': '1', role+'_slot': str(slot), role+'_generation': '2'})
            body = f'[STACK_GUARD][RETIRE] task={task} lifecycle=1 slot={slot} generation=2 request={request} requested=3 acknowledged=3 status=COMPLETE'
            lines.append(body+f' crc={zlib.crc32(body.encode()):08x}')
            services[request] = dict(operation='1', status='0')
        raw['STACK_COVERAGE'].append(row)
        raw['MAXIMUM'].append(dict(cpu=str(cpu), path='7', count='2', invalid='0'))
    lines += ['[IRQ_LATENCY][STACK_COVERAGE] cpu=0', '[IRQ_LATENCY][STACK_COVERAGE] cpu=1', '[IRQ_LATENCY][WORK_CUTOFF] boundary']
    serial = '\n'.join(lines)+'\n'
    def verify(r=raw, s=serial, c=W.STACK_COVERAGE, ledger=services, last=4):
        return W.stack_coverage_lifetimes(s,r,2,c,ledger,last)
    expected=verify();cases=[dict(name='complete-lifecycle-and-service',result='PASS',synthetic=True)]
    def reject(name, r=raw, s=serial, c=W.STACK_COVERAGE, ledger=services, last=4):
        try: verify(r,s,c,ledger,last)
        except (ValueError,KeyError): cases.append(dict(name=name,result='REJECT',synthetic=True))
        else: raise AssertionError(name)
        require(verify()==expected,'coverage restored positive changed')
    for key,value in [('before','1'),('after','1'),('child_cpu','1'),('version','2'),
                      ('worker_retired','0'),('child_retired','0'),('worker_generation','9'),('status','FAIL')]:
        r=copy.deepcopy(raw);r['STACK_COVERAGE'][0][key]=value;reject('wrong-'+key,r=r)
    r=copy.deepcopy(raw);r['STACK_COVERAGE'].pop();reject('missing-cpu',r=r)
    r=copy.deepcopy(raw);r['STACK_COVERAGE'].append(r['STACK_COVERAGE'][0]);reject('duplicate-cpu',r=r)
    r=copy.deepcopy(raw);r['MAXIMUM'][0]['count']='0';reject('missing-real-stack-observation',r=r)
    r=copy.deepcopy(raw);r.pop('STACK_COVERAGE_METHOD');reject('missing-source-contract',r=r)
    for key,value in [('version','2'),('cpus','8'),('epoch','2')]:
        r=copy.deepcopy(raw);r['STACK_COVERAGE_METHOD'][0][key]=value;reject('wrong-declaration-'+key,r=r)
    reject('current-evidence-selected-historical',c=None)
    reject('unknown-version',c='targeted-stack-lifecycle-v2')
    for at,name in [(0,'prefix'),(1,'middle'),(3,'tail')]:
        reject('missing-retirement-'+name,s='\n'.join(lines[:at]+lines[at+1:])+'\n')
    reject('duplicate-retirement',s=lines[0]+'\n'+serial)
    reject('CRC-change',s=serial.replace('task=10','task=11',1))
    reject('missing-ACK-service',ledger={k:v for k,v in services.items() if k!=1})
    reject('late-service',last=3)
    reject('coverage-after-cutoff',s='\n'.join(lines[-1:]+lines[:-1])+'\n')
    historical={'BEGIN':[{}]}
    require(W.stack_coverage(historical,2,None) is None,'historical selector affected')
    result=dict(status='PASS',authority='HOSTED_VERIFIER_CONTROLS_ONLY',cases=cases)
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();print(json.dumps(run(args.output)))
