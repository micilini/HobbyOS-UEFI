#!/usr/bin/env python3
"""Compile unchanged function bodies from the driver with explicit device seams."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import os

ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    match=re.search(r'^void '+name+r'\([^;]+?\)\s*\{|^int '+name+r'\([^;]+?\)\s*\{',source,re.M)
    if not match:raise ValueError('missing actual function '+name)
    start=match.start();cursor=source.index('{',start);depth=0
    for end in range(cursor,len(source)):
        if source[end]=='{':depth+=1
        if source[end]=='}':
            depth-=1
            if not depth:return source[start:end+1]+'\n'
    raise ValueError('unclosed actual function '+name)

def run(output,source,legacy):
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    text=source.read_text()
    selected=['xhci_parse_config','xhci_configure_owned_root','xhci_configure_device_with_context']
    bodies={name:function(text,name) for name in selected}
    generated=output/'enumeration.actual.c';generated.write_text('\n'.join(bodies.values()))
    rows=[]
    for mode,flags in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        binary=output/mode
        command=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie',
                 *flags,'-DUSB_ENUMERATION_ACTUAL_SOURCE="'+str(generated)+'"',
                 *(['-DEXPECT_LEGACY_ENUMERATION=1'] if legacy else []),
                 str(ROOT/'scripts/usb-enumeration-host.c'),'-o',str(binary)]
        for phase,args in [('compile',command),('execute',[str(binary)])]:
            result=subprocess.run(args,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            (output/(mode+'-'+phase+'.log')).write_text(result.stdout)
            if result.returncode:raise ValueError(mode+' '+phase+': '+result.stdout)
        expected=10 if legacy else 37
        assert result.stdout==f'USB_ENUMERATION_HOST cases={expected} status=PASS authority=actual_functions_substituted_device\n'
        rows.append(dict(mode=mode,command=command,summary=result.stdout,sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    receipt=dict(status='PASS',legacy_witness=legacy,actual_executions=len(rows),executions=rows,
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        functions={name:hashlib.sha256(body.encode()).hexdigest() for name,body in bodies.items()},
        authority='Actual parser/enumeration flow. Hardware, command completion and topology substituted.')
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');return receipt

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True)
    p.add_argument('--source',type=Path,default=ROOT/'kernel/src/drivers/usb/xhci/xhci.c')
    p.add_argument('--legacy-witness',action='store_true');a=p.parse_args()
    r=run(a.output,a.source,a.legacy_witness)
    print(json.dumps({k:r[k] for k in ('status','legacy_witness','actual_executions')}))
