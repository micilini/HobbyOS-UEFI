#!/usr/bin/env python3
"""Synthetic raw controls using actual collector clocks; no runtime authority."""
import argparse,copy,json,subprocess
from pathlib import Path
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_pairs as P
import irq_latency_entry as J
import irq_latency_interior as I

def clock_cases(raw):
    raw=copy.deepcopy(raw)
    for cpu in range(2):
        original=next(r for r in raw['INTERIOR'] if r['cpu']==str(cpu) and r['path']=='0')
        row={k:v for k,v in original.items() if k!='path'}
        row.update(method='masked-clock-segment',version='3',phase='3',incoming_flags='514',site=str(0xffffffff80001000),
            sequence='1',started='1',retired='1',pending='0')
        raw['CLOCK_BODY'].append(row)
        for p in raw['INTERIOR_POINT']:
            if p['cpu']==str(cpu) and p['path']=='0':
                point={k:v for k,v in p.items() if k!='path'};point.update(lock='0',depth='0')
                raw['CLOCK_BODY_POINT'].append(point)
    result=I.verify_clock(raw,2)
    assert all(r['observed_violation'] and not r['below_budget_acceptance'] for r in result['rows'])
    cases=[dict(name='clock-body-source-bound-minimum',status='PASS')]
    mutations=[
        ('clock-old-interior-relabel',lambda r:r['CLOCK_BODY'][0].update(method='continuous-mask-lock-body')),
        ('clock-unknown-version',lambda r:r['CLOCK_BODY'][0].update(version='2')),
        ('clock-historical-body-relabel',lambda r:r['CLOCK_BODY'][0].update(version='1',method='masked-clock-body')),
        ('clock-unknown-phase',lambda r:r['CLOCK_BODY'][0].update(phase='5')),
        ('clock-enabled-sampling-with-disabled-caller',lambda r:r['CLOCK_BODY'][0].update(phase='1',incoming_flags='2')),
        ('clock-missing-end',lambda r:r['CLOCK_BODY_POINT'].pop()),
        ('clock-duplicate-end',lambda r:r['CLOCK_BODY_POINT'].append(r['CLOCK_BODY_POINT'][1])),
        ('clock-missing-maximum',lambda r:r['CLOCK_BODY'][0].update(cycles='0')),
        ('clock-erased-count',lambda r:r['CLOCK_BODY'][0].update(count='0')),
        ('clock-missing-reservation',lambda r:r['CLOCK_BODY'][0].update(started='0')),
        ('clock-wrong-frequency',lambda r:r['CLOCK_BODY'][0].update(frequency_upper='1')),
        ('clock-upward-rounding',lambda r:r['CLOCK_BODY'][0].update(minimum_ns=str(int(r['CLOCK_BODY'][0]['minimum_ns'])+1))),
        ('clock-upper-bound-promotion',lambda r:r['CLOCK_BODY'][0].update(below_budget='PASS')),
        ('clock-non-source-site',lambda r:r['CLOCK_BODY'][0].update(site='0')),
        ('clock-untracked-boundary',lambda r:r['TERMINAL'][0].update(invalid_boundaries='1'))]
    for field in ('segment','switches','entries','faults','owner','generation','depth','lock','epoch','actual_cpu'):
        mutations.append(('clock-changed-'+field,lambda r,k=field:r['CLOCK_BODY_POINT'][1].update({k:str(int(r['CLOCK_BODY_POINT'][1][k])+1)})))
    mutations.extend([
        ('clock-enabled-endpoint',lambda r:r['CLOCK_BODY_POINT'][1].update(flags='514')),
        ('clock-hidden-observer-work',lambda r:r['CLOCK_BODY_POINT'][1].update(queries='0')),
        ('clock-erased-end-timestamp',lambda r:r['CLOCK_BODY_POINT'][1].update(tsc='0'))])
    for name,change in mutations:
        altered=copy.deepcopy(raw);change(altered)
        try:I.verify_clock(altered,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for name,change in [
        ('clock-incomplete-retirement-unavailable',lambda r:r['CLOCK_BODY'][0].update(started='2',pending='2')),
        ('clock-invalid-continuity-unavailable',lambda r:r['CLOCK_BODY'][0].update(invalid='1')),
        ('clock-outside-calibration-unavailable',lambda r:[p.update(tsc=str(int(p['tsc'])+100000000000000)) for p in r['CLOCK_BODY_POINT']])]:
        altered=copy.deepcopy(raw);change(altered)
        assert not I.verify_clock(altered,2)['rows'][0]['observed_violation']
        cases.append(dict(name=name,status='PASS'))
    return cases,raw,result

def tick_cases(clock_raw):
    raw=copy.deepcopy(clock_raw);raw['TICK_BODY']=[];raw['TICK_BODY_POINT']=[];raw['TICK_SAMPLE']=[]
    for row in raw['CLOCK_BODY']:
        r={k:v for k,v in row.items() if k not in ('phase','incoming_flags','site')}
        cpu=int(r['cpu']);a=next(p for p in raw['CLOCK_BODY_POINT'] if p['cpu']==r['cpu'] and p['endpoint']=='begin')
        start=int(a['tsc']);cycles=int(r['cycles'])
        r.update(method='lapic-handler-body',version='4',vector='34',without_memory='2',
            started='3',retired='3',outer_sequence='11',sample_begin=str(start+10),sample_end=str(start+20),after_clock=str(start+cycles//3),after_work=str(start+2*cycles//3))
        local=[p for p in raw['REFERENCE'] if p['cpu']==r['cpu'] and p['valid']=='1']
        ticks=sum(max(int(p['hpet']) for p in local if int(p['role'])==role) for role in (0,1))//2
        raw['TICK_SAMPLE'].append(dict(cpu=r['cpu'],epoch='1',sequence=r['sequence'],outer_sequence='11',
            counter_end=str(start+15),ticks=str(ticks),mode=local[0]['mode'],retries='0',valid='1',source='1',source_epoch='1'))
        raw['TICK_BODY'].append(r)
    raw['TICK_BODY_POINT']=copy.deepcopy(raw['CLOCK_BODY_POINT'])
    result=I.verify_tick(raw,2);assert all(r['observed_violation'] for r in result['rows'])
    cases=[dict(name='tick-bound-same-event-minimum',status='PASS')]
    changes=[('missing-endpoint',lambda r:r['TICK_BODY_POINT'].pop()),
        ('duplicate-endpoint',lambda r:r['TICK_BODY_POINT'].append(r['TICK_BODY_POINT'][0])),
        ('missing-cpu',lambda r:r['TICK_BODY'].pop()),
        ('wrong-vector',lambda r:r['TICK_BODY'][0].update(vector='225')),
        ('wrong-version',lambda r:r['TICK_BODY'][0].update(version='2')),
        ('missing-sample',lambda r:r['TICK_SAMPLE'].pop()),
        ('duplicate-sample',lambda r:r['TICK_SAMPLE'].append(r['TICK_SAMPLE'][0])),
        ('wrong-sample-sequence',lambda r:r['TICK_SAMPLE'][0].update(sequence='3')),
        ('wrong-sample-epoch',lambda r:r['TICK_SAMPLE'][0].update(epoch='2')),
        ('missing-counter-end',lambda r:r['TICK_SAMPLE'][0].update(counter_end='0')),
        ('reversed-counter-end',lambda r:r['TICK_SAMPLE'][0].update(counter_end=r['TICK_BODY'][0]['sample_end'])),
        ('invalid-counter',lambda r:r['TICK_SAMPLE'][0].update(valid='0')),
        ('retry-exhaustion',lambda r:r['TICK_SAMPLE'][0].update(retries='5')),
        ('wrong-counter-domain',lambda r:r['TICK_SAMPLE'][0].update(mode='2')),
        ('old-clock-relabel',lambda r:r['TICK_BODY'][0].update(method='masked-clock-segment')),
        ('lost-sample-start',lambda r:r['TICK_BODY'][0].update(sample_begin='0')),
        ('reversed-sample',lambda r:r['TICK_BODY'][0].update(sample_end=r['TICK_BODY'][0]['sample_begin'])),
        ('lost-stage',lambda r:r['TICK_BODY'][0].update(after_clock='0')),
        ('reversed-stage',lambda r:r['TICK_BODY'][0].update(after_work=r['TICK_BODY'][0]['after_clock'])),
        ('missing-outer',lambda r:r['TICK_BODY'][0].update(outer_sequence='0')),
        ('lost-count',lambda r:r['TICK_BODY'][0].update(count='0')),
        ('lost-nonmemory',lambda r:r['TICK_BODY'][0].update(without_memory='0')),
        ('lost-max',lambda r:r['TICK_BODY'][0].update(cycles='0')),
        ('wrong-frequency',lambda r:r['TICK_BODY'][0].update(frequency_upper='1')),
        ('wrong-rounding',lambda r:r['TICK_BODY'][0].update(minimum_ns='0')),
        ('small-promoted-upper',lambda r:r['TICK_BODY'][0].update(below_budget='PASS'))]
    for key,value in [('actual_cpu','1'),('epoch','2'),('segment','5'),('generation','3'),('switches','4'),
                      ('entries','8'),('faults','1'),('flags','514'),('depth','2'),('lock','1'),('queries','0')]:
        changes.append((key,lambda r,k=key,v=value:r['TICK_BODY_POINT'][1].update({k:v})))
    for name,change in changes:
        altered=copy.deepcopy(raw);change(altered)
        try:I.verify_tick(altered,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError('tick-'+name)
        cases.append(dict(name='tick-'+name,status='REJECTED'))
    for name,change in [('pending',lambda r:r['TICK_BODY'][0].update(started='4',pending='4')),
                        ('invalid',lambda r:r['TICK_BODY'][0].update(invalid='1'))]:
        altered=copy.deepcopy(raw);change(altered);row=I.verify_tick(altered,2)['rows'][0]
        assert row['authority']=='UNAVAILABLE' and not row['observed_violation']
        cases.append(dict(name='tick-'+name,status='PASS_UNAVAILABLE'))
    altered=copy.deepcopy(raw);altered['TICK_SAMPLE'][0]['ticks']='1'
    assert not I.verify_tick(altered,2)['rows'][0]['observed_violation']
    cases.append(dict(name='tick-counter-outside-epoch',status='PASS_UNAVAILABLE'))
    return cases,raw,result

def run(collector,output):
    output.mkdir(parents=True,exist_ok=False)
    serial=output/'synthetic-collector.serial';subprocess.run([str(collector.resolve()),str(serial)],check=True,capture_output=True)
    raw=V.records(serial.read_text());clocks=P.clocks(raw,2)
    raw['TERMINAL']=[dict(invalid_boundaries='0')]
    raw['CPU']=[dict(cpu=str(c),invalid='0') for c in range(2)]
    for p in raw['OVERHEAD']:
        raw['ENTRY_CHECKPOINT'].append(dict(role=p['role'],index=p['index'],probe=p['probe'],epoch='1',cpu=p['cpu'],
            sequence=p['sequence_after'],method='entry-return-checkpoints',version='2',tsc='125',exit_tsc='275'))
    for cpu in range(2):
        upper=V.ceildiv(max(clocks[(r,cpu)]['upper_hz'] for r in (0,1))*1010000,1000000)
        start=max(V.number(p,'tsc_after') for p in raw['REFERENCE'] if p['role']=='0' and int(p['cpu'])==cpu)+1000000
        cycles=V.ceildiv(50001*upper,1000000000)
        for path in range(11):
            raw['INTERIOR'].append(dict(cpu=str(cpu),path=str(path),method='continuous-mask-lock-body',version='2',epoch='1',
                count='1',invalid='0',cycles=str(cycles),paths=str(1<<path),frequency_upper=str(upper),
                minimum_ns=str(cycles*1000000000//upper),frequency_valid='1',authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',below_budget='NOT_ESTABLISHED'))
            for end in range(2):
                raw['INTERIOR_POINT'].append(dict(cpu=str(cpu),path=str(path),endpoint='end' if end else 'begin',tsc=str(start+end*cycles),
                    flags='2',actual_cpu=str(cpu),epoch='1',segment='4',switches='3',entries='7',faults='0',owner='112',generation='2',
                    depth='1',lock='4096',queries=str(20+end*3),recorder_sequence=str(40+end*2)))
    assert J.verify(raw,2)['status']=='PASS'
    result=I.verify(raw,2);assert all(r['observed_violation'] and not r['below_budget_acceptance'] for r in result['rows'])
    cases=[dict(name='bound-synthetic-minimum',status='PASS')]
    for name,change,checker in [
        ('missing-checkpoint',lambda r:r['ENTRY_CHECKPOINT'].pop(),J.verify),
        ('checkpoint-outside',lambda r:r['ENTRY_CHECKPOINT'][0].update(tsc='151'),J.verify),
        ('checkpoint-tail-before-end',lambda r:r['ENTRY_CHECKPOINT'][0].update(exit_tsc='160'),J.verify),
        ('checkpoint-tail-after-return',lambda r:r['ENTRY_CHECKPOINT'][0].update(exit_tsc='1000'),J.verify),
        ('checkpoint-tail-missing',lambda r:r['ENTRY_CHECKPOINT'][0].pop('exit_tsc'),J.verify),
        ('checkpoint-wrong-cpu',lambda r:r['ENTRY_CHECKPOINT'][0].update(cpu='1'),J.verify),
        ('checkpoint-unknown-version',lambda r:r['ENTRY_CHECKPOINT'][0].update(version='3'),J.verify),
        ('unsafe-prior-method',lambda r:r['INTERIOR'][0].update(version='1'),I.verify),
        ('untracked-transition',lambda r:r['TERMINAL'][0].update(invalid_boundaries='1'),I.verify),
        ('hidden-enable',lambda r:r['INTERIOR_POINT'][1].update(segment='5'),I.verify),
        ('migration',lambda r:r['INTERIOR_POINT'][1].update(actual_cpu='1'),I.verify),
        ('owner-generation',lambda r:r['INTERIOR_POINT'][1].update(generation='3'),I.verify),
        ('switch',lambda r:r['INTERIOR_POINT'][1].update(switches='4'),I.verify),
        ('irq-return',lambda r:r['INTERIOR_POINT'][1].update(entries='8'),I.verify),
        ('returning-fault',lambda r:r['INTERIOR_POINT'][1].update(faults='1'),I.verify),
        ('missing-endpoint',lambda r:r['INTERIOR_POINT'].pop(),I.verify),
        ('duplicate-endpoint',lambda r:r['INTERIOR_POINT'].append(r['INTERIOR_POINT'][0]),I.verify),
        ('wrong-rounding',lambda r:r['INTERIOR'][0].update(minimum_ns=str(int(r['INTERIOR'][0]['minimum_ns'])+1)),I.verify),
        ('wrong-frequency',lambda r:r['INTERIOR'][0].update(frequency_upper='2400000000'),I.verify),
        ('small-promoted-upper',lambda r:r['INTERIOR'][0].update(below_budget='PASS'),I.verify),
        ('historical-enclosing-kind',lambda r:r['INTERIOR'][0].update(method='ordered-tsc-irq-envelope'),I.verify),
        ('hidden-observer-work',lambda r:r['INTERIOR_POINT'][1].update(queries='19'),I.verify),
        ('wrong-epoch',lambda r:r['INTERIOR_POINT'][1].update(epoch='2'),I.verify),
        ('after-unlock-depth',lambda r:r['INTERIOR_POINT'][1].update(depth='0'),I.verify),
        ('invalid-clock',lambda r:r['TIMEBASE'][0].update(clock_errors='1'),I.verify)]:
        altered=copy.deepcopy(raw);change(altered)
        try:checker(altered,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    altered=copy.deepcopy(raw);altered['INTERIOR'][0]['invalid']='1'
    assert not I.verify(altered,2)['rows'][0]['observed_violation']
    cases.append(dict(name='incomplete-continuity-unavailable',status='PASS'))
    clock_controls,clock_raw,clock_result=clock_cases(raw);cases.extend(clock_controls)
    E.write(output/'clock-raw.json',clock_raw);E.write(output/'clock-positive.json',clock_result)
    tick_controls,tick_raw,tick_result=tick_cases(clock_raw);cases.extend(tick_controls)
    E.write(output/'tick-raw.json',tick_raw);E.write(output/'tick-positive.json',tick_result)
    E.write(output/'raw.json',raw);E.write(output/'positive.json',result)
    result=dict(status='PASS',authority='HOSTED_SYNTHETIC_ONLY',cases=cases,raw_sha256=E.sha(output/'raw.json'),
        collector_serial_sha256=E.sha(serial),implementation_acceptance=False)
    E.write(output/'result.json',result);return result
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--collector',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    print(json.dumps(run(a.collector,a.output)))
