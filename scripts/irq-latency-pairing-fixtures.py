#!/usr/bin/env python3
"""Independent raw-ledger mutations of actual C collector output (synthetic)."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import irq_latency_evidence as V
import irq_latency_pairs as P
import memory_evidence as E


def run(binary,output):
    output.mkdir(parents=True,exist_ok=False)
    serial=output/'synthetic-collector.serial'
    command=[str(binary.resolve()),str(serial.resolve())]
    done=subprocess.run(command,capture_output=True,check=True)
    raw=V.records(serial.read_text());positive=V.clocks(raw,2)
    cases=[]
    def reject(name,mutate,expected):
        data=copy.deepcopy(raw);mutate(data)
        try: V.clocks(data,2)
        except (ValueError,KeyError) as error:
            E.require(expected in str(error),'pairing negative wrong reason '+name+': '+str(error))
            cases.append(dict(name=name,result='REJECTED',reason=str(error)))
        else: raise ValueError('pairing negative accepted: '+name)
        E.require(V.clocks(raw,2)==positive,'pairing positive not restored')
    for name,idx in [('first',0),('middle',128),('last',255)]:
        reject('missing-'+name+'-point',lambda r,i=idx:r['REFERENCE'].pop(i),'complete reference ledger')
        reject('missing-'+name+'-disposition',lambda r,i=idx:r['POINT_DISPOSITION'].pop(i),'complete disposition')
    reject('unattempted-point',lambda r:r['REFERENCE'][0].update(acquired='0',valid='0'),'point never acquired')
    reject('duplicate-point',lambda r:r['REFERENCE'].append(copy.deepcopy(r['REFERENCE'][0])),'duplicate point')
    reject('wrong-actual-cpu',lambda r:r['REFERENCE'][0].update(cpu='1'),'point outcome')
    reject('intra-point-switch',lambda r:r['REFERENCE'][0].update(switch_after='1'),'point outcome')
    reject('false-acquisition-outcome',lambda r:r['REFERENCE'][0].update(sample_valid='0'),'point outcome')
    reject('wrong-role',lambda r:r['REFERENCE'][0].update(role='1'),'complete reference ledger')
    reject('wrong-epoch',lambda r:r['PAIRING'][0].update(epoch='2'),'pairing plan')
    reject('period-change',lambda r:r['REFERENCE'][0].update(period_fs='99999999',period_after='99999999'),'contradictory timebase')
    reject('mode-change',lambda r:r['REFERENCE'][0].update(mode='2',bits='32'),'contradictory timebase')
    reject('earliest-end-replaced',lambda r:r['PAIR'][0].update(end='6'),'deterministic earliest pair')
    reject('shared-endpoint',lambda r:r['PAIR'][1].update(begin=r['PAIR'][0]['end']),'deterministic earliest pair')
    reject('cross-cpu-pair',lambda r:r['PAIR'][0].update(end='5'),'deterministic earliest pair')
    reject('hidden-invalid-pair',lambda r:r['PAIR'].pop(0),'complete pair ledger')
    reject('interior-as-tail',lambda r:r['POINT_DISPOSITION'][2].update(kind='5'),'point disposition')
    reject('target-changed',lambda r:r['PAIRING'][0].update(target_ns='21000000'),'pairing plan')
    reject('incomplete-worker',lambda r:r['WINDOW'][0].update(complete='0'),'acquisition worker')
    reject('invented-worker',lambda r:r['WINDOW'][0].update(worker='2'),'acquisition worker')
    reject('unknown-method',lambda r:r['BEGIN'][0].update(calibration='future'),'unknown pairing')
    reject('downgraded-witness',lambda r:r['BEGIN'][0].update(witness_cells='25'),'downgraded witness')
    reject('legacy-report-promotion',lambda r:r['BEGIN'][0].pop('calibration'),'legacy method')
    reject('convenient-selected',lambda r:r['TIMEBASE'][0].update(selected0='3'),'summary differs')
    reject('erased-probe-maximum',lambda r:r['TIMEBASE'][0].update(overhead_max='0'),'summary differs')
    reject('missing-post',lambda r:r['PAIRING'].pop(),'pre/post pair roles')
    result=dict(status='PASS',authority='SYNTHETIC_ACTUAL_C_REPORT_ONLY',runtime_measurement=False,
        argv=command,binary_sha256=E.sha(binary),stdout=done.stdout.decode(),serial_sha256=E.sha(serial),
        positive_rows=len(positive),negative_cases=cases)
    E.write(output/'result.json',result);return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    print(json.dumps(run(a.binary,a.output),sort_keys=True))
