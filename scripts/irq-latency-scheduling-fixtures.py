#!/usr/bin/env python3
"""Test scheduling deltas and unavailable facts, with explicit synthetic input."""
import argparse
import copy
import json
from pathlib import Path
import irq_latency_scheduling as V


def stat(pid, start):
    return str(pid)+' (synthetic worker) S '+' '.join(['0']*18+[str(start)]+['0']*30)


def run():
    samples = []
    for i in range(3):
        b = 100+i*100
        samples.append({'collection_begin_ns': b, 'collection_end_ns': b+20, 'monotonic_ns': b+21,
            'process_stat_begin': stat(10, 1), 'process_stat_end': stat(10, 1), 'vm_pid': 10,
            'guest_cpus': 1, 'host_boot_id': 'synthetic-host',
            'qmp_identity': {'uuid': 'synthetic-vm', 'cpus': [{'cpu-index': 0, 'thread-id': 11}]},
            'threads': [{'tid': 11, 'stat_begin': stat(11, 2), 'stat': stat(11, 2),
                         'collection_begin_ns': b+1, 'collection_end_ns': b+10,
                         'schedstat': f'{1000+i*100} {20+i*5} {30+i}'}],
            'cgroup': {'ancestors': [{'path': '/synthetic', 'fields': {
                'cpu.stat': f'usage_usec {100+i}\nnr_throttled {i}',
                'cpu.pressure': f'some avg10=0.00 avg60=0.00 avg300=0.00 total={i*10}'}, 'unavailable': {}}]}})
    positive = V.verify(samples)
    assert positive['intervals'][0]['vcpus'][0] == {'execution_ns':100, 'runqueue_ns':5, 'timeslices':1}
    cases = []
    def reject(name, mutate, reason):
        rows = copy.deepcopy(samples); mutate(rows)
        try: V.verify(rows)
        except ValueError as e:
            assert reason in str(e), (name, str(e))
            cases.append({'name':name, 'synthetic':True, 'reason':str(e)})
        else: raise AssertionError('accepted '+name)
        assert V.verify(samples) == positive
    reject('process-reused', lambda r:r[1].update(process_stat_end=stat(10,9)), 'process lifetime')
    reject('thread-reused', lambda r:r[1]['threads'][0].update(stat=stat(11,9)), 'thread lifetime')
    reject('thread-replaced-between', lambda r:r[1]['threads'][0].update(stat=stat(11,9),stat_begin=stat(11,9)), 'epoch changed')
    reject('overlap',lambda r:r[1].update(collection_begin_ns=110), 'overlapping/out-of-order')
    reject('thread-outside-collection',lambda r:r[1]['threads'][0].update(collection_end_ns=999), 'thread collection')
    reject('schedstat-reset',lambda r:r[1]['threads'][0].update(schedstat='0 0 0'), 'counter reset/wrap')
    reject('schedstat-unavailable',lambda r:r[1]['threads'][0].update(schedstat=''), 'schedstat unavailable')
    reject('wrong-qmp-thread',lambda r:r[1]['qmp_identity']['cpus'][0].update({'thread-id':99}), 'thread inventory')
    reject('cgroup-counter-reset',lambda r:r[1]['cgroup']['ancestors'][0]['fields'].update({'cpu.stat':'usage_usec 0\nnr_throttled 0'}), 'counter reset/wrap')
    reject('pressure-reset',lambda r:r[2]['cgroup']['ancestors'][0]['fields'].update({'cpu.pressure':'some total=0'}), 'counter reset/wrap')
    reject('silent-missing-interface',lambda r:r[1]['cgroup']['ancestors'][0]['fields'].pop('cpu.pressure'), 'missing interface fact')
    unavailable = copy.deepcopy(samples)
    g = unavailable[1]['cgroup']['ancestors'][0]
    del g['fields']['cpu.pressure'];g['unavailable']['cpu.pressure']={'status':'UNAVAILABLE','errno':13}
    result = V.verify(unavailable)
    assert all(x['cgroups']['/synthetic']['cpu.pressure']['status']=='UNAVAILABLE' for x in result['intervals'])
    return {'status':'PASS','authority':'SYNTHETIC_COUNTER_COMPARATOR_TESTS','negative_cases':cases,
            'restored_positives':len(cases),'explicit_unavailable_positive':result}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=run()
    with a.output.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps({'status':r['status'],'negative_cases':len(r['negative_cases'])}))
