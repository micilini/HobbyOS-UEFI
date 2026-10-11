#!/usr/bin/env python3
"""Actual boot formatter and synthetic adversarial raw controls; no guest authority."""
import copy
import json
import subprocess
import importlib.util
import tempfile
from unittest.mock import patch
from pathlib import Path
import memory_evidence as E
import clocksource_evidence as C

def reconstruct(serial):
    positive=C.boot(serial,2,2);cases=[dict(name='actual-boot-formatter-qualified',status='PASS')]
    controls={
        'missing-selection':lambda s:'\n'.join(s.splitlines()[1:])+'\n',
        'duplicate-selection':lambda s:s+s.splitlines()[0]+'\n',
        'missing-CPU':lambda s:'\n'.join(x for x in s.splitlines() if not x.startswith('[CLOCKSOURCE_CPU] version=3 cpu=1'))+'\n',
        'missing-point-tail':lambda s:'\n'.join(s.splitlines()[:-1])+'\n',
        'wrong-epoch':lambda s:s.replace('source=2 epoch=2','source=2 epoch=1'),
        'wrong-offset':lambda s:s.replace('offset=95000','offset=95001'),
        'noncanonical-offset':lambda s:s.replace('offset=95000','offset=095000'),
        'wrong-guest-topology':lambda s:s.replace('cpus=2','cpus=8'),
        'zero-rate':lambda s:s.replace('mul0=2147483648','mul0=0'),
        'odd-version':lambda s:s.replace('ver0=2','ver0=3'),
        'missing-stability':lambda s:s.replace('flags0=1','flags0=0'),
        'stopped-guest':lambda s:s.replace('flags0=1','flags0=3'),
        'missing-MSR-readback':lambda s:s.replace('registration=1048577','registration=0'),
        'unregistered-selected-page':lambda s:s.replace('registered=1','registered=0'),
        'clock-self-oracle':lambda s:s.replace('valid=1','valid=0'),
        'missing-HPET':lambda s:s.replace('period=10000000','period=0'),
        'overflow-shift':lambda s:s.replace('shift0=1','shift0=64'),
        'source-regression-hidden':lambda s:s.replace('status=3 reason=0','status=4 reason=6'),
        'timing-provider-unknown':lambda s:s.replace('source=2 epoch=2','source=4 epoch=2'),
        'old-method-as-current':lambda s:s.replace('version=3','version=2'),
        'increased-rate-reserve':lambda s:s.replace('rate_ppm=10000','rate_ppm=20000'),
        'missing-origin':lambda s:__import__('re').sub(r' origin_ns=\d+','',s),
        'chosen-origin':lambda s:__import__('re').sub(r'origin_ns=\d+','origin_ns=1',s),
        'wrong-BSP-slot':lambda s:s.replace('bsp=0','bsp=2'),
    }
    for name,change in controls.items():
        try:C.boot(change(serial),2,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    raw={'CLOCKSOURCE_TERMINAL':[], 'CLOCKSOURCE_REFERENCE':[]}
    for cpu in range(2):
        previous=copy.deepcopy(positive['cpus'][cpu]['points'][-1]);previous.pop('index')
        previous.update(epoch='2',point='0')
        p=copy.deepcopy(previous);p['point']='1'
        for k in ('ns0','ns1','ns','tsc0','tsc1'):p[k]=str(int(p[k])+1000000000)
        p['ticks']=str(int(p['ticks'])+100000000)
        raw['CLOCKSOURCE_REFERENCE'].extend((previous,p))
        raw['CLOCKSOURCE_TERMINAL'].append(dict(version='3',cpu=str(cpu),source='2',epoch='2',reason='0',busy='0',registered='1',
            reads='1000',references='1',maximum_reference_ns='10000',last_tsc=p['tsc1'],last_ns=p['ns1']))
    C.terminal(raw,positive);cases.append(dict(name='bound-synthetic-terminal',status='PASS'))
    precise=copy.deepcopy(raw)
    for cpu in range(2):
        p=precise['CLOCKSOURCE_REFERENCE'][cpu*2+1]
        p['ns1']=str(int(p['ns0'])+10);p['tsc1']=str(int(p['tsc0'])+10)
        precise['CLOCKSOURCE_TERMINAL'][cpu].update(maximum_reference_ns='10',last_tsc=p['tsc1'],last_ns=p['ns1'])
        lo,hi=C.point(p)
        assert not lo<=positive['offset']<=hi
    C.terminal(precise,positive);cases.append(dict(name='narrower-valid-reference-not-fixed-origin-oracle',status='PASS'))
    delayed=copy.deepcopy(raw)
    for cpu in range(2):
        p=delayed['CLOCKSOURCE_REFERENCE'][cpu*2+1]
        p['ns1']=str(int(p['ns0'])+124594);p['tsc1']=str(int(p['tsc0'])+124594)
        delayed['CLOCKSOURCE_TERMINAL'][cpu].update(maximum_reference_ns='124594',last_tsc=p['tsc1'],last_ns=p['ns1'])
    C.terminal(delayed,positive);cases.append(dict(name='slow-reference-full-rate-interval-valid-no-IRQ-pass',status='PASS'))
    observed=dict(CLOCK_BODY=[dict(cpu=str(c),count='1',phase='4') for c in range(2)],
        TICK_BODY=[dict(cpu=str(c),count='1') for c in range(2)],
        TICK_SAMPLE=[dict(cpu=str(c),source='2',source_epoch='2') for c in range(2)])
    C.observations(observed,positive);cases.append(dict(name='bound-synthetic-clock-observations',status='PASS'))
    for name,table,key,value in [('old-clock-as-fast','CLOCK_BODY','phase','3'),
        ('HPET-as-fast-counter','TICK_SAMPLE','source','1'),('stale-source-epoch','TICK_SAMPLE','source_epoch','1')]:
        bad=copy.deepcopy(observed);bad[table][0][key]=value
        try:C.observations(bad,positive)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    mutations=[('missing-terminal-CPU',lambda r:r['CLOCKSOURCE_TERMINAL'].pop()),
        ('missing-reference',lambda r:r['CLOCKSOURCE_REFERENCE'].pop()),
        ('unknown-reference',lambda r:r['CLOCKSOURCE_REFERENCE'].append(copy.deepcopy(r['CLOCKSOURCE_REFERENCE'][0])))]
    for key,value in [('version','1'),('epoch','1'),('cpu','1'),('point','0'),('shift0','63'),('ns0','0')]:
        mutations.append(('reference-'+key,lambda r,k=key,v=value:r['CLOCKSOURCE_REFERENCE'][1].update({k:v})))
    def wrong_rate(r):
        p=r['CLOCKSOURCE_REFERENCE'][1]
        p['ns']=str(int(p['ns'])+20000000);p['ticks']=str(int(p['ticks'])+2000000)
    mutations.append(('independent-rate-disagreement',wrong_rate))
    def uncertain_rate(r):
        p=r['CLOCKSOURCE_REFERENCE'][1]
        p['ns1']=str(int(p['ns0'])+20000000);p['tsc1']=str(int(p['tsc0'])+20000000)
        r['CLOCKSOURCE_TERMINAL'][0].update(maximum_reference_ns='20000000',last_tsc=p['tsc1'],last_ns=p['ns1'])
    mutations.append(('rate-interval-only-overlaps-limit',uncertain_rate))
    for key,value in [('epoch','1'),('source','1'),('reason','6'),('busy','1'),('registered','0'),('reads','0'),('last_tsc','0'),('references','0'),('maximum_reference_ns','1')]:
        mutations.append(('terminal-'+key,lambda r,k=key,v=value:r['CLOCKSOURCE_TERMINAL'][0].update({k:v})))
    for name,change in mutations:
        bad=copy.deepcopy(raw);change(bad)
        try:C.terminal(bad,positive)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return dict(status='PASS',authority='HOSTED_AND_SYNTHETIC_ONLY',cases=cases,
        source=positive,terminal=raw,implementation_acceptance=False)

def terminal_history(serials):
    """Reconstruct captures from the real producer; mutations are verifier probes."""
    cases=[];captures={}
    for kind,serial in serials.items():
        boot='\n'.join(line for line in serial.splitlines()
                       if not line.startswith(('[CLOCKSOURCE_TERMINAL]','[CLOCKSOURCE_REFERENCE]')))+'\n'
        selection=C.boot(boot,2,2)
        raw={'CLOCKSOURCE_TERMINAL':[], 'CLOCKSOURCE_REFERENCE':[]}
        for line in serial.splitlines():
            marker=line.split(']',1)[0][1:]
            if marker in raw:raw[marker].append(dict(p.split('=',1) for p in line.split()[1:]))
        C.terminal(raw,selection);captures[kind]=(raw,selection)
        cases.append(dict(name='actual-producer-history-'+kind,status='PASS'))
        # Both the boot formatter and terminal collection may arrive in CPU order
        # different from numeric slot order; their explicit identities control.
        reordered=C.boot('\n'.join(reversed(boot.splitlines()))+'\n',2,2)
        reversed_raw={k:list(reversed(v)) for k,v in raw.items()}
        C.terminal(reversed_raw,reordered)
        cases.append(dict(name='reordered-CPU-history-'+kind,status='PASS'))
    raw,selection=captures['2']
    for cpu,row in enumerate(raw['CLOCKSOURCE_TERMINAL']):
        p=raw['CLOCKSOURCE_REFERENCE'][cpu*2+1]
        assert int(row['reads'])==int(row['references'])==1
        assert int(row['last_tsc'])<int(p['tsc0'])<int(p['tsc1'])
        assert int(row['last_ns'])<int(p['ns0'])<int(p['ns1'])
    raw,selection=captures['0']
    assert int(raw['CLOCKSOURCE_TERMINAL'][1]['last_ns'])<int(selection['cpus'][1]['points'][-1]['ns'])
    mutations=[]
    for field,boot_field in [('last_tsc','tsc1'),('last_ns','ns1')]:
        for value in ('1',selection['cpus'][0]['points'][-1][boot_field]):
            mutations.append(('0','last-sample-'+field+'-'+value,
                lambda r,k=field,v=value:r['CLOCKSOURCE_TERMINAL'][0].update({k:v}),
                'last successful sample predates qualification'))
    mutations.extend([
        ('3','more-references-than-reads',lambda r:r['CLOCKSOURCE_TERMINAL'][0].update(reads='1'),
         'references exceed successful reads'),
        ('3','one-reference-with-runtime-predecessor',lambda r:r['CLOCKSOURCE_TERMINAL'][0].update(references='1'),
         'first reference predecessor'),
        ('2','changed-boot-predecessor-snapshot',lambda r:r['CLOCKSOURCE_REFERENCE'][0].update(retry0='1'),
         'first reference predecessor'),
        ('0','zero-references-with-maximum',lambda r:r['CLOCKSOURCE_TERMINAL'][0].update(maximum_reference_ns='1'),
         'lost reference maximum'),
    ])
    def overlap_boot(r):
        p=copy.deepcopy(captures['3'][1]['cpus'][0]['points'][-1]);p.pop('index')
        p.update(point='0',epoch='2',tsc0=str(int(p['tsc0'])+1),ns0=str(int(p['ns0'])+1))
        r['CLOCKSOURCE_REFERENCE'][0]=p
    mutations.append(('3','runtime-predecessor-inside-boot',overlap_boot,'independent rate agreement'))
    def equal_maximum(r):
        p=r['CLOCKSOURCE_REFERENCE'][0];width=int(r['CLOCKSOURCE_TERMINAL'][0]['maximum_reference_ns'])
        p.update(ns1=str(int(p['ns0'])+width),tsc1=str(int(p['tsc0'])+width))
    mutations.append(('3','predecessor-already-had-maximum',equal_maximum,'maximum did not exceed predecessor'))
    def changed_reference_source(r):
        for p in r['CLOCKSOURCE_REFERENCE'][:2]:p.update(mode='2',retries='0')
    mutations.append(('3','runtime-reference-source-changed',changed_reference_source,'runtime scale/epoch'))
    for kind,name,change,reason in mutations:
        original,selection=captures[kind];bad=copy.deepcopy(original);change(bad)
        try:C.terminal(bad,selection)
        except ValueError as error:
            assert reason in str(error),(name,str(error))
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED',reason=reason))
    for name,change in (
        ('duplicate-prepared-CPU',lambda s:s['cpus'].__setitem__(1,copy.deepcopy(s['cpus'][0]))),
        ('unknown-prepared-CPU',lambda s:s['cpus'][1].update(cpu=2))):
        raw,selection=captures['2'];bad=copy.deepcopy(selection);change(bad)
        try:C.terminal(raw,bad)
        except ValueError as error:assert 'prepared CPU identity' in str(error)
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return dict(status='PASS',authority='ACTUAL_PRODUCER_SYNTHETIC_CLOCKS_AND_SEMANTIC_NEGATIVES',cases=cases)

def run(binary,output):
    output.mkdir(parents=True,exist_ok=False)
    serial=subprocess.check_output([str(binary.resolve()),'--boot-report']).decode()
    (output/'actual-hosted-boot.txt').write_text(serial)
    result=reconstruct(serial)
    serials={str(kind):subprocess.check_output([str(binary.resolve()),'--terminal-report',str(kind)]).decode()
             for kind in (0,1,2,3,4,5,7)}
    E.write(output/'actual-terminal-history.json',serials)
    result['terminal_history']=terminal_history(serials)
    result['device_adapter']=device_adapter(output/'device-adapter')
    E.write(output/'result.json',result)
    return result

def verify(output):
    expected=reconstruct((output/'actual-hosted-boot.txt').read_text())
    expected['terminal_history']=terminal_history(json.loads((output/'actual-terminal-history.json').read_text()))
    saved=json.loads((output/'result.json').read_text())
    E.require(saved.pop('device_adapter')==device_adapter(output/'device-adapter',verify=True),
              'clocksource device adapter reconstruction differs')
    E.require(saved==expected,'clocksource fixture reconstruction differs')
    return expected

def device_adapter(output,verify=False):
    spec=importlib.util.spec_from_file_location('clocksource_device_adapter',E.ROOT/'scripts/device-mapping-host.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    names=('makefile','kernel/src/memory/pmem.c','kernel/src/core/clocksource.c',
           'kernel/src/core/pvclock.c','scripts/irq-latency-production-changes.json')
    if not verify:
        for n in names:
            p=output/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((E.ROOT/n).read_bytes())
    for n in names:E.require((output/n).read_bytes()==(E.ROOT/n).read_bytes(),'device adapter source identity')
    if verify:
        with tempfile.TemporaryDirectory(prefix='clocksource-adapter-verify-') as temporary:
            return device_adapter(Path(temporary))
    contract=output/'scripts/irq-latency-production-changes.json';original=contract.read_bytes()
    d=json.loads(original);cases=[]
    with patch.object(module,'ROOT',output):
        expected='DEVICE_DMA_HOST checks=532952 logical_frames=268435456 privileged_operations=SUBSTITUTED status=PASS\n'
        assert module.expected_summary('dma')==expected
        for name,change in (
            ('current-as-old-schema',lambda c:c.update(schema=1)),
            ('unknown-schema',lambda c:c.update(schema=3)),
            ('missing-production-clock',lambda c:c['new_units'].pop('kernel/src/core/clocksource.c')),
            ('wrong-production-clock',lambda c:c['new_units']['kernel/src/core/clocksource.c'].update(current_sha256='0'*64)),
            ('weakened-PMM-kind',lambda c:c['units']['kernel/src/memory/pmem.c'].update(kind='instrumentation-only'))):
            bad=copy.deepcopy(d);change(bad)
            try:
                contract.write_bytes(E.canonical(bad))
                try:module.expected_summary('dma')
                except ValueError:pass
                else:raise AssertionError(name)
            finally:contract.write_bytes(original)
            cases.append(dict(name=name,status='REJECTED'))
        assert module.expected_summary('dma')==expected
    return dict(status='PASS',cases=cases,source_sha256=E.sha(E.ROOT/'scripts/device-mapping-host.py'))
