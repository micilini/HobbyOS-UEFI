#!/usr/bin/env python3
"""Exercise explicit observer scope and unchanged historical role selection."""
import argparse
import json
import os
from pathlib import Path
import shutil
import memory_evidence as E
import irq_latency_current as I
import irq_latency_production as P
import stack_current as C


def run(baseline, output):
    P.approved_baseline(baseline)
    output.mkdir(parents=True, exist_ok=False)
    env={key:os.environ.get(key) for key in ('HOBBYOS_VALIDATION_CONTRACT','HOBBYOS_MEMORY_OBSERVER_ROLE')}
    cases=[]
    def reject(name, fn, boundary):
        try:fn()
        except ValueError as error:
            E.require(boundary in str(error),'scope fixture rejection: '+str(error))
            cases.append({'name':name,'synthetic':True,'reason':str(error)})
        else:raise ValueError('scope mutation accepted: '+name)
    try:
        os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
        os.environ.pop('HOBBYOS_MEMORY_OBSERVER_ROLE',None)
        E.require(C.contract(baseline)['name']=='guarded-kernel-stacks','historical role changed')
        E.require(I.build_flags('selftest')==[],'historical build options changed')
        reject('dirty-observer-as-historical',lambda:C.contract(),'stack protected')
        os.environ['HOBBYOS_MEMORY_OBSERVER_ROLE']=I.ROLE
        current=C.contract();C.source_guard();C.audit_sites();C.boundary_semantics()
        E.require(I.build_flags('selftest')==['IRQ_LATENCY=1'] and
                  I.build_flags('production')==['IRQ_LATENCY=0'],'current build role separation')
        os.environ['HOBBYOS_VALIDATION_CONTRACT']='kernel-image-protection'
        reject('wrong-inherited-role',I.enabled,'explicit inherited guarded-stack role')
        os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
        os.environ['HOBBYOS_MEMORY_OBSERVER_ROLE']='unknown'
        reject('unknown-observer-role',I.enabled,'unknown memory observer role')
        os.environ['HOBBYOS_MEMORY_OBSERVER_ROLE']=I.ROLE
        root=output/'synthetic-source';root.mkdir()
        names=set(current['immutable_reviewed_anchors'])|set(current['promoted_sources'])|{
            'scripts/irq-latency-scope.json','scripts/irq-latency-conversion-sites.json'}
        for name in sorted(names):
            dest=root/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(E.ROOT/name,dest)
        for name in ('kernel/src/graphics/graphics.c','kernel/src/core/spinlock.c'):
            p=root/name;original=p.read_bytes()
            try:
                p.write_bytes(original+b'\n/* explicit synthetic source drift */\n')
                reject('changed-'+p.stem,lambda:C.contract(root),'observer '+('protected' if '/graphics/' in name else 'promoted'))
            finally:p.write_bytes(original)
            C.contract(root)
        p=root/'scripts/irq-latency-scope.json';original=p.read_bytes()
        changed=json.loads(original);changed['base_tree']='0'*40
        try:
            p.write_bytes(E.canonical(changed))
            reject('wrong-approved-tree',lambda:C.contract(root),'approved pre-edit identity')
        finally:p.write_bytes(original)
        C.contract(root)
        result={'schema':1,'status':'PASS','cases':cases,'historical_baseline':I.BASE,
            'current_role':I.ROLE,'current_semantics':'PASS','hardware':'NOT_RUN',
            'authority':'SOURCE_AND_ROLE_SELECTION_ONLY','full_campaign_acceptance':False}
        E.write(output/'result.json',result)
        return result
    finally:
        for key,value in env.items():
            if value is None:os.environ.pop(key,None)
            else:os.environ[key]=value


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    print(json.dumps(run(a.baseline.resolve(),a.output.resolve())))
