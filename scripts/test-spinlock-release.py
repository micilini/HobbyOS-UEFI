#!/usr/bin/env python3
"""Actual x86 ticket release, incoming interference witness and emitted code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
MODES={'normal':[], 'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
       'asan':['-fsanitize=address','-fno-omit-frame-pointer']}

def run(output,before=None):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    rows=[]
    def command(argv,name):
        p=subprocess.run(argv,cwd=ROOT,capture_output=True,text=True,timeout=180,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1'})
        (output/(name+'.log')).write_text(p.stdout+p.stderr)
        rows.append({'command':argv,'exit':p.returncode,'log':name+'.log'})
        (output/'commands.json').write_text(json.dumps(rows,indent=2)+'\n')
        if p.returncode:raise RuntimeError(name+': '+p.stdout+p.stderr)
        return p.stdout
    definitions=[]
    if before:
        source=before.read_text();a=source.index('void spin_unlock(');b=source.index('\nirq_flags_t spin_lock_irqsave',a)
        body=source[a:b];assert body.count('__atomic_compare_exchange_n(')==1
        generated=output/'incoming.c'
        generated.write_text(source[:a]+body.replace('__atomic_compare_exchange_n(','release_compare(')+source[b:])
        definitions=['-DINCOMING_RELEASE_CONTROL=1','-DSPINLOCK_ACTUAL_SOURCE="'+str(generated)+'"',
                     '-Wno-unused-function','-Wno-unused-variable']
    for mode,flags in MODES.items():
        binary=output/('release-'+mode)
        command(['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-pthread',
            '-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=0','-iquote',str(ROOT/'kernel/src/core'),
            *definitions,*flags,'scripts/spinlock-release-host.c','-o',str(binary)],mode+'-compile')
        command([str(binary)],mode+'-execute')
    if not before:
        profiles={'production':['-DHOBBYOS_IRQ_LATENCY=0','-DHOBBYOS_DEBUG_ASSERT=0'],
            'debug':['-DHOBBYOS_IRQ_LATENCY=0','-DHOBBYOS_DEBUG_ASSERT=1'],
            'selftest':['-DHOBBYOS_IRQ_LATENCY=0','-DHOBBYOS_DEBUG_ASSERT=1','-DHOBBYOS_SELFTEST=1'],
            'latency':['-DHOBBYOS_IRQ_LATENCY=1','-DHOBBYOS_DEBUG_ASSERT=1','-DHOBBYOS_SELFTEST=1',
                       '-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1']}
        for profile,defines in profiles.items():
            obj=output/(profile+'.o')
            command(['gcc','-std=gnu11','-O2','-ffreestanding','-mno-red-zone','-mgeneral-regs-only',
                *defines,'-c','kernel/src/core/spinlock.c','-o',str(obj)],profile+'-codegen-compile')
            asm=command(['objdump','-dr',str(obj)],profile+'-codegen')
            body=asm.split('<spin_unlock>:',1)[1].split('\n\n',1)[0]
            assert len(re.findall(r'lock addw\s+\$0x1,0x4\(%[a-z0-9]+\)',body))==1
            assert not re.search(r'\b(cmpxchg|j[a-z]+)\b',body)
            if profile=='latency':
                assert body.count('irq_latency_interior_before_unlock')==1
                assert body.index('irq_latency_interior_before_unlock')<body.index('lock addw')
            else:assert not re.search(r'\bcall\b',body)
            symbols=command(['nm','-S',str(obj)],profile+'-symbols')
            if profile in ('production','debug'):assert 'g_spinlock_test_observer' not in symbols
    result={'status':'PASS','actual_executions':3,'incoming_control':bool(before),
        'source_sha256':hashlib.sha256((before or ROOT/'kernel/src/core/spinlock.c').read_bytes()).hexdigest(),
        'timing_acceptance':False,'authority':'Actual x86 code and control flow; hosted IRQ substitutes; no elapsed-time bound'}
    (output/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--before',type=Path);a=p.parse_args();print(json.dumps(run(a.output,a.before)))
