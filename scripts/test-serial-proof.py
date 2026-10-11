#!/usr/bin/env python3
"""Actual UART, command and retained proof tests; compile only in scratch."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
MODES={'normal':[], 'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
       'asan':['-fsanitize=address','-fno-omit-frame-pointer']}

def run(output):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    source=(ROOT/'kernel/src/drivers/serial.c').read_text()
    old='__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));'
    assert source.count(old)==1
    source=source.replace(old,'host_outb(port,val);')
    old='__asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));'
    assert source.count(old)==1
    source=source.replace(old,'ret=host_inb(port);')
    generated=output/'serial.actual.c';generated.write_text(source)
    rows=[]
    common=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread']
    diagnostic=['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=1',
                '-DHOBBYOS_SERIAL_PROOF_DIAGNOSTIC=1','-DHOBBYOS_SERIAL_PROOF_ID=0x1234ULL']
    subjects={
      'serial':([*diagnostic,'-I'+str(ROOT/'kernel/src/drivers'),'-DSERIAL_ACTUAL_SOURCE="'+str(generated)+'"'],['scripts/serial-proof-host.c']),
      'record':(diagnostic,['scripts/serial-proof-record-host.c']),
      'command':(diagnostic,['scripts/serial-proof-command-host.c']),
      'summary':(diagnostic,['scripts/serial-proof-summary-host.c','kernel/src/core/irq_latency_time.c']),
      'boot-publication':(diagnostic,['scripts/serial-proof-boot-host.c']),
      'tlb-proof':(['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=0'],['scripts/tlb-proof-host.c','kernel/src/memory/tlb_proof.c']),
      'tlb-emitter':(['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=0'],['scripts/tlb-proof-emitter-host.c','kernel/src/memory/tlb_proof.c']),
      'tlb-uart-export':(['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=0','-DHOBBYOS_SERIAL_PROOF_DIAGNOSTIC=1'],['scripts/tlb-proof-emitter-host.c','kernel/src/memory/tlb_proof.c']),
    }
    for mode,flags in MODES.items():
        for name,(defines,files) in subjects.items():
            binary=output/(name+'-'+mode)
            commands=[[*common,*flags,*defines,*files,'-o',str(binary)],[str(binary)]]
            for phase,command in zip(('compile','execute'),commands):
                result=subprocess.run(command,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
                (output/(name+'-'+mode+'-'+phase+'.log')).write_text(result.stdout)
                if result.returncode:raise RuntimeError(f'{name} {mode} {phase}: {result.stdout}')
            rows.append(dict(subject=name,mode=mode,commands=commands,sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    receipt=dict(status='PASS',actual_executions=len(rows),executions=rows,
        source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
            [ROOT/'kernel/src/drivers/serial.c',ROOT/'kernel/src/shell/commands/cmd_irqlatency.c',ROOT/'kernel/src/memory/tlb_fixture.c']},
        authority='Hosted substituted UART/IRQ effects; no hardware timing or invalidation claim')
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();r=run(args.output);print(json.dumps({k:r[k] for k in ('status','actual_executions','authority')}))
