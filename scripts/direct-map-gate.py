#!/usr/bin/env python3
"""Verified RAM construction layered on the complete owned-memory gate."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import memory_evidence as E
import validation_resources as R
import validation_stages as S
import low_current as LOW

if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') not in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
    os.environ['HOBBYOS_VALIDATION_CONTRACT']='verified-ram-direct-map'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
ROOT=E.ROOT
def module(name):
    spec=importlib.util.spec_from_file_location('direct_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
D=module('device-mapping-gate');G=D.G;T=D.T
G.SURFACE+=('cmd_rammap','ram_map_fixture_run','pmm_test_alloc_range','paging_ram_scratch_table')

def read(path):return json.loads(path.read_text())
def receipt(evidence,name,result):T.receipt(evidence,'direct-'+name,result)
def plan():
    return {'schema':1,'build_input_sha256':E.identity(E.manifest()),'ordinary_plan':D.plan(),
        'supplements':{'high-ram':{'boots':3,'machine':'q35,max-ram-below-4g=1G','cpu':'host','accel':'kvm','smp':4,'memory':'2G'},
            'high-ram-fallback':{'boots':3,'machine':'q35,max-ram-below-4g=1G','cpu':'max,-pdpe1gb','accel':'tcg','smp':4,'memory':'2G'}},
        'direct_base':0xffff800000000000,'direct_span':0x400000000000,
        'paging_comparison':{'samples_per_profile':3,'maximum_ratio_to_original':0.5,
            'scope':'Unchanged host serial markers; no hardware timing claim'},
        'identity':'RETIRED' if LOW.enabled() else 'RETAINED','effective_framebuffer_type':'NOT_COMPUTED','hardware':'DEFERRED_TO_FINAL_CLOSURE'}

def static(evidence):
    D.static(evidence)
    import direct_current
    result=direct_current.boundary_semantics();result['scope']=direct_current.scope('verified-ram-direct-map')
    E.write(evidence/'direct-test-plan.json',plan());receipt(evidence,'static',result)

def host(evidence):
    destination=evidence
    if (evidence/'direct-host.json').exists():
        destination=evidence/'stage-revisions/direct-host'/G.stamp();destination.mkdir(parents=True)
    inputs={'build':E.manifest(),'harness':E.manifest(harness=True)}
    E.write(destination/'host-inputs.json',inputs)
    D.host(destination)
    path=destination/'direct-host'/G.stamp();module('direct-map-host').run(path)
    E.require(inputs=={'build':E.manifest(),'harness':E.manifest(harness=True)},'RAM hosted source changed during execution')
    receipt(destination,'host',{'path':path.relative_to(destination).as_posix(),'sha256':E.sha(path/'direct-host.json')})
    if destination!=evidence:
        E.write(destination/'complete.json',{'schema':1,'status':'PASS',
            'build_input_sha256':E.identity(inputs['build']),'harness_sha256':E.identity(inputs['harness']),
            'host_inputs_sha256':E.sha(destination/'host-inputs.json'),
            'direct_host_sha256':E.sha(destination/'direct-host.json')})

def host_base(evidence):
    revisions=sorted((evidence/'stage-revisions/direct-host').glob('*/complete.json'))
    if not revisions:return evidence
    path=revisions[-1];row=read(path);base=path.parent
    E.require(row['schema']==1 and row['status']=='PASS' and
        row['build_input_sha256']==E.identity(E.manifest()) and
        E.sha(base/'host-inputs.json')==row['host_inputs_sha256'] and
        E.sha(base/'direct-host.json')==row['direct_host_sha256'],'RAM hosted successor binding')
    inputs=read(base/'host-inputs.json')
    E.require(E.identity(inputs['build'])==row['build_input_sha256'] and
        E.identity(inputs['harness'])==row['harness_sha256'],'RAM hosted source manifest binding')
    return base

def build(evidence):
    D.build(evidence)
    result={}
    for role in ('selftest','debug','production','framebuffer-reference'):
        path=evidence/'candidates'/role;symbols=(path/'symbols.txt').read_text()
        for symbol in ('paging_build_create','paging_build_verify','paging_build_activate','ram_map_plan_verify',
                       'physical_pointer_range','pmm_direct_map_activate','paging_boot_requirements'):
            E.require(re.search(r'\b'+symbol+r'$',symbols,re.M),'RAM production implementation absent: '+symbol)
        for symbol in ('cmd_rammap','pmm_test_alloc_range','ram_map_fixture_run'):
            E.require(bool(re.search(r'\b'+symbol+r'$',symbols,re.M))==(role in ('selftest','framebuffer-reference')),
                'RAM test isolation: '+role+':'+symbol)
        result[role]={'candidate_id':G.verify_local_candidate(path),'symbols_sha256':E.sha(path/'symbols.txt')}
    receipt(evidence,'build',result)

def owner_proof(evidence):
    import direct_map_evidence as RAM
    selected=read(evidence/'direct-owner-readback.json');E.safe(selected['path'])
    path=evidence/selected['path'];run=read(path/'result.json')
    candidate=evidence/'candidates/selftest'
    identity=E.verify_candidate(read(candidate/'candidate.json'),read(candidate/'build-receipt.json'))
    E.require(run['candidate_id']==identity and
        E.sha(path/'result.json')==selected['result_sha256'],'RAM owner run/candidate binding')
    D.V.verify_complete_run(path)
    actual=RAM.owner_readback(path);E.require(actual==selected['result'],'RAM owner readback result differs')
    return actual

def paging_measurements(evidence):
    import statistics
    validator=module('verify-memory-observation');series=read(evidence/'series.json');result={}
    for profile,original,owned in (('q35-tcg-smp4',1415497788,2564996943),
                                    ('q35-kvm-smp4',156314361,176817922)):
        selected=[name for name in series['runs'] if name.startswith('profiles/'+profile+'-')]
        E.require(len(selected)==3,'RAM paging sample profile/repetition coverage')
        samples=[];identities=set()
        for name in selected:
            path=evidence/name;run=read(path/'result.json');identity=(run['vm_id'],run['boot_id'])
            E.require(identity not in identities,'RAM repeated paging boot');identities.add(identity)
            samples.append({'path':name,'candidate_id':run['candidate_id'],'vm_id':run['vm_id'],
                'boot_id':run['boot_id'],'serial_sha256':E.sha(path/'qemu-serial.log'),
                'observations_sha256':E.sha(path/'serial-observations.jsonl'),
                'timing':validator.timing(path,(path/'qemu-serial.log').read_bytes())})
        median=statistics.median(s['timing']['duration_ns'] for s in samples)
        E.require(median*2<=original,'RAM paging construction is not materially below original observation')
        result[profile]={'samples':samples,'median_ns':median,'ratio_to_original':median/original,
            'ratio_to_owned_mapping':median/owned}
    return {'schema':1,'status':'PASS','profiles':result,
        'authority':'Host serial-reception intervals; unquantified buffering/scheduling uncertainty; earlier PAT marker scope differs.'}

def runtime(evidence):
    if (evidence/'direct-runtime.json').exists():
        for gate,prefix in ((D.P,'physical'),(D,'device')):
            S.revalidate_runtime(evidence,prefix,lambda path,gate=gate,prefix=prefix:
                S.stage(gate,evidence,prefix,'runtime',receipt_override=path))
        S.revalidate_runtime(evidence,'direct',lambda path:reusable(evidence,'runtime',receipt_override=path))
        return
    state=R.classify(evidence/'device-runtime.json',
        lambda row:D.verify_runtime_recorded(evidence,row['result']),source=E.identity(E.manifest()))
    if not R.require_successor(state):D.runtime(evidence)
    if not (evidence/'direct-owner-readback.json').exists():
        import direct_map_evidence as RAM
        R.before_step(evidence,'runtime')
        name=None
        if LOW.enabled():
            import low_address_evidence
            diagnostics=read(evidence/'series.json')['diagnostics']
            E.require(len(diagnostics)==1,'one independent current bootstrap diagnostic required')
            name=diagnostics[0];E.safe(name)
            D.V.verify_complete_run(evidence/name)
            low_address_evidence.readback(evidence/name)
        reused=name is not None
        name=name or 'ram-owner-diagnostic/'+G.stamp();path=evidence/name
        argv=['python3','-B','scripts/memory-qemu.py','--evidence',str(path),
            '--candidate',str(evidence/'candidates/selftest'),'--machine','q35','--accel','kvm',
            '--smp','4','--readback','--serial-backend','file']
        if LOW.enabled():argv+=['--low-bootstrap-readback']
        for command in D.V.COMMANDS:argv+=['--command',command]
        if not reused:G.run(evidence,'ram-owner-readback',argv)
        D.V.verify_complete_run(path)
        actual=RAM.owner_readback(path)
        import validation_retention
        validation_retention.retire_run(path)
        E.write(evidence/'direct-owner-readback.json',{'schema':1,'status':'PASS','path':name,
            'result_sha256':E.sha(path/'result.json'),'result':actual})
    receipt(evidence,'runtime',{'inherited':D.verify_runtime(evidence),'owners':owner_proof(evidence),
        'paging':paging_measurements(evidence)})
def production(evidence):D.production(evidence);receipt(evidence,'production',[
    D.V.verify_run(evidence/name) for name in read(evidence/'production.json')['runs']])
def fixtures(evidence):
    D.fixtures(evidence)
    output=evidence/'direct-fixtures'/G.stamp()
    G.run(evidence,'direct-semantic-fixtures',['python3','-B','scripts/direct-map-fixtures.py',
        '--evidence',str(evidence),'--output',str(output)])
    receipt(evidence,'fixtures',{'path':output.relative_to(evidence).as_posix(),'sha256':E.sha(output/'results.json')})

def verify_host(evidence):
    evidence=host_base(evidence)
    D.T.verify_host(evidence);D.P.verify_host(evidence);D.verify_host(evidence)
    receipt_data=read(evidence/'direct-host.json');directory=evidence/receipt_data['result']['path']
    E.require(receipt_data['build_input_sha256']==E.identity(E.manifest()) and
        E.sha(directory/'direct-host.json')==receipt_data['result']['sha256'],'RAM hosted input binding')
    return module('direct-map-host').verify(directory)

def reusable(evidence,mode,*,receipt_override=None):
    selected=host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
    def semantic(row):
        if mode=='runtime':S.runtime_predecessor(evidence,row)
        E.require(S.stage(D,selected,'device',mode)['state']=='REUSABLE',
            'RAM inherited stage is not reusable: '+mode)
        if mode=='host':return verify_host(evidence)
        if mode=='static':
            import direct_current
            expected=direct_current.boundary_semantics();expected['scope']=direct_current.scope('verified-ram-direct-map')
            E.require(row['result']==expected and read(S.static_base(evidence)/'direct-test-plan.json')==plan(),'RAM static predicate differs')
        elif mode=='build':
            for role in ('selftest','debug','production','framebuffer-reference'):
                path=evidence/'candidates'/role
                E.require(row['result'][role]=={'candidate_id':G.verify_local_candidate(path),
                    'symbols_sha256':E.sha(path/'symbols.txt')},'RAM build role identity differs')
        elif mode=='runtime':
            D.verify_runtime_recorded(evidence,row['result']['inherited'])
            E.require(row['result']['owners']==owner_proof(evidence),'RAM owner stage result differs')
            E.require(row['result']['paging']==paging_measurements(evidence),'RAM paging timing result differs')
        elif mode=='production':
            E.require(row['result']==[D.V.verify_run(evidence/name) for name in read(evidence/'production.json')['runs']],
                'RAM production result differs')
        elif mode=='fixtures':
            path=evidence/row['result']['path']/'results.json'
            E.require(E.sha(path)==row['result']['sha256'],'RAM fixture bytes differ')
            module('direct-map-fixtures').verify_receipt(read(path))
        return {'status':'PASS'}
    path=S.runtime_receipt(evidence,'direct') if mode=='runtime' else selected/('direct-'+mode+'.json')
    return R.classify(receipt_override or path,semantic,
        source=E.identity(E.manifest()),harness=None if mode in ('build','production') else E.identity(E.manifest(harness=True)))

def verify(evidence,transport=None):
    E.require(read(S.static_base(evidence)/'direct-test-plan.json')==plan(),'RAM frozen plan differs')
    inherited=D.verify(evidence,transport);hosted=verify_host(evidence)
    for mode in ('static','host','build','runtime','production','fixtures'):
        selected=host_base(evidence) if mode=='host' else S.static_base(evidence) if mode=='static' else evidence
        row=read(S.runtime_receipt(evidence,'direct') if mode=='runtime' else selected/('direct-'+mode+'.json'))
        if mode=='runtime':S.runtime_predecessor(evidence,row)
        E.require(row['status']=='PASS' and row['build_input_sha256']==E.identity(E.manifest()),'RAM stale/missing mode '+mode)
    fixture=read(evidence/'direct-fixtures.json');path=evidence/fixture['result']['path']/'results.json'
    E.require(fixture['harness_sha256']==E.identity(E.manifest(harness=True)) and E.sha(path)==fixture['result']['sha256'],
        'RAM negative fixture binding')
    module('direct-map-fixtures').verify_receipt(read(path))
    owners=owner_proof(evidence)
    return {'status':'PASS','inherited':inherited,'host':hosted,'owners':owners,'paging':paging_measurements(evidence),
        'independent_review':'PENDING','hardware':'DEFERRED_TO_FINAL_CLOSURE'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    p.add_argument('--evidence',required=True,type=Path);p.add_argument('--scratch-parent',type=Path)
    p.add_argument('--observation-baseline',type=Path);a=p.parse_args();os.chdir(ROOT)
    evidence=a.evidence.resolve();evidence.relative_to(ROOT/'backups');evidence.mkdir(parents=True,exist_ok=True)
    if a.scratch_parent:R.configure_scratch(a.scratch_parent)
    if a.observation_baseline:D.adopt_observation(evidence,a.observation_baseline.resolve())
    if a.mode=='all':
        D.P.storage_admission(evidence,D)
        for name in ('static','host','build','runtime','production'):
            if not R.require_successor(reusable(evidence,name)):
                if name in ('build','runtime','production'):R.before_step(evidence,'runtime' if name=='production' else name)
                globals()[name](evidence)
        if not R.require_successor(S.ledger(D,evidence)):
            G.run(evidence,'current-native-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(evidence)])
        if not R.require_successor(reusable(evidence,'fixtures')):R.before_step(evidence,'fixtures');fixtures(evidence)
        T.finalize(evidence)
        if not (evidence/'physical-acceptance.json').exists():D.P.receipt(evidence,'acceptance',D.P.verify(evidence))
        if not (evidence/'device-acceptance.json').exists():D.receipt(evidence,'acceptance',D.verify(evidence))
        result=verify(evidence);receipt(evidence,'acceptance',result)
    elif a.mode=='verify':print(json.dumps(verify(evidence),sort_keys=True))
    else:
        if a.mode in ('build','runtime','production','fixtures'):R.before_step(evidence,'runtime' if a.mode=='production' else a.mode)
        globals()[a.mode](evidence)
    print(json.dumps({'mode':a.mode,'status':'PASS','evidence':str(evidence)}))
if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
