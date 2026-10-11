#!/usr/bin/env python3
"""Compile portable production pairs and exercise their actual rejection paths."""
import argparse
import copy
import io
import json
from pathlib import Path
import tarfile
import zipfile
import memory_evidence as E
import irq_latency_production as P


def run(archive, output):
    E.require(E.sha(archive)=='62edece893865d0e06a857df16c3c20b05333fc0522ab8e55a0f951a129f6b7b',
              'reviewed diagnostic archive identity')
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    baseline=output/'baseline';current=output/'current';baseline.mkdir();current.mkdir()
    with zipfile.ZipFile(archive) as z:
        source=json.loads(z.read('source-manifest.json'))
        for row in source['files']:
            E.safe(row['path']);p=current/row['path'];p.parent.mkdir(parents=True,exist_ok=True)
            data=z.read('source/'+row['path']);p.write_bytes(data);p.chmod(row['mode'])
            E.require(E.sha(p)==row['sha256'], 'reviewed current source hash')
        mapping={r['path']:r for r in json.loads(z.read('session-map.json'))['files']}
        tar=z.read(mapping['before/starting-source.tar.gz']['storage'])
        record=json.loads(z.read(mapping['evidence/production-code-identity/result.json']['storage']))
        with tarfile.open(fileobj=io.BytesIO(tar)) as t:
            for member in t.getmembers():
                E.safe(member.name);E.require(member.isfile(),'baseline regular member')
                p=baseline/member.name;p.parent.mkdir(parents=True,exist_ok=True)
                p.write_bytes(t.extractfile(member).read());p.chmod(0o755 if member.mode & 0o111 else 0o644)
    return controls(baseline,current,record,output)


