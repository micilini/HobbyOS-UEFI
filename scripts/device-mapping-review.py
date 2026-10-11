#!/usr/bin/env python3
"""Focused owned-mapping delivery through the accepted common transport."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import memory_evidence as E
ROOT=Path(__file__).resolve().parent.parent

def module(name):
    spec=importlib.util.spec_from_file_location('owned_review_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('session','campaign','review'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--archive',type=Path);p.add_argument('--extract',type=Path);p.add_argument('--preview',action='store_true')
    a=p.parse_args();contract=os.environ.get('HOBBYOS_VALIDATION_CONTRACT','owned-device-mapping')
    E.require(contract in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'),'unknown review contract')
    stack=contract=='guarded-kernel-stacks';image=stack or contract=='kernel-image-protection';low=image or contract=='low-address-isolation';direct=low or contract=='verified-ram-direct-map';R=module('physical-translation-review')
    os.environ['HOBBYOS_VALIDATION_CONTRACT']=contract
    G=module('stack-guard-gate' if stack else 'kernel-wx-gate' if image else 'low-half-gate' if low else 'direct-map-gate' if direct else 'device-mapping-gate')
    for name in ('session','campaign','review'):
        value=getattr(a,name).resolve();value.relative_to(ROOT/'backups');setattr(a,name,value)
    derived=G.verify(a.campaign);accepted=json.loads((a.campaign/('stack-acceptance.json' if stack else 'image-acceptance.json' if image else 'low-acceptance.json' if low else 'direct-acceptance.json' if direct else 'device-acceptance.json')).read_text())
    E.require(accepted['status']=='PASS' and accepted['result']==derived and
        accepted['harness_sha256']==E.identity(E.manifest(harness=True)),'current owned acceptance differs from replay')
    if not a.preview:
        E.require(not subprocess.check_output(['git','status','--porcelain=v1'],cwd=ROOT).strip(),'completion source not clean committed')
        E.require(all((a.session/'evidence'/n).is_file() for n in ('closure.json','storage-preservation.json')),'completion receipts absent')
    E.require(bool(a.archive)==bool(a.extract) and not(a.preview and a.archive),'archive requires complete committed delivery')
    index=R.select(a.session,a.campaign,a.review,device=True,direct=direct,low=low,image=image,stack=stack)
    result={'status':'SELECTED','logical_files':len(index['raw'])+len(index['synthetic_deltas'])}
    if a.archive:
        a.archive=a.archive.resolve();a.archive.relative_to(ROOT/'backups')
        result=R.archive(a.review,a.archive,a.extract.resolve())
    print(json.dumps(result,sort_keys=True))
if __name__=='__main__':main()
