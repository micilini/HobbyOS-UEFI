#!/usr/bin/env python3
"""Preserved diagnostic negatives and actual compiler controls, no acceptance."""
import importlib.util
import json
import shutil
from pathlib import Path
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_production as P


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def run(history,baseline,current,output):
    output.mkdir(parents=True,exist_ok=False)
    serial=history/'attempts/components-kvm8/qemu-serial.log'
    run_dir=serial.parent
    failed=history/'correction/experiments/token-observer/runtime/qemu-serial.log'
    results={}
    results['historical_raw']=V.inspect(serial.read_text())
    E.require(results['historical_raw']==json.loads((history/'evidence/components-raw-replay.json').read_text()),
              'historical original facts changed')
    for key,name,args in (
        ('raw','irq-latency-fixtures',(serial,)),
        ('workload','irq-latency-workload-fixtures',(serial.read_text(),8)),
        ('capacity','irq-latency-authority-fixtures',(run_dir/'host-running.json',run_dir/'launch.json')),
        ('allowance','irq-latency-envelope-fixtures',(serial,)),
        ('scheduling','irq-latency-scheduling-fixtures',()),
        ('scope','irq-latency-scope-fixtures',(baseline,output/'scope')),
        ('retrospective','irq-latency-pairing-stream',(failed,output/'retrospective'))):
        results[key]=module(name).run(*args)
    controls=output/'production';controls.mkdir()
    shutil.copytree(baseline,controls/'baseline');shutil.copytree(current,controls/'current')
    record=json.loads((history/'evidence/production-code-identity/result.json').read_text())
    change_contract=json.loads((current/'scripts/irq-latency-production-changes.json').read_text())
    results['production']=module('irq-latency-production-fixtures').controls(controls/'baseline',controls/'current',record,controls,change_contract)
    results.update(status='PASS',implementation_acceptance=False,complete_portable_acceptance=False)
    E.write(output/'result.json',results);return results