def controls(baseline,current,record,output,change_contract=None):
    """Both roots are disposable copies; immutable exports are never mutated."""
    result=P.compare(baseline,current,record,output/'paired',change_contract=change_contract)
    cases=[]
    def reject(name,fn,reason):
        try:fn()
        except ValueError as error:
            E.require(reason in str(error),'production fixture wrong rejection: '+name+': '+str(error))
            cases.append({'name':name,'synthetic':True,'result':'REJECTED','reason':str(error)})
        else:raise ValueError('production mutation accepted: '+name)
    if change_contract is not None:
        for name,mutate,reason in (
            ('missing-change-unit',lambda c:c['units'].pop('kernel/src/memory/pmem.c'),'change contract inventory'),
            ('hidden-clock-critical-section-correction',lambda c:c['units']['kernel/src/core/clock.c'].update(kind='instrumentation-only'),'change semantic contract'),
            ('missing-clock-revalidation-semantics',lambda c:c['units']['kernel/src/core/clock.c'].update(subjects=[]),'change semantic contract'),
            ('hidden-request-telemetry-correction',lambda c:c['units']['kernel/src/smp/cpu_request.c'].update(kind='instrumentation-only'),'change semantic contract'),
            ('hidden-local-telemetry-correction',lambda c:c['units']['kernel/src/memory/kernel_tlb.c'].update(kind='instrumentation-only'),'change semantic contract'),
            ('hidden-sample-layout-correction',lambda c:c['units']['kernel/src/smp/cpu_request_state.c'].update(kind='instrumentation-only'),'change semantic contract'),
            ('wrong-change-source',lambda c:c['units']['kernel/src/memory/pmem.c'].update(current_sha256='0'*64),'intentional source binding'),
            ('hidden-production-change',lambda c:c['units']['kernel/src/memory/pmem.c'].update(kind='instrumentation-only'),'change semantic contract'),
            ('hidden-heap-preparation-change',lambda c:c['units']['kernel/src/memory/heap.c'].update(kind='instrumentation-only'),'change semantic contract'),
            ('missing-heap-preparation-semantics',lambda c:c['units']['kernel/src/memory/heap.c'].update(subjects=[]),'change semantic contract'),
            ('missing-change-semantics',lambda c:c['units']['kernel/src/memory/pmem.c'].update(subjects=[]),'change semantic contract')):
            change=copy.deepcopy(change_contract);mutate(change)
            reject(name,lambda:P.compare(baseline,current,record,output/name,change_contract=change),reason)
    changed=copy.deepcopy(record);changed['rows'].pop()
    reject('omitted-unit',lambda:P.validate_recorded(changed),'unit inventory')
    changed=copy.deepcopy(record);changed['rows'][0]['argv'][1]='-fhosted'
    reject('changed-production-flag',lambda:P.validate_recorded(changed),'compiler flags/output')
    changed=copy.deepcopy(record);changed['rows'][0]['argv'][-1]='/tmp/unchecked-output.o'
    reject('unexpected-recorded-output',lambda:P.validate_recorded(changed),'compiler flags/output')
    changed=copy.deepcopy(record);changed['rows'][0]['argv'][0]='sh'
    reject('unexpected-compiler-command',lambda:P.validate_recorded(changed),'compiler flags/output')
    changed=copy.deepcopy(record);changed['rows'][0]['exit']=1
    reject('ignored-recorded-compile-failure',lambda:P.validate_recorded(changed),'recorded failed/different object')
    reject('current-as-baseline',lambda:P.compare(current,current,record,output/'substitution'),
           'current source used as baseline')
    reject('scratch-inside-source',lambda:P.compare(baseline,current,record,current/'bad-output'),
           'writable output path')
    target=baseline/'kernel/src/core/idt.c';original=target.read_bytes()
    try:
        target.write_bytes(original+b'\n/* synthetic baseline drift */\n')
        reject('baseline-tree-drift',lambda:P.approved_baseline(baseline),'approved baseline tree')
    finally:target.write_bytes(original)
    P.approved_baseline(baseline)
    target=current/'kernel/src/core/idt.c';original=target.read_bytes()
    for name,suffix,reason in [
        ('actual-production-difference',b'\nint irq_latency_fixture_difference = 1;\n','local paired objects differ'),
        ('actual-compilation-failure',b'\n#error synthetic production compile failure\n','local compilation failed')]:
        destination=output/name;destination.mkdir()
        try:
            target.write_bytes(original+suffix)
            reject(name,lambda:P.compile_pair(baseline,current,'kernel/src/core/idt.c',destination,
                   result['local_toolchain']['driver']),reason)
        finally:target.write_bytes(original)
        restored=output/(name+'-restored');restored.mkdir()
        P.compile_pair(baseline,current,'kernel/src/core/idt.c',restored,result['local_toolchain']['driver'])
    P.validate_recorded(record)
    if change_contract is not None and 'kernel/src/cpu/mmio.c' in change_contract['units']:
        E.require(result['ordinary_debug_mmio']['object_equal'], 'ordinary DEBUG MMIO changed')
        target=current/'kernel/src/cpu/mmio.c';original=target.read_bytes()
        destination=output/'actual-debug-mmio-difference';destination.mkdir()
        try:
            target.write_bytes(original+b'\n#if HOBBYOS_DEBUG_ASSERT\nint irq_latency_fixture_debug_difference=1;\n#endif\n')
            reject('actual-debug-mmio-difference',lambda:P.compile_pair(baseline,current,'kernel/src/cpu/mmio.c',
                destination,result['local_toolchain']['driver'],change_contract['units']['kernel/src/cpu/mmio.c'],
                ordinary_debug=True),'local paired objects differ')
        finally:target.write_bytes(original)
        P.compile_pair(baseline,current,'kernel/src/cpu/mmio.c',output/'actual-debug-mmio-restored',
            result['local_toolchain']['driver'],change_contract['units']['kernel/src/cpu/mmio.c'],ordinary_debug=True)
    E.require(result['recorded_build']['exact_reproduction']=='UNAVAILABLE_TOOLCHAIN_IDENTITY',
              'missing original full toolchain fingerprint misrepresented')
    summary={'schema':1,'status':'PASS','paired_production_units':len(result['rows']),
        'mode':result['mode'],'recorded_build':result['recorded_build'],'cases':cases,
        'restored_compiled_positives':2,'runtime_acceptance':False,
        'reviewer_integral_historical_status':'STOPPED_AT_CROSS_TOOLCHAIN_OBJECT_HASH_CHECK',
        'source_exports_immutable_during_compilation':True}
    E.write(output/'fixtures.json',summary)
    return summary


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    print(json.dumps(run(a.archive,a.output)))
