#!/usr/bin/env python3
"""Replay selected incomplete work without granting current acceptance.

The complete gate remains device-mapping-gate.py. This entry point preserves a
resource-blocked handoff and requires its missing coverage to remain explicit.
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import memory_evidence as E
import review_transport as R
import device_current as D

ROOT=Path(__file__).resolve().parent.parent
os.environ['HOBBYOS_VALIDATION_CONTRACT']='owned-device-mapping'
sys.dont_write_bytecode=True

def module(name):
    spec=importlib.util.spec_from_file_location('owned_progress_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def read(path):return json.loads(path.read_text())

def selected_proof(campaign,selection,package=None,host=False):
    E.require(selection['schema']==1 and selection['current_acceptance']=='BLOCKED' and
        selection['complete_portable_acceptance']=='NOT_AVAILABLE' and
        selection['external_blocker']=='PERSISTENT_STORAGE' and selection['missing_requirements'],
        'incomplete handoff falsely grants acceptance')
    E.safe(selection['resource_receipt']);resource=read(campaign/selection['resource_receipt'])
    E.require(resource['status']=='INSUFFICIENT' and 0<resource['free_bytes']<resource['required_free_bytes'] and
        resource['build_input_sha256']==E.identity(E.manifest()),'resource rejection missing/stale')
    G=module('device-mapping-gate');V=G.V
    D.contract();D.source_guard();D.audit_sites();D.boundary_semantics()
    captured_plan=G.workload_capture_plan();captured_plan['commands']=V.WORKLOAD_CAPTURE_COMMANDS
    E.require(read(campaign/'device-test-plan.json')==captured_plan,'incomplete capture geometry changed')
    candidates={}
    for name in ('selftest','production','debug','framebuffer-reference'):
        directory=campaign/'candidates'/name
        _,identity=module('device-mapping-partial-replay').candidate(directory,ROOT)
        candidates[name]=identity
        if package is None:G.G.verify_local_candidate(directory)
    E.require(len(set(candidates.values()))==4,'candidate roles conflated')
    host_result={'device':G.verify_host(campaign),'tlb':G.T.verify_host(campaign),
                 'translation':G.P.verify_host(campaign)}
    runs=[];boots=set();paths=set();counts={};totals={k:0 for k in ('replacements','requests','acknowledgements','actual_loads')}
    for row in selection['runs']:
        E.safe(row['path']);directory=campaign/row['path'];raw=read(directory/'result.json')
        E.require(row['path'] not in paths and (raw['vm_id'],raw['boot_id']) not in boots,'duplicate incomplete boot')
        paths.add(row['path']);boots.add((raw['vm_id'],raw['boot_id']))
        role=row['role'];E.require(role in ('ordinary','hub','reference','production','diagnostic'),'unknown incomplete run role')
        expected='framebuffer-reference' if role=='reference' else 'production' if role=='production' else 'selftest'
        E.require(raw['candidate_id']==candidates[expected] and bool(raw['production'])==(role=='production') and
            bool(raw['readback'])==(role=='diagnostic') and
            (raw.get('device_profile')=='hub-keyboard')==(role=='hub'),'incomplete role/candidate identity')
        result=V.verify_run(directory,role=='reference')
        entry={'path':row['path'],'role':role,'profile':raw['profile'],'vm_id':raw['vm_id'],'boot_id':raw['boot_id'],
               'candidate_id':raw['candidate_id'],'status':'PASS'}
        if role!='production':
            entry['benchmark']=result['benchmark']
            entry['request_audit_coverage']=result['request_audit_coverage']
        if role=='ordinary':
            E.require(not raw['trace_enabled'] and not raw['readback'],'diagnostic used for paging timing')
            entry['paging_ns']=G.O.timing(directory,(directory/'qemu-serial.log').read_bytes())['duration_ns']
        if role not in ('production','reference'):
            entry['workloads']=result['inherited']['invalidation']['workloads']
            for workload in entry['workloads']:
                for key in totals:totals[key]+=workload[key]
        if role=='production':entry['readback']=result['readback']
        runs.append(entry);key=role+'/'+raw['profile'];counts[key]=counts.get(key,0)+1
    comparison={}
    for profile in ('q35-tcg-smp4','q35-kvm-smp4'):
        current=[r for r in runs if r['profile']==profile and r['role']=='ordinary']
        reference=[r for r in runs if r['profile']==profile and r['role']=='reference']
        E.require(len(current)==len(reference)==3,'incomplete paired comparison boot count')
        E.require(all(r['benchmark']['geometry']==current[0]['benchmark']['geometry']
            for r in current+reference),'paired copy geometry changed')
        samples=[r['paging_ns'] for r in current]
        comparison[profile]={'paging_samples_ns':samples,'paging_median_ns':statistics.median(samples),
            'wc':[r['benchmark'] for r in current],'wb':[r['benchmark'] for r in reference]}
    series={}
    for name in selection['series']:
        E.safe(name);series[name]=G.O.verify_series(campaign/name)
        raw=read(campaign/name/'series.json')
        E.require({(Path(name)/n).as_posix() for n in raw['runs']+raw['diagnostics']}<=paths,
            'selected incomplete series lost a required raw boot')
    failures={}
    failure_index=read(campaign/'tlb-failure-experiments.json')
    E.require(set(failure_index)=={'context-failure','delayed-response'},'incomplete expected-failure inventory')
    for kind,name in failure_index.items():
        E.safe(name);directory=campaign/name;raw=read(directory/'result.json')
        E.require(raw['candidate_id']==candidates['selftest'] and
            (raw['vm_id'],raw['boot_id']) not in boots,'expected failure candidate/boot reused')
        boots.add((raw['vm_id'],raw['boot_id']))
        failures[kind]=G.O.verify_run(directory/'qemu-serial.log',failure_experiment=kind)
    fixture_path=campaign/selection['device_fixtures'];E.safe(selection['device_fixtures'])
    expected=read(fixture_path/'results.json');module('device-mapping-fixtures').verify_receipt(expected)
    with tempfile.TemporaryDirectory(prefix='owned-progress-tests-') as temp:
        dest=Path(temp)
        regenerated=module('device-mapping-fixtures').run(campaign,dest/'fixtures',selection['fixture_bases'])
        E.require(regenerated==expected,'incomplete handoff fixture replay changed')
        if host:
            (dest/'host').mkdir()
            with contextlib.redirect_stdout(io.StringIO()):G.host(dest/'host')
            G.verify_host(dest/'host');G.T.verify_host(dest/'host',True);G.P.verify_host(dest/'host',True)
    E.require(not (campaign/'device-acceptance.json').exists(),'incomplete handoff includes a false acceptance receipt')
    return {'current_acceptance':'BLOCKED','complete_portable_acceptance':'NOT_AVAILABLE',
        'limited_evidence_replay':'PASS','independent_review':'PENDING','host':host_result,'counts':counts,
        'totals':totals,'runs':runs,'series':series,'device_negative_cases':len(expected['cases']),
        'expected_failures':failures,'comparison':comparison,
        'transport_negative_cases':len(expected['transport']['cases']),
        'missing_requirements':selection['missing_requirements'],'resource':resource,
        'binary_limitation':R.LIMITATION,'hardware':'DEFERRED_TO_FINAL_CLOSURE'}

def replay(package,host):
    os.chdir(ROOT)
    M=module('memory-portable-replay');transport=M.Transport(package);transport.verify_source_identity()
    prior=module('physical-translation-portable-replay');prior.P=D
    os.environ['HOBBYOS_VALIDATION_CONTRACT']='owned-device-mapping'
    changed=prior.source_identity(package);prior.source_fixtures(package)
    repair=module('device-mapping-replay').repair_identity(package)
    source=read(package/'source-manifest.json')
    E.require(source['head']==D.BASE and source['head_tree']==D.TREE and
        not source['source_clean'] and not source['git_worktree_clean'],'incomplete work falsely committed')
    result=selected_proof(package/'evidence/current-campaign',read(package/'progress-selection.json'),package,host)
    result['preserved_baselines']=module('verify-physical-translation').baseline_references(
        package/'preservation/paging-baselines')
    result.update(repair_start=repair,changed_files=changed,export_tree=source['export_tree'])
    return result

def select(session,campaign,selection,review):
    P=module('package-memory-review');P.export(ROOT,review)
    S=module('memory-review-collect');facts=S.Selection(review);store=R.Store(review);pending=[];selected=set()
    prefix='evidence/current-campaign/'
    def add(path,name,purpose,consumer):
        path=Path(path);E.safe(name)
        if name in selected:return
        selected.add(name);R.regular(path)
        with path.open('rb') as stream:magic=stream.read(4)
        if R.local_binary_only(path.suffix,magic,path.stat().st_size,False):
            digest=E.sha(path);store.omitted({'path':name,'size':path.stat().st_size,'sha256':digest,
                'os_permissions':0o755 if path.stat().st_mode&0o111 else 0o644,'local_path':str(path.resolve()),
                'transport':'LOCAL_BINARY_ONLY','purpose':purpose,'facts':facts.proof(path,digest)},[consumer])
        else:store.add(path,name,purpose,[consumer])
        if path.suffix=='.json' and path.is_relative_to(campaign):pending.append(path)
    def tree(path,name,purpose,consumer):
        E.require(path.is_dir(),'required incomplete proof directory missing: '+str(path))
        for child in sorted(path.rglob('*')):
            if child.is_dir():continue
            relative=child.relative_to(path)
            if '__pycache__' in relative.parts or child.suffix in ('.pyc','.sock','.pid') or child.name=='harness.lock':continue
            add(child,name+'/'+relative.as_posix(),purpose,consumer)
    for name in ('device-static.json','device-host.json','device-build.json','device-test-plan.json',
                 'physical-host.json','tlb-host.json','control-host.json'):
        add(campaign/name,prefix+name,'current-manifests','limited-current-proof')
    for name in ('selftest','debug','production','framebuffer-reference'):
        tree(campaign/'candidates'/name,prefix+'candidates/'+name,'build-and-payload','candidate-binding')
    for row in selection['runs']:
        tree(campaign/row['path'],prefix+row['path'],'current-raw','limited-current-proof')
    for name in selection['series']:
        add(campaign/name/'series.json',prefix+name+'/series.json','current-manifests','independent-boot-series')
        for path in sorted((campaign/name/'campaign-attempts').glob('*.json')):
            add(path,prefix+path.relative_to(campaign).as_posix(),'chronology','independent-boot-series')
            if not path.name.endswith('.result.json'):
                started=read(path);finished=read(path.with_suffix('.result.json'))
                if finished['status']!='PASS':tree(campaign/name/started['path'],prefix+name+'/'+started['path'],'causal-current-attempt','failed-series-attempt')
    for name in selection['causal_directories']:
        E.safe(name);tree(campaign/name,prefix+name,'causal-current-attempt','failed-attempt-boundary')
    add(campaign/'tlb-failure-experiments.json',prefix+'tlb-failure-experiments.json',
        'current-manifests','expected-failure-identity')
    for name in read(campaign/'tlb-failure-experiments.json').values():
        E.safe(name);tree(campaign/name,prefix+name,'expected-failure','actual-timeout-context-boundary')
    add(campaign/selection['resource_receipt'],prefix+selection['resource_receipt'],'external-limit','resource-admission')
    tree(campaign/selection['device_fixtures'],prefix+selection['device_fixtures'],'current-negatives','actual-negative-replay')
    for name in selection.get('additional_receipts',[]):
        E.safe(name);add(campaign/name,prefix+name,'current-manifests','limited-current-proof')
    tree(session/'before/approved-source','baseline-source','prior-source','source-tree')
    add(session/'before/approved-source-manifest.json','preservation/source-manifest.json','source-identity','source-tree')
    tree(session/'before/source','preservation/repair-source','repair-start-source','source-tree')
    add(session/'before/source-manifest.json','preservation/repair-source-manifest.json','source-identity','source-tree')
    tree(session/'before/observation-source','observation-source','prior-source','historical-source-tree')
    add(session/'before/observation-source-retention.json','preservation/observation-source-retention.json','source-identity','historical-source-tree')
    tree(session/'before/paging-baselines','preservation/paging-baselines','timing-reference','preserved-measurements')
    controls=session/'before'/selection['hub_control_index']
    add(controls,'preservation/hub-controls.json','causal-index','historical-hub-failure')
    for row in read(controls)['files']:
        path=Path(row['local_path']);E.require(E.sha(path)==row['sha256'] and path.stat().st_size==row['size'],'old hub control changed')
        add(path,'preservation/hub-controls/'+row['path'],'causal-hub-control','historical-hub-failure')
    for name in selection['preservation_receipts']:
        E.safe(name);add(session/name,'preservation/'+Path(name).name,'local-retention','scope-and-chronology')
    scanned=set();review_module=module('physical-translation-review')
    def strings(value):
        if isinstance(value,str):yield value
        elif isinstance(value,list):
            for v in value:yield from strings(v)
        elif isinstance(value,dict):
            for v in value.values():yield from strings(v)
    while pending:
        path=pending.pop()
        if path in scanned:continue
        scanned.add(path)
        for value in strings(read(path)):
            possible=review_module.reference_path(value)
            if possible is None:continue
            targets=[possible] if possible.is_absolute() else [campaign/possible,path.parent/possible]
            for target in targets:
                try:relative=target.resolve().relative_to(campaign).as_posix()
                except (ValueError,OSError):continue
                if not target.exists() or relative=='.':continue
                if target.is_dir():
                    if len(Path(relative).parts)<2:continue
                    tree(target,prefix+relative,'referenced-current-proof','receipt:'+path.relative_to(campaign).as_posix())
                else:add(target,prefix+relative,'referenced-current-proof','receipt:'+path.relative_to(campaign).as_posix())
    for path in sorted((review/'binary-facts').glob('*')):
        store.add(path,path.relative_to(review).as_posix(),'bounded-binary-facts',['candidate-codegen']);path.unlink()
    (review/'binary-facts').rmdir()
    index=store.finish([{'path':str(ROOT/'backups'),'reason':'Original archives, full binaries and unselected retired campaigns remain local; no wholesale historical selection.'}])
    E.write(review/'progress-selection.json',selection);E.write(review/'selection-audit.json',R.selection_audit(index))
    E.write(review/'local-candidate-index.json',{'candidates':[{'role':n,'path':str(campaign/'candidates'/n),
        'candidate_id':E.identity(read(campaign/'candidates'/n/'candidate.json'))} for n in ('selftest','debug','production','framebuffer-reference')],
        'omitted_binaries':'transport-manifest.json:local_binaries'})
    E.write(review/'delivery-state.json',{'current_acceptance':'BLOCKED','complete_portable_acceptance':'NOT_AVAILABLE',
        'independent_review':'PENDING','hardware':'DEFERRED_TO_FINAL_CLOSURE','next_increment':'NOT_STARTED'})
    shutil.copyfile(session/'evidence/final-report.md',review/'final-report.md')
    E.write(review/'replay.json',{'commands':[{'argv':['python3','-B','source/scripts/device-mapping-progress.py','replay',
        '--package','.','--host-tests'],'expected_exit':0}]})
    E.write(review/'package-size-report.json',R.size_report(review))
    return index

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='mode',required=True)
    p=sub.add_parser('replay');p.add_argument('--package',type=Path,required=True)
    p.add_argument('--host-tests',action='store_true');p.add_argument('--materialized',action='store_true')
    p.add_argument('--scratch-parent',type=Path,
        default=os.environ.get('HOBBYOS_REVIEW_SCRATCH'))
    p=sub.add_parser('select')
    for name in ('session','campaign','selection','review'):p.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    if args.mode=='select':
        selection=read(args.selection);selected_proof(args.campaign,selection)
        index=select(args.session.resolve(),args.campaign.resolve(),selection,args.review.resolve())
        print(json.dumps({'status':'INCOMPLETE_SELECTED','logical_files':len(index['raw'])}));return
    package=args.package.resolve()
    if args.materialized:print(json.dumps(replay(package,args.host_tests),sort_keys=True));return
    P=module('package-memory-review');P.validate_tree(package)
    E.require(R.selection_audit(R.index(package))==read(package/'selection-audit.json'),'incomplete selection audit changed')
    before={p.relative_to(package).as_posix():(E.sha(p),p.stat().st_mode&0o777) for p in package.rglob('*') if p.is_file()}
    with tempfile.TemporaryDirectory(prefix='owned-progress-materialize-',dir=args.scratch_parent) as temp:
        restored=Path(temp)/'proof';R.materialize(package,restored)
        argv=[sys.executable,'-B',str(restored/'source/scripts/device-mapping-progress.py'),'replay','--package',str(restored),'--materialized']
        if args.host_tests:argv.append('--host-tests')
        process=subprocess.run(argv,capture_output=True,text=True,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',
            HOBBYOS_REVIEW_SCRATCH=temp))
        E.require(process.returncode==0,'incomplete semantic replay failed:\n'+process.stdout+process.stderr)
        result=json.loads(process.stdout.splitlines()[-1])
    after={p.relative_to(package).as_posix():(E.sha(p),p.stat().st_mode&0o777) for p in package.rglob('*') if p.is_file()}
    E.require(before==after,'incomplete replay mutated immutable originals');P.validate_tree(package)
    result.update(exact_reconstruction='PASS',immutable_original_recheck='PASS');print(json.dumps(result,sort_keys=True))

if __name__=='__main__':main()
