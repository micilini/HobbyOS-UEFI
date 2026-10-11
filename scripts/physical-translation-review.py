#!/usr/bin/env python3
"""Select the current dependency closure and transport immutable bytes once."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import zipfile
import memory_evidence as E
import validation_candidates as candidates
import review_transport as R

ROOT=Path(__file__).resolve().parent.parent
os.environ['HOBBYOS_VALIDATION_CONTRACT']='physical-translation'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
def module(name):
    spec=importlib.util.spec_from_file_location('physical_review_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);sys.modules[spec.name]=result;spec.loader.exec_module(result);return result

def requirement_source(campaign):
    path=ROOT/'scripts/memory-current-requirements.json'
    E.require(E.sha(path)==json.loads((campaign/'regression-ledger.json').read_text())['inventory_sha256'],
              'selected requirement inventory differs from collected ledger')
    return path

def reference_path(value):
    if not value or '\n' in value or len(value)>2048:return None
    possible=Path(value)
    # Long hexadecimal observations are data, not filesystem paths.
    if any(len(part.encode('utf-8'))>255 for part in possible.parts):return None
    return possible

def audit_admission(preview,archive,extraction):
    E.require(preview and not archive and not extraction,'selection audit cannot create a delivery archive')

def retention_report(session):
    complete=session/'retention/report-complete.json'
    if complete.is_file():return complete
    final=session/'retention/report-final.json'
    return final if final.is_file() else session/'retention/report.json'

def local_reuse_inventory(path, campaign):
    """Local chronology does not expand copied or discarded working trees."""
    if path in (campaign/'reuse.json', campaign/'formatting-reuse.json'):
        return True
    if path.parent.parent == campaign/'regression-attempts' and path.suffix == '.json':
        selection=json.loads((campaign/'regression-selection.json').read_text())
        if path.parent.relative_to(campaign).as_posix() in selection.values():
            return False
        result=path.parent/'result.json'
        E.require(result.is_file(), 'discarded native attempt has no terminal outcome')
        row=json.loads(result.read_text());started=json.loads((path.parent/'started.json').read_text())
        E.require(row.get('status') in ('PASS','FAIL') and row.get('key') == started['requirement']['key'] and
            row.get('build_input_sha256') == started['build_input_sha256'] and
            isinstance(row.get('commands'),list) and row['commands'] and
            len(row['commands']) == len(set(row['commands'])) and
            row.get('finished_ns',0) >= started['started_ns'], 'discarded native outcome is invalid')
        for name in row['commands']:
            E.safe(name)
            E.require(name.startswith('command-') and name.endswith('.json') and '/' not in name,
                'discarded native command path is invalid')
            command=json.loads((path.parent/name).read_text())
            E.require(type(command.get('exit_status')) is int and
                command.get('build_input_sha256') == row['build_input_sha256'] and
                E.sha(path.parent/name.replace('.json','.stdout.log')) == command.get('stdout_sha256'),
                'discarded native command bytes differ')
        E.require(path.name in ('started.json','result.json',*row['commands']),
            'unclassified discarded native metadata')
        # These files are selected explicitly for chronology. Their environment
        # names a former output directory, not an additional replay dependency.
        return True
    if path.name != 'reuse.json':
        return False
    selection=json.loads((campaign/'regression-selection.json').read_text())
    name=selection.get('formatting')
    if name is None:
        return False
    E.safe(name)
    return path == campaign/name/'evidence/reuse.json'

def local_retention(session,campaign):
    """Index former candidates without selecting their campaign payloads."""
    accepted=json.loads((session/'before/accepted-candidate-index.json').read_text())
    roots=set()
    for row in accepted['all_local_binaries']:
        for parent in Path(row['local_path']).parents:
            if (parent/'candidates/selftest/candidate.json').is_file():
                roots.add(parent);break
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        previous=json.loads((session/'before/current-candidates.json').read_text())
        for row in previous['candidates']:
            E.safe(row['path']);path=ROOT/row['path']
            E.require(E.sha(path/'candidate.json')==row['candidate_id'],'approved rollback candidate metadata changed')
            roots.add(path.parent.parent)
    retired=[]
    retention=retention_report(session)
    if retention.is_file():
        for batch in json.loads(retention.read_text())['batches']:
            E.safe(batch['plan']);plan=json.loads((session/batch['plan']).read_text())
            retired.extend(ROOT/entry['path'] for entry in plan['entries'])
    rows=[]
    for root in sorted(roots):
        manifest=root/'candidates/selftest/candidate.json';candidate=json.loads(manifest.read_text())
        retirement=root/'candidate-retirement.json';acceptance=root/'tlb-acceptance.json'
        if retirement.is_file():
            receipt=json.loads(retirement.read_text());outcome=receipt['status'];reason=receipt['reason'];proof=retirement
        elif acceptance.is_file():
            receipt=json.loads(acceptance.read_text());outcome=receipt['status']
            reason='Accepted predecessor; current runtime authority belongs to the new linked inputs.';proof=acceptance
        else:
            outcome='NO_FINAL_ACCEPTANCE_RECEIPT';reason='Earlier candidate retained as local development evidence.';proof=manifest
        bulk_retired=any(path.is_relative_to(root) for path in retired)
        rows.append({'local_path':str(root),'candidate_id':E.identity(candidate),
            'build_input_sha256':candidate['source_sha256'],'outcome':outcome,'retirement_or_exclusion':reason,
            'receipt':str(proof),'receipt_sha256':E.sha(proof),'retained_locally':not bulk_retired,
            'retention':'SELECTED_BULK_RETIRED' if bulk_retired else 'LOCAL_ONLY',
            'portably_replayed':False})
    for root in sorted((session/'evidence').iterdir()):
        if root==campaign or not root.is_dir():continue
        result=root/'result.json'
        if result.is_file():
            record=json.loads(result.read_text())
            rows.append({'local_path':str(root),'candidate_id':record.get('candidate_id'),
                'outcome':record.get('status','NO_FINAL_ACCEPTANCE_RECEIPT'),
                'retirement_or_exclusion':'Development proof superseded by the complete frozen current campaign.',
                'receipt':str(result),'receipt_sha256':E.sha(result),'retained_locally':True,'portably_replayed':False})
    return {'schema':1,'status':'LOCAL_RETENTION_INDEX','campaigns':rows,
        'boundary':'Identity and retention index only; omitted full campaigns are not claimed as portable replays.'}

def sealed_static_host_helper(path, image=False):
    """Keep the bounded executable whose bytes a static successor explicitly seals."""
    path=Path(path)
    if (not image or path.name!='check' or path.parent.name!='static-vectors' or
        len(path.parents)<4 or path.parents[2].name!='current-static' or
        path.parents[3].name!='stage-revisions'):
        return False
    seal=path.parents[1]/'complete.json'
    if not seal.exists():return False
    E.require(seal.is_file() and not seal.is_symlink(),'sealed static host helper seal differs')
    row=json.loads(seal.read_text())
    matches=[r for r in row.get('files',[]) if r.get('path')=='static-vectors/check']
    E.require(row.get('schema')==1 and row.get('status')=='PASS' and len(matches)==1 and
        path.is_file() and not path.is_symlink(),'sealed static host helper differs')
    size=path.stat().st_size
    with path.open('rb') as stream:prefix=stream.read(4)
    E.require(prefix==b'\x7fELF' and 0<size==matches[0]['size']<=65536 and
        E.sha(path)==matches[0]['sha256'],'sealed static host helper differs')
    return True


def select(session,campaign,review,acceptance_verified=True,*,device=False,direct=False,low=False,image=False,stack=False):
    E.require(not stack or device and direct and low and image and
        os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks','stack transport contract hierarchy')
    P=module('package-memory-review');S=module('memory-review-collect')
    P.export(ROOT,review);store=R.Store(review);facts=S.Selection(review);selected={};pending=[]
    def add(path,name,purpose,consumers,synthetic=False):
        E.safe(name);path=Path(path);R.regular(path)
        if name in selected:
            E.require(selected[name]==path.resolve(),'logical source collision');return
        selected[name]=path.resolve()
        with path.open('rb') as stream:prefix=stream.read(4)
        size=path.stat().st_size
        sealed_host_helper = sealed_static_host_helper(path,image)
        if image and path.name == 'input-layout' and (path.parent/'interruption.json').is_file():
            seal=json.loads((path.parent/'interruption.json').read_text())
            matches=[r for r in seal['files'] if r['path']=='input-layout']
            E.require(seal['status']=='INTERRUPTED_HOST_REBOOT' and seal['classification']=='transport' and
                len(matches)==1 and prefix==b'\x7fELF' and size==matches[0]['size']<=65536 and
                E.sha(path)==matches[0]['sha256'],'interrupted required host helper differs')
            # This small host executable is sealed raw crash evidence, not a
            # guest image. Keep its exact bytes for the interruption checker.
            sealed_host_helper = True
        if R.local_binary_only(path.suffix,prefix,size,synthetic) and not sealed_host_helper:
            row={'path':name,'size':size,'sha256':E.sha(path),
                'os_permissions':0o755 if path.stat().st_mode&0o111 else 0o644,
                'purpose':purpose,'local_path':str(path.resolve()),'transport':'LOCAL_BINARY_ONLY'}
            row['facts']=facts.proof(path,row['sha256']);store.omitted(row,consumers)
        else:
            blob=('synthetic-blobs/'+E.sha(path)) if synthetic else None
            if blob and E.sha(path) in store.source:blob=store.source[E.sha(path)]['path']
            store.add(path,name,purpose,consumers,group='synthetic_deltas' if synthetic else 'raw',blob=blob)
        if path.suffix=='.json' and not synthetic:pending.append(path)
    def tree(path,name,purpose,consumers):
        E.require(path.is_dir(),'required selected dependency directory missing: '+str(path))
        for source in sorted(path.rglob('*')):
            if source.is_dir():continue
            suffix=source.relative_to(path)
            if any(p in ('.git','__pycache__') for p in suffix.parts) or source.suffix in ('.pyc','.sock','.pid') or source.name=='harness.lock':continue
            synthetic=any(p in ('fixtures','fixture','replay-fixtures','oracle-fixtures') for p in suffix.parts)
            add(source,name+'/'+suffix.as_posix(),purpose,consumers,synthetic)
    prefix='evidence/current-campaign/'
    top=(*S.TOP,'test-plan.json','tlb-static.json','tlb-host.json','control-host.json','tlb-build.json',
         'tlb-runtime.json','tlb-production.json','tlb-failure-experiments.json','tlb-fixtures.json','tlb-acceptance.json',
         'physical-static.json','physical-host.json','physical-build.json','physical-runtime.json',
         'physical-production.json','physical-fixtures.json','physical-acceptance.json','previous-observation.json')
    if device:
        top+=('device-test-plan.json',*(f'device-{n}.json' for n in ('static','host','build','runtime','production','fixtures','acceptance')))
    if direct:
        top+=('direct-test-plan.json','direct-owner-readback.json',*(f'direct-{n}.json' for n in ('static','host','build','runtime','production','fixtures','acceptance')))
    if low:
        top+=('low-test-plan.json','low-destructive.json',*(f'low-{n}.json' for n in ('static','host','build','runtime','production','fixtures','acceptance')))
    if image:
        top+=('image-test-plan.json',*(f'image-{n}.json' for n in ('static','host','build','runtime','production','fixtures','acceptance')))
    if stack:
        top+=('stack-test-plan.json','stack-experiments.json','stack-taskman-peak.json','current-collection-freeze.json',
            *(f'stack-{n}.json' for n in ('static','host','build','runtime','production','fixtures','acceptance')))
    optional={'build-reuse.json','runtime.json'}
    for name in top:
        path=campaign/name
        if name=='current-requirement-inventory.json':
            # The versioned inventory is the original collector input. Its
            # transport alias shares those exact bytes, not a new success claim.
            path=requirement_source(campaign)
        if not path.exists() and name in optional:continue
        E.require(path.is_file(),'required current receipt missing: '+name)
        add(path,prefix+name,'current-manifests',['physical-gate','native-regression'])
    if device:
        for name in ('reuse.json','device-host-reuse.json','formatting-reuse.json'):
            if (campaign/name).is_file():add(campaign/name,prefix+name,'reuse-derivation',['source-bound-reuse'])
        if (campaign/'stage-revisions').is_dir():
            tree(campaign/'stage-revisions',prefix+'stage-revisions','reuse-derivation',['source-bound-reuse'])
    candidate_names=('selftest','debug','production','framebuffer-reference') if device else ('selftest','debug','production')
    for name in candidate_names:
        tree(candidates.resolve(campaign/'candidates'/name),prefix+'candidates/'+name,'build-and-payload',['codegen','candidate-binding','native-layout'])
    series_roots=(campaign,campaign/'nx-disabled',campaign/'xapic')
    if device:series_roots+=(campaign/'hub-keyboard',campaign/'framebuffer-reference')
    if direct:series_roots+=(campaign/'high-ram',campaign/'high-ram-fallback')
    for base in series_roots:
        rel=base.relative_to(campaign).as_posix();pre=prefix+('' if rel=='.' else rel+'/')
        if base!=campaign:
            add(base/'series.json',pre+'series.json','current-manifests',['series'])
        series=json.loads((base/'series.json').read_text())
        selected_runs=set(series['runs']+series['diagnostics'])
        for name in sorted(selected_runs):tree(base/name,pre+name,'current-raw',['observation','cpu-policy','tlb-coherence','physical-boundary'])
        for path in sorted((base/'campaign-attempts').glob('*.json')):
            add(path,pre+'campaign-attempts/'+path.name,'chronology',['series'])
            if path.name.endswith('.result.json'):continue
            started=json.loads(path.read_text());finished=json.loads(path.with_suffix('.result.json').read_text())
            if finished['status']!='PASS' or started['path'] not in selected_runs:
                # Only this candidate's attempts participate in its chronology.
                tree(base/started['path'],pre+started['path'],'causal-current-attempt',['series','failure-boundary'])
    for name in json.loads((campaign/'production.json').read_text())['runs']:
        tree(campaign/name,prefix+name,'production-raw',['production-isolation','cpu-policy','physical-boundary'])
    if direct:
        name=json.loads((campaign/'direct-owner-readback.json').read_text())['path'];E.safe(name)
        tree(campaign/name,prefix+name,'current-raw',['ram-pointer-owners','independent-table-readback'])
    if low:
        for row in json.loads((campaign/'low-destructive.json').read_text()).values():
            name=row['path'];E.safe(name)
            tree(campaign/name,prefix+name,'required-failure-experiment',['NULL-installed-fault-and-panic'])
    if stack:
        for row in json.loads((campaign/'stack-experiments.json').read_text())['runs']:
            name=row['path'];E.safe(name)
            tree(campaign/name,prefix+name,'required-failure-experiment',['guarded-task-overflow','stack-ACK-quarantine'])
        for name in json.loads((campaign/'stack-taskman-peak.json').read_text())['runs']:
            E.safe(name)
            tree(campaign/name,prefix+name,'current-raw',['actual-257-task-peak','complete-terminal-service-audit'])
    for name in json.loads((campaign/'tlb-failure-experiments.json').read_text()).values():
        tree(campaign/name,prefix+name,'required-failure-experiment',['tlb-failure-boundary'])
    for key,name in json.loads((campaign/'regression-selection.json').read_text()).items():
        tree(campaign/name,prefix+name,'native-current-raw',['native:'+key,'fixed-regression-ledger'])
    # Every discarded native attempt retains its outcome and command/causal
    # output, but never pulls a retired candidate image or synthetic tree in.
    current_native=set(json.loads((campaign/'regression-selection.json').read_text()).values())
    for attempt in sorted((campaign/'regression-attempts').iterdir()):
        rel=attempt.relative_to(campaign).as_posix()
        if rel in current_native:continue
        for path in sorted(attempt.glob('*')):
            if path.is_file() and (path.suffix=='.json' or path.name.startswith('command-')):
                add(path,prefix+rel+'/'+path.name,'causal-native-attempt',['native-attempt-chronology'])
    for path in (campaign/'attempts').glob('*/command.json'):
        if path.parent.name.split('Z-',1)[-1] in ('diff-check','shell-syntax','inventory','deps-check','debug-stack','production-stack','selftest-stack'):
            rel=path.parent.relative_to(campaign).as_posix();tree(path.parent,prefix+rel,'current-checks',['static','canonical-builds'])
    tree(campaign/'previous-observation',prefix+'previous-observation','selected-baseline',['historical-observation-positive','normalized-policy-negative'])
    for name in ('source-manifest.json','preflight.json','protected-anchors.json','unstaged.diff','staged.diff',
                 'accepted-review-receipt.json','observation-source-retention.json'):
        path=session/'before'/name
        if direct and name=='source-manifest.json':path=session/'before/checkpoint/source-manifest.json'
        elif device and name=='source-manifest.json':path=session/'before/approved-source-manifest.json'
        if path.is_file():add(path,'preservation/'+name,'preservation',['scope','source-tree'])
    module('verify-physical-translation').baseline_references(session/'before/paging-baselines')
    tree(session/'before/paging-baselines','preservation/paging-baselines','preserved-timing-subsets',
         ['preserved-paging-comparison'])
    if low:
        module('low-half-gate').V.historical_copy(session/'before/copy-baseline')
        tree(session/'before/copy-baseline','preservation/copy-baseline','preserved-copy-subset',
             ['historical-copy-comparison'])
    for name in ('closure.json','storage-preservation.json','changed-file-purposes.json'):
        path=session/'evidence'/name
        if name=='changed-file-purposes.json' and (session/'evidence/changed-file-purposes-final.json').is_file():
            path=session/'evidence/changed-file-purposes-final.json'
        if path.is_file():add(path,'preservation/'+name,'local-preservation',['local-closure'])
    if direct:
        instruction_name='CODEX_GUARDED_KERNEL_STACKS.md' if stack else 'CODEX_KERNEL_IMAGE_PROTECTION.md' if image else 'CODEX_LOW_ADDRESS_ISOLATION.md' if low else 'CODEX_VERIFIED_RAM_DIRECT_MAP.md'
        instruction=session/'review'/instruction_name
        origin=session/'review/instruction-source.json'
        E.require(instruction.is_file() and origin.is_file(),'exact implementation instruction missing')
        add(instruction,'preservation/'+instruction_name,'implementation-contract',['scope-review'])
        add(origin,'preservation/instruction-source.json','implementation-contract',['scope-review'])
    causal=session/'evidence/causal-selection.json'
    if causal.is_file():
        selected_causal=json.loads(causal.read_text())
        E.require(selected_causal['schema']==1 and len(selected_causal['files'])<=64,
                  'causal proof selection bound/schema')
        add(causal,'preservation/causal-selection.json','causal-index',['native-attempt-chronology'])
        for row in selected_causal['files']:
            E.safe(row['path']);path=session/row['path']
            E.require(path.resolve().is_relative_to(session) and path.is_file() and
                      path.stat().st_size==row['size']<=2*1024*1024 and E.sha(path)==row['sha256'],
                      'selected causal proof differs or exceeds bound')
            add(path,'preservation/causal/'+row['path'],'causal-static-proof',row['consumers'])
    tree(session/('before/checkpoint/source' if direct else 'before/approved-source' if device else 'before/source'),'baseline-source','prior-source',['approved-tree','scope-delta'])
    tree(session/'before/observation-source','observation-source','prior-source',['historical-observation-positive'])
    if device:
        repair=session/'before/repair-source'
        repair_manifest=session/'before/repair-source-manifest.json'
        if not repair.exists():
            repair=session/'before/source';repair_manifest=session/'before/source-manifest.json'
        tree(repair,'preservation/repair-source','prior-source',['reviewed-uncommitted-tree','repair-delta'])
        add(repair_manifest,'preservation/repair-source-manifest.json','preservation',['reviewed-uncommitted-tree'])
        controls=session/'before/hub-controls-complete.json'
        data=json.loads(controls.read_text());add(controls,'preservation/hub-controls.json','preservation',['historical-hub-failure'])
        for row in data['files']:
            source=Path(row['local_path']);E.require(source.stat().st_size==row['size'] and E.sha(source)==row['sha256'],'original hub control changed')
            add(source,'preservation/hub-controls/'+row['path'],'causal-hub-control',['historical-hub-failure'])
        retention=retention_report(session)
        if retention.is_file():
            report=json.loads(retention.read_text())
            add(retention,'preservation/retention/report.json','retention-accounting',['local-retirement-and-kept-closure'])
            for row in report['proof_files']:
                E.safe(row['path']);source=session/row['path']
                E.require(source.resolve().is_relative_to(session) and source.stat().st_size==row['size'] and
                          E.sha(source)==row['sha256'],'retention proof changed')
                add(source,'preservation/retention/'+row['path'],'retention-accounting',['local-retirement-and-kept-closure'])
    # Resolve only explicit references within the current campaign. An absolute
    # old path in a historical receipt is a provenance label, not a tree selector.
    scanned=set()
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
        if not path.is_relative_to(campaign) or path.is_relative_to(campaign/'previous-observation'):continue
        # This is a local copy inventory, not a validator dependency list.
        # Reused build/host/production/reference consumers select their inputs
        # above; unused command output from the origin stays local.
        if local_reuse_inventory(path,campaign):continue
        for value in strings(json.loads(path.read_text())):
            possible=reference_path(value)
            if possible is None:continue
            targets=[possible] if possible.is_absolute() else [campaign/possible,path.parent/possible]
            for target in targets:
                try:rel=target.resolve().relative_to(campaign).as_posix()
                except (ValueError,OSError):continue
                if not target.exists() or rel=='.':continue
                E.safe(rel)
                # Root groups are never inferred. A validator names a concrete
                # attempt/candidate/subdirectory or file below a group.
                if target.is_dir() and len(Path(rel).parts)<2:continue
                if target.is_dir():tree(target,prefix+rel,'referenced-current-proof',['receipt:'+path.relative_to(campaign).as_posix()])
                elif prefix+rel not in selected:add(target,prefix+rel,'referenced-current-proof',['receipt:'+path.relative_to(campaign).as_posix()])
    for path in sorted((review/'binary-facts').glob('*')):
        relative=path.relative_to(review).as_posix()
        store.add(path,relative,'bounded-binary-facts',['native-layout','production-payload','codegen'])
        path.unlink()
    (review/'binary-facts').rmdir()
    local_only=[{'path':str(session/'attempts'),'reason':'Unselected development work; explicit retirement receipts distinguish retained and retired objects'},
                {'path':str(ROOT/'backups'),'reason':'Prior archives and retired campaigns are not dependencies; selected immutable baseline only'},
                {'path':str(campaign),'selection':'transport-manifest.json:local_binaries',
                 'reason':'Only the indexed full binary payloads stay local; required raw campaign proof is selected separately'}]
    index=store.finish(local_only)
    E.write(review/'selection-audit.json',R.selection_audit(index))
    E.write(review/'local-retention-index.json',local_retention(session,campaign))
    E.write(review/'local-candidate-index.json',{'schema':1,'build_input_sha256':E.identity(E.manifest()),
        'candidates':[{ 'profile':n,'path':str(candidates.resolve(campaign/'candidates'/n)),
                       'metadata_path':str(campaign/'candidates'/n),'candidate_id':E.identity(json.loads((campaign/'candidates'/n/'candidate.json').read_text()))}
                      for n in candidate_names],
        'binary_index':'transport-manifest.json:local_binaries'})
    report=session/'evidence/final-report.md'
    if report.is_file():shutil=__import__('shutil');shutil.copyfile(report,review/'final-report.md')
    E.write(review/'delivery-state.json',{'schema':1,'current_acceptance':'PASS' if acceptance_verified else 'NOT_REPLAYED',
        'portable_replay':'PENDING_FRESH_EXTRACTION' if acceptance_verified else 'NOT_EXECUTED',
        'independent_review':'PENDING','hardware':'DEFERRED_TO_FINAL_CLOSURE','human_visual_review':'PENDING','next_increment':'NOT_AUTHORIZED'})
    entry='stack-guard-replay.py' if stack else 'kernel-wx-replay.py' if image else 'low-half-replay.py' if low else 'direct-map-replay.py' if direct else 'device-mapping-replay.py' if device else 'physical-translation-portable-replay.py'
    E.write(review/'replay.json',{'commands':[{'argv':['python3','-B','source/scripts/'+entry,
        '--package','.','--host-tests'],'expected_exit':0}]})
    E.write(review/'package-size-report.json',R.size_report(review))
    return index

def archive(review,output,extraction):
    """Fixed-size stored metadata makes exact in-archive size reporting finite."""
    P=module('package-memory-review');R.validate(review)
    E.require(not output.exists() and not extraction.exists(),'new archive/extraction required')
    report=R.size_report(review)
    report.update(compressed_bytes=0,compressed_payload_bytes=0,physical_zip_overhead_bytes=0,
                  members=0,archive_logical_bytes=0)
    report_path=review/'package-size-report.json'
    def padded(path,value,size=None):
        data=E.canonical(value)
        size=size or ((len(data)+8191)//4096)*4096
        E.require(len(data)<=size,'fixed metadata reservation exhausted')
        path.write_bytes(data+b' '*(size-len(data)));os.chmod(path,0o644);return size
    report_size=padded(report_path,report,((len(E.canonical(report))+131071)//4096)*4096)
    P.manifest(review);manifest_path=review/'manifest.json'
    manifest=json.loads(manifest_path.read_text());manifest_size=padded(manifest_path,manifest)
    def write_zip(path):
        with zipfile.ZipFile(path,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            for source in sorted(review.rglob('*')):
                if source.is_dir():continue
                R.regular(source);name=source.relative_to(review).as_posix();E.safe(name)
                info=zipfile.ZipInfo.from_file(source,arcname=name);info.create_system=3
                info.external_attr=(stat.S_IFREG|P.mode(source))<<16
                info.compress_type=zipfile.ZIP_STORED if name.startswith('objects/') or name in ('manifest.json','package-size-report.json') else zipfile.ZIP_DEFLATED
                with source.open('rb') as src,z.open(info,'w',force_zip64=True) as dst:
                    __import__('shutil').copyfileobj(src,dst,1024*1024)
    # The measured temporary ZIP belongs to the review workspace. The final
    # archive alone needs the persistent delivery filesystem's free space.
    provisional=review.parent/(output.name+'.size-measurement')
    write_zip(provisional);report=R.size_report(review,provisional)
    padded(report_path,report,report_size)
    for row in manifest['files']:
        if row['path']=='package-size-report.json':row.update(sha256=E.sha(report_path),size=report_size)
    padded(manifest_path,manifest,manifest_size);P.validate_tree(review)
    write_zip(output)
    E.require(output.stat().st_size==report['compressed_bytes'],'measured ZIP size not stable')
    admission=R.archive_admission(output,extraction)
    result=P.verify_archive(output,extraction)
    semantic=json.loads(result['replay'][-1]['stdout'].splitlines()[-1])
    declared=json.loads((review/'delivery-state.json').read_text())
    acceptance=semantic['current_acceptance']
    E.require(acceptance==declared['current_acceptance'] and acceptance in ('PASS','BLOCKED'),
              'archive semantic acceptance differs from delivery status')
    if acceptance=='PASS':
        E.require(semantic.get('portable_replay',semantic.get('complete_portable_acceptance'))=='PASS',
                  'complete archive has no complete portable acceptance')
        portable='PASS'
    else:
        E.require(semantic.get('complete_portable_acceptance')=='NOT_AVAILABLE' and
            semantic.get('limited_evidence_replay')=='PASS','incomplete archive claims full replay')
        portable='LIMITED_EVIDENCE_PASS'
    selected=R.index(review)
    for row in selected['raw']+selected['synthetic_deltas']:
        if row['purpose']=='bounded-binary-facts':continue
        path=Path(row['local_path'])
        E.require(path.stat().st_size==row['size'] and E.sha(path)==row['sha256'] and
                  stat.S_IMODE(path.stat().st_mode)==row['local_os_permissions'],'original selected evidence mutated')
    replay_entry=json.loads((review/'replay.json').read_text())['commands'][0]['argv'][2]
    previous_bytes=(189539373 if replay_entry.endswith('stack-guard-replay.py') else
                    157936504 if replay_entry.endswith('kernel-wx-replay.py') else
                    164874270 if replay_entry.endswith('low-half-replay.py') else
                    142687084 if replay_entry.endswith('direct-map-replay.py') else 277783251)
    result.update(path=str(output),sha256=E.sha(output),size=output.stat().st_size,archive_admission=admission,
        package_size_report=report,source_files=report['source_files'],selected_originals_rechecked=True,
        package_integrity='PASS',current_acceptance=acceptance,portable_replay=portable,
        complete_portable_acceptance='PASS' if acceptance=='PASS' else 'NOT_AVAILABLE',independent_review='PENDING',
        previous_archive_bytes=previous_bytes,reduction_bytes=previous_bytes-output.stat().st_size,
        size_comparison_authority='Complete proof for distinct increments; archive bytes are not an equivalent-coverage comparison' if acceptance=='PASS' else 'Incomplete coverage; not an equivalent-coverage reduction',
        local_binary_index=str(review/'local-candidate-index.json'))
    Path(str(output)+'.sha256').write_text(result['sha256']+'  '+output.name+'\n')
    E.write(Path(str(output)+'.receipt.json'),result)
    return {k:v for k,v in result.items() if k!='replay'}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('session','campaign','review'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--archive',type=Path);p.add_argument('--extract',type=Path)
    p.add_argument('--preview',action='store_true',help='Select verified complete evidence before commit without claiming delivery.')
    p.add_argument('--selection-audit-only',action='store_true',
                   help='Audit dependency selection under --preview without claiming semantic acceptance or creating an archive.')
    args=p.parse_args()
    for n in ('session','campaign','review'):
        path=getattr(args,n).resolve();path.relative_to(ROOT/'backups');setattr(args,n,path)
    if args.selection_audit_only:
        audit_admission(args.preview,args.archive,args.extract)
        captured=json.loads((args.campaign/'physical-acceptance.json').read_text())
        E.require(captured['build_input_sha256']==E.identity(E.manifest()),'selection audit linked inputs changed')
    else:
        gate=module('physical-translation-gate');derived=gate.verify(args.campaign)
        E.require(derived==json.loads((args.campaign/'physical-acceptance.json').read_text())['result'],'acceptance replay differs')
    if not args.preview:
        E.require(not subprocess.check_output(['git','status','--porcelain=v1'],cwd=ROOT).strip(),'final source not clean committed')
        E.require(all((args.session/'evidence'/name).is_file() for name in ('closure.json','storage-preservation.json')),
                  'final local closure/preservation receipts missing')
    E.require(bool(args.archive)==bool(args.extract),'archive/extraction must be paired')
    selected=select(args.session,args.campaign,args.review,not args.selection_audit_only)
    result={'status':'SELECTION_AUDIT_ONLY' if args.selection_audit_only else 'SELECTED',
            'logical_files':len(selected['raw'])+len(selected['synthetic_deltas']),
            'unique_chunks':len(selected['blobs']),'omitted_full_binaries':len(selected['local_binaries'])}
    if args.archive:
        output=args.archive.resolve();output.relative_to(ROOT/'backups')
        result=archive(args.review,output,args.extract.resolve())
    print(json.dumps(result,sort_keys=True))

if __name__=='__main__':main()
