#!/usr/bin/env python3
"""Complete owned-device gate using the retained native campaign and transport."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import memory_evidence as E
import device_current as D
import validation_resources as R
import validation_stages as S
import validation_candidates as candidates

ROOT=Path(__file__).resolve().parent.parent
os.environ.setdefault('HOBBYOS_VALIDATION_CONTRACT','owned-device-mapping')
os.environ['PYTHONDONTWRITEBYTECODE']='1'
def module(name):
    spec=importlib.util.spec_from_file_location('device_gate_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
P=module('physical-translation-gate');T=P.T;G=T.G;V=module('verify-device-mapping');O=T.O
T.COMMANDS=V.COMMANDS
G.SURFACE+=('cmd_mappingtest','mapping_probe','mapping_fixture','mmio_framebuffer_reference',
            'cpu_request_audit_','usb_host_control_events')

def receipt(evidence,name,result):T.receipt(evidence,'device-'+name,result)
def read(path):return json.loads(path.read_text())

def reusable_attempts(origin):
    """Static commands are recollected for this checkout, never imported twice."""
    result=[]
    for path in sorted((origin/'attempts').iterdir()):
        E.require(path.is_dir() and not path.is_symlink(),'reuse attempt is not a directory')
        if path.name.split('Z-',1)[-1] not in ('diff-check','shell-syntax','inventory'):
            result.append(path.relative_to(origin).as_posix())
    return result

def adopt_observation(evidence, origin):
    """Import only the authenticated comparison capture, never its campaign."""
    import shutil
    record=read(origin/'previous-observation.json');source=origin/'previous-observation'
    E.require(record['copy']=='previous-observation' and record['candidate_id']==
        'ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c',
        'wrong historical observation identity')
    rows=record['files'];names=[r['path'] for r in rows]
    E.require(len(names)==len(set(names)) and len(rows)<=256 and
        all(type(r['size']) is int and r['size']>=0 for r in rows) and
        sum(r['size'] for r in rows)<=128*R.MIB,'historical observation closure bounds')
    E.require(set(names)=={p.relative_to(source).as_posix() for p in source.rglob('*') if p.is_file()},
        'historical observation member inventory differs')
    for row in rows:
        E.safe(row['path']);path=source/row['path']
        E.require(path.resolve()==path and path.is_file() and path.stat().st_size==row['size'] and
            E.sha(path)==row['sha256'],'historical observation bytes differ')
    E.require(E.sha(source/'candidate.json')==record['candidate_id'],'historical observation candidate differs')
    target=evidence/'previous-observation'
    E.require(not target.exists() and not (evidence/'previous-observation.json').exists(),
        'historical observation destination exists')
    R.before_step(evidence,'fixtures')
    pending=evidence/('observation-import-'+G.stamp());pending.mkdir()
    for row in rows:
        path=source/row['path'];copy=pending/row['path'];copy.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(path,copy)
        E.require(copy.stat().st_size==row['size'] and E.sha(copy)==row['sha256'] and
            copy.stat().st_mode&0o777==path.stat().st_mode&0o777,'historical observation copy changed')
    pending.rename(target)
    shutil.copy2(origin/'previous-observation.json',evidence/'previous-observation.json')
    baseline=evidence.parent.parent/'before/observation-source'
    G.run(evidence,'observation-baseline-replay',['python3','-B',str(baseline/'scripts/verify-memory-observation.py'),
        '--logfile',str(target/'qemu-serial.log')])
    E.write(evidence/'observation-adoption.json',{'schema':1,'status':'PASS','origin':str(origin),
        'receipt_sha256':E.sha(evidence/'previous-observation.json'),'candidate_id':record['candidate_id'],
        'files':len(rows),'bytes':sum(r['size'] for r in rows),'authority':'Historical observation only; never a current boot or normalized-policy success.'})

def reuse(evidence, origin):
    """Reuse exact validated inputs; never import incomplete ordinary/hub series."""
    import shutil
    E.require(not (evidence/'reuse.json').exists(),'reuse destination already initialized')
    E.require(S.stage(sys.modules[__name__],origin,'device','build')['state']=='REUSABLE','reuse build not verified')
    E.require(S.stage(sys.modules[__name__],origin,'device','production')['state']=='REUSABLE','reuse production not verified')
    P.verify_host(origin);T.verify_host(origin);verify_host(origin)
    if (origin/'previous-observation.json').is_file():adopt_observation(evidence,origin)
    for role in ('selftest','debug','production','framebuffer-reference'):
        candidates.adopt(origin/'candidates'/role,evidence/'candidates'/role,G.verify_local_candidate)
    source=E.identity(E.manifest());harness=E.identity(E.manifest(harness=True));copied=[]
    def copy(path):
        src=origin/path;dst=evidence/path
        E.require(src.exists() and not dst.exists(),'reuse source/destination: '+path)
        if src.is_dir():shutil.copytree(src,dst,copy_function=shutil.copy2)
        else:dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        paths=src.rglob('*') if src.is_dir() else (src,)
        for item in paths:
            E.require(not item.is_symlink(),'reuse symlink')
            if item.is_file():
                target=evidence/item.relative_to(origin)
                E.require(E.sha(item)==E.sha(target) and item.stat().st_mode&0o777==target.stat().st_mode&0o777,'reuse copy changed')
                copied.append({'path':target.relative_to(evidence).as_posix(),'sha256':E.sha(target),'size':target.stat().st_size})
    for name in (*reusable_attempts(origin),'tlb-host','control-host','physical-host','device-host','framebuffer-reference',
                 'production','expected-failures'):
        copy(name)
    for name in ('build.json','tlb-build.json','physical-build.json','device-build.json',
                 'control-host.json','tlb-host.json','physical-host.json','device-host.json',
                 'production.json','tlb-production.json','physical-production.json','device-production.json',
                 'tlb-failure-experiments.json'):
        copy(name)
    original=read(origin/'candidates/selftest/candidate.json')['harness_inputs']
    tested={r['path']:r['sha256'] for r in original['files'] if
            ('host' in Path(r['path']).name and Path(r['path']).suffix in ('.c','.h')) or
            Path(r['path']).name in ('device-mapping-host.py','device-ownership-host.py')}
    E.require(all(E.sha(ROOT/name)==digest for name,digest in tested.items()),'tested host producer changed')
    host_row={'schema':1,'status':'PASS','build_input_sha256':source,'harness_sha256':harness,
        'derivation':'source-bound-host-replay','tested_sources':tested,
        'original_receipts':{name:E.sha(evidence/name) for name in
            ('device-host.json','physical-host.json','tlb-host.json','control-host.json')}}
    E.write(evidence/'device-host-reuse.json',host_row)
    E.require(S.stage(sys.modules[__name__],evidence,'device','host')['state']=='REUSABLE','host reuse semantic replay failed')
    S.series(G,evidence/'framebuffer-reference','framebuffer-reference',evidence/'candidates/framebuffer-reference',lambda p:V.verify_run(p,True))
    E.write(evidence/'reuse.json',{'schema':1,'status':'PASS','origin':str(origin),'files':copied,
        'build_input_sha256':source,'harness_sha256':harness,'ordinary_or_hub_boots_imported':0,
        'binary_policy':'Canonical instances referenced; no full frozen binary copied.'})
def workload_capture_plan():
    return {'schema':1,'build_input_sha256':E.identity(E.manifest()),'commands':V.COMMANDS,
        'reference_commands':V.REFERENCE_COMMANDS,'normal':O.TLB_MATRIX,
        'supplements':{'nx-disabled':3,'xapic':3,'hub-keyboard-tcg':3,'hub-keyboard-kvm':3},
        'framebuffer_reference':{'q35-tcg-smp4':3,'q35-kvm-smp4':3},'mapping_invocations':2,
        'unmap_cycles_per_invocation':8,'backing_pages':[1,2,3,4,1,2,3,4],
        'alignment_bytes':[64<<n for n in range(8)],'scratch_dma_maximum':0xffffffff,
        'framebuffer_copy':{'warmup':3,'samples':8,'scope':'swap_buffers_plus_sfence'},
        'window_base':0xffffd00000000000,'window_bytes':0x40000000,'retained_tables':514,
        'service_chronology':'Complete actual boot-wide audit; independent pre-admission checkpoints; schema 6'}

def plan():
    value=workload_capture_plan();value['schema']=2
    value['service_chronology']='Complete boot-wide audit through both mapping workloads; independent pre-admission checkpoints; schema 6'
    return value

def static(evidence):
    E.write(evidence/'device-test-plan.json',plan())
    P.static(evidence)
    result=D.scope('owned-device-mapping');result['guard']=D.source_guard();result['instructions']=D.audit_sites()
    result['worker']=module('device-mapping-order').fixtures((ROOT/'kernel/src/core/kernel_init.c').read_text())
    receipt(evidence,'static',result)

def host(evidence):
    if not (evidence/'physical-host.json').exists():P.host(evidence)
    directory=evidence/'device-host'/G.stamp()
    mapping=module('device-mapping-host').run(directory/'mapping')
    ownership=module('device-ownership-host').run(directory/'ownership')
    receipt(evidence,'host',{'path':directory.relative_to(evidence).as_posix(),
        'mapping':E.sha(directory/'mapping/device-host.json'),'ownership':E.sha(directory/'ownership/ownership-host.json'),
        'tests':len(mapping['subjects'])+len(ownership['results'])})

def build(evidence):
    P.build(evidence)
    facts={}
    for name in ('production','debug','selftest'):
        directory=evidence/'candidates'/name;symbols=(directory/'symbols.txt').read_text()
        strings=subprocess.check_output(['strings',str(directory/'kernel.elf')],text=True)
        if name!='selftest':V.production(symbols,strings)
        else:E.require('mapping_probe_fault' in symbols and 'cmd_mappingtest' in symbols,'mapping guest workload missing')
        facts[name]={'candidate_id':G.verify_local_candidate(directory),'symbols_sha256':E.sha(directory/'symbols.txt')}
    flags=['DEBUG_ASSERT=1','SELFTEST=1','SELFTEST_AUTORUN=0','FRAMEBUFFER_REFERENCE=1']
    import irq_latency_current as observer
    flags+=observer.build_flags('framebuffer-reference')
    inputs=E.identity(E.manifest());attempts=[G.run(evidence,'framebuffer-reference-clean',['make','clean'])]
    for target in ('kernel-check','image'):
        attempts.append(G.run(evidence,'framebuffer-reference-'+target,['make',target,'JOBS=4',*flags]))
    directory=evidence/'candidates/framebuffer-reference';G.save_candidate(directory,flags,inputs,attempts)
    G.code_generation(directory);T.build_facts(directory,False)
    G.run(evidence,'framebuffer-reference-stack-check',['make','stack-check','JOBS=4',*flags])
    E.require('mmio_framebuffer_reference' in (directory/'symbols.txt').read_text(),'paired reference selector missing')
    facts['framebuffer-reference']={'candidate_id':G.verify_local_candidate(directory),'purpose':'Controlled WB copy comparison only; never WC/production acceptance'}
    receipt(evidence,'build',facts)

def runtime(evidence):
    P.runtime(evidence,validation=V.verify_complete_run)
    hub=evidence/'hub-keyboard';hub.mkdir(exist_ok=True)
    G.runtime(hub,scope='hub-keyboard',commands=V.COMMANDS,candidate=evidence/'candidates/selftest',validation=V.verify_complete_run,serial_backend='file')
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        for scope in ('high-ram','high-ram-fallback'):
            path=evidence/scope;path.mkdir(exist_ok=True)
            G.runtime(path,scope=scope,commands=V.COMMANDS,candidate=evidence/'candidates/selftest',validation=V.verify_complete_run,serial_backend='file')
    reference=evidence/'framebuffer-reference';reference.mkdir(exist_ok=True)
    G.runtime(reference,scope='framebuffer-reference',commands=V.REFERENCE_COMMANDS,
        candidate=evidence/'candidates/framebuffer-reference',validation=lambda path:V.verify_run(path,True),serial_backend='file')
    receipt(evidence,'runtime',verify_runtime(evidence))

def production(evidence):
    candidate=evidence/'candidates/production';G.verify_local_candidate(candidate);runs=[]
    (evidence/'production-rejections').mkdir(exist_ok=True)
    for accel,smp in [('tcg',1),('kvm',4)]:
        name=f'production/q35-{accel}-smp{smp}';directory=evidence/name
        selected=None
        for previous in sorted((evidence/'production').glob(directory.name+'*')):
            if not previous.is_dir():continue
            try:V.verify_run(previous)
            except (ValueError,KeyError,OSError,AssertionError) as error:
                rejection=evidence/'production-rejections'/(previous.name+'.json')
                if not rejection.exists():
                    E.write(rejection,{'path':previous.relative_to(evidence).as_posix(),'status':'REJECTED',
                        'result_sha256':E.sha(previous/'result.json') if (previous/'result.json').exists() else None,
                        'reason':str(error),'verifier_sha256':E.identity(E.manifest(harness=True))})
            else:selected=previous
        if selected is not None:
            runs.append(selected.relative_to(evidence).as_posix());continue
        if directory.exists():
            name+='-'+G.stamp();directory=evidence/name
        argv=['python3','-B','scripts/memory-qemu.py','--evidence',str(directory),'--candidate',str(candidate),
              '--machine','q35','--accel',accel,'--smp',str(smp),'--serial-backend','file','--production']
        contract=os.environ.get('HOBBYOS_VALIDATION_CONTRACT')
        rejected=(['rammap','rammap high'] if contract in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else [])
        rejected+=(['lowmap','lowmap crash'] if contract in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else [])
        rejected+=(['wxtest'] if contract in ('kernel-image-protection','guarded-kernel-stacks') else [])
        rejected+=(['stacktest','stacktest overflow','stacktest timeout'] if contract=='guarded-kernel-stacks' else [])
        for command in rejected+['ptinspect','cpustate','memmap','tlbtest','phystest','mappingtest','mem','cpu','irq check']:
            argv+=['--command',command]
        G.run(evidence,'production-'+directory.name,argv)
        V.verify_run(directory);runs.append(name)
    E.write(evidence/'production.json',{'status':'PASS','candidate_id':G.verify_local_candidate(candidate),'runs':runs})
    T.receipt(evidence,'tlb-production',[T.V.verify_run(evidence/name) for name in runs])
    P.receipt(evidence,'production',[P.V.verify_run(evidence/name) for name in runs])
    receipt(evidence,'production',[V.verify_run(evidence/name) for name in runs])

def verify_runtime(evidence):
    selected={};identities=set();benchmarks={};reference_identity=None;ordinary_identity=None
    for scope,directory in [('tlb-matrix',evidence),('nx-disabled',evidence/'nx-disabled'),
        ('xapic',evidence/'xapic'),('hub-keyboard',evidence/'hub-keyboard'),('framebuffer-reference',evidence/'framebuffer-reference')]+(
        [(s,evidence/s) for s in ('high-ram','high-ram-fallback')] if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else []):
        series=O.verify_series(directory);E.require(series['scope']==scope,'mapping series scope mismatch')
        manifest=read(directory/'series.json');runs=[];reference=scope=='framebuffer-reference'
        for name in manifest['runs']+manifest['diagnostics']:
            path=directory/name;result=V.verify_complete_run(path,reference);raw=read(path/'result.json')
            pair=(raw['vm_id'],raw['boot_id']);E.require(pair not in identities,'same boot reused for mapping acceptance');identities.add(pair)
            if reference:
                if reference_identity is None:reference_identity=raw['candidate_id']
                E.require(raw['candidate_id']==reference_identity,'reference candidate changed')
            else:
                if ordinary_identity is None:ordinary_identity=raw['candidate_id']
                E.require(raw['candidate_id']==ordinary_identity,'ordinary matrix candidate changed')
            if scope in ('tlb-matrix','framebuffer-reference') and raw['profile'] in ('q35-tcg-smp4','q35-kvm-smp4') and not raw['readback']:
                benchmarks.setdefault((raw['profile'],reference),[]).append(result['benchmark'])
            runs.append({'path':path.relative_to(evidence).as_posix(),'result':result})
        selected[scope]={'series':series,'runs':runs}
    E.require(ordinary_identity!=reference_identity,'reference and WC candidate identity collapsed')
    comparison=[]
    for profile in ('q35-tcg-smp4','q35-kvm-smp4'):
        wb=benchmarks[(profile,True)];wc=benchmarks[(profile,False)]
        E.require(len(wb)==len(wc)==3,'paired independent benchmark boots')
        E.require(all(r['geometry']==wb[0]['geometry'] for r in wb+wc),'paired copy geometry/checksum differs')
        comparison.append({'profile':profile,'wb':wb,'wc':wc})
    return {'series':selected,'copy_comparison':comparison,'ordinary_candidate':ordinary_identity,'reference_candidate':reference_identity}

def verify_host(evidence):
    evidence=S.host_base(evidence)
    row=read(evidence/'device-host.json');E.require(row['build_input_sha256']==E.identity(E.manifest()),'device hosted source changed')
    directory=evidence/row['result']['path'];observed=[]
    for name,filename,key,execute_key in [('mapping','device-host.json','subjects','execute'),('ownership','ownership-host.json','results','execution')]:
        path=directory/name;data=read(path/filename)
        E.require(E.sha(path/filename)==row['result'][name],'device host receipt changed')
        if name=='ownership':
            E.require(data.get('summary_cases')==module('device-ownership-host').summary_cases(),
                      'USB hosted contention summary controls missing or altered')
        expected_subjects={'mapping','dma'} if name=='mapping' else {'usb-control','usb-ownership','usb-hotplug','hub-buffer','hub-ownership','probe','origin'}
        seen=set()
        for test in data[key]:
            pair=(test['subject'],test['mode']);E.require(pair not in seen,'duplicate device C host test');seen.add(pair)
            compiled,log=T.C.verify_command(path,test['build']);ran,output=T.C.verify_command(path,test[execute_key])
            E.require(compiled['argv'][0]=='gcc' and ran['argv']==[compiled['argv'][-1]] and
                compiled['build_input_sha256']==ran['build_input_sha256']==row['build_input_sha256'] and
                'PASS' in output and output.strip()==test['summary'].strip(),'actual device C compile/execution mismatch')
            if name=='ownership':module('device-ownership-host').verify_summary(test['subject'],output)
            else:E.require(output==module('device-mapping-host').expected_summary(test['subject']),
                'device actual C independent case inventory differs')
            sanitizer=test['mode']
            E.require(('-fsanitize=address' in compiled['argv'])==(sanitizer=='asan') and
                ('-fsanitize=undefined' in compiled['argv'])==(sanitizer=='ubsan'),'device C sanitizer role')
        E.require(seen=={(s,m) for s in expected_subjects for m in ('normal','ubsan','asan')},'missing device C subject/mode')
        observed.append({'suite':name,'executions':len(seen)})
    return observed

def reconcile_runtime(recorded, observed, transport=None):
    """Normalize verifier provenance only after replaying every captured fact."""
    E.require(set(recorded['series'])==set(observed['series']), 'device runtime series omitted')
    for scope,group in observed['series'].items():
        prior=recorded['series'][scope]
        E.require(len(prior['runs'])==len(group['runs']),'device runtime receipt omitted run')
        for old,new in zip(prior['runs'],group['runs']):
            a,b=old['result']['inherited'],new['result']['inherited']
            if scope!='framebuffer-reference':a,b=a['invalidation']['policy'],b['invalidation']['policy']
            T.C.verify_mode_run(a,b,transport)
        group['series']['replay_verifier_sha256']=prior['series']['replay_verifier_sha256']
    E.require(observed==recorded,'device semantic runtime receipt differs')
    return observed

def verify_runtime_recorded(evidence, recorded, transport=None):
    return reconcile_runtime(recorded,verify_runtime(evidence),transport)

def fixtures(evidence):
    P.fixtures(evidence)
    directory=evidence/'device-fixtures'/G.stamp()
    G.run(evidence,'device-semantic-fixtures',['python3','-B','scripts/device-mapping-fixtures.py','--evidence',str(evidence),'--output',str(directory)])
    base=next(name for name in read(evidence/'series.json')['runs'] if name.startswith('profiles/q35-kvm-smp4-'))
    G.run(evidence,'validation-resumption-fixtures',['python3','-B','scripts/validation-resumption-fixtures.py',
        '--output',str(directory/'resumption.json'),'--base',str(evidence/base)])
    G.run(evidence,'validation-retention-fixtures',['python3','-B','scripts/validation-retention-fixtures.py',
        '--output',str(directory/'retention.json')])
    receipt(evidence,'fixtures',{'path':directory.relative_to(evidence).as_posix(),'sha256':E.sha(directory/'results.json'),
        'resumption_sha256':E.sha(directory/'resumption.json'),'resumption_base':base,
        'retention_sha256':E.sha(directory/'retention.json')})

def verify(evidence,transport=None):
    E.require(read(S.static_base(evidence)/'device-test-plan.json')==plan(),'frozen mapping test geometry differs')
    D.contract();D.boundary_semantics();D.audit_sites()
    inherited=P.verify(evidence,transport)
    modes={name:read((S.static_base(evidence) if name=='static' else S.host_base(evidence) if name=='host' else evidence)/('device-'+name+'.json')) for name in ('static','host','build','runtime','production','fixtures')}
    for name,r in modes.items():
        E.require(r['status']=='PASS' and r['build_input_sha256']==E.identity(E.manifest()),'missing/stale device mode: '+name)
    result=verify_runtime_recorded(evidence,modes['runtime']['result'],transport);host_result=verify_host(evidence)
    # Inherited native replay validates harness revisions; raw fields and paths
    # are replayed afresh here, not trusted because a receipt says PASS.
    smokes=[V.verify_run(evidence/name) for name in read(evidence/'production.json')['runs']]
    E.require(len(smokes)==len(modes['production']['result'])==2,'device production replay coverage')
    for old,new in zip(modes['production']['result'],smokes):
        T.C.verify_mode_run(old['inherited']['invalidation']['policy'],new['inherited']['invalidation']['policy'],transport)
    E.require(smokes==modes['production']['result'],'device production replay differs')
    fixture=modes['fixtures'];path=evidence/fixture['result']['path']/'results.json'
    E.require(fixture['harness_sha256']==E.identity(E.manifest(harness=True)) and E.sha(path)==fixture['result']['sha256'],
              'device negative verifier/input binding')
    module('device-mapping-fixtures').verify_receipt(read(path))
    E.require(E.sha(path.parent/'resumption.json')==fixture['result']['resumption_sha256'],
              'resumption negative receipt changed')
    module('validation-resumption-fixtures').verify_receipt(read(path.parent/'resumption.json'))
    E.require(E.sha(path.parent/'retention.json')==fixture['result']['retention_sha256'],'retention fixture binding')
    module('validation-retention-fixtures').verify_receipt(read(path.parent/'retention.json'))
    return {'status':'PASS','inherited':inherited,'runtime':result,'host':host_result,'production':smokes,
            'hardware':'DEFERRED_TO_FINAL_CLOSURE','independent_review':'PENDING'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--reuse-campaign',type=Path);p.add_argument('--scratch-parent',type=Path)
    p.add_argument('--observation-baseline',type=Path)
    a=p.parse_args();os.chdir(ROOT)
    evidence=a.evidence.resolve();evidence.relative_to(ROOT/'backups');evidence.mkdir(parents=True,exist_ok=True)
    if a.scratch_parent:R.configure_scratch(a.scratch_parent)
    if a.observation_baseline:
        origin=a.observation_baseline.resolve();origin.relative_to(ROOT/'backups')
        adopt_observation(evidence,origin)
    if a.reuse_campaign and not (evidence/'reuse.json').exists():
        R.before_step(evidence,'build');reuse(evidence,a.reuse_campaign.resolve())
    if a.mode=='all':
        S.revalidate_host_tools(sys.modules[__name__],evidence,'device')
        P.storage_admission(evidence,sys.modules[__name__])
        if (evidence/'device-acceptance.json').exists():
            result=verify(evidence)
            E.require(read(evidence/'device-acceptance.json')['result']==result,
                      'preserved acceptance differs; use a successor destination')
            print(json.dumps(result,sort_keys=True));return
        for name in ('static','host','build','runtime','production'):
            if not R.require_successor(S.stage(sys.modules[__name__],evidence,'device',name)):
                if name in ('build','runtime'):R.before_step(evidence,name)
                globals()[name](evidence)
        if not R.require_successor(S.ledger(sys.modules[__name__],evidence)):
            G.run(evidence,'current-inherited-regression',['python3','-B','scripts/memory-current-regression.py','all','--evidence',str(evidence)])
        if not R.require_successor(S.stage(sys.modules[__name__],evidence,'device','fixtures')):
            R.before_step(evidence,'fixtures');fixtures(evidence)
        T.finalize(evidence)
        if not (evidence/'physical-acceptance.json').exists():P.receipt(evidence,'acceptance',P.verify(evidence))
        result=verify(evidence)
        receipt(evidence,'acceptance',result)
    elif a.mode=='verify':print(json.dumps(verify(evidence),sort_keys=True))
    else:
        if a.mode in ('build','runtime','production','fixtures'):
            R.before_step(evidence,'runtime' if a.mode=='production' else a.mode)
        globals()[a.mode](evidence)
    print(json.dumps({'mode':a.mode,'status':'PASS','evidence':str(evidence)}))
if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
