#!/usr/bin/env python3
"""Reconstruct focused proof and execute the same semantic/native validators."""
import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import memory_evidence as E
import review_transport as R
import physical_current as P

ROOT=Path(__file__).resolve().parent.parent
os.environ['HOBBYOS_VALIDATION_CONTRACT']='physical-translation'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
sys.dont_write_bytecode=True
def module(name):
    spec=importlib.util.spec_from_file_location('physical_portable_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def source_identity(package):
    saved=json.loads((package/'preservation/source-manifest.json').read_text())
    E.require(saved['schema']==2 and saved['head']==P.BASE and saved['head_tree']==saved['export_tree']==P.TREE and
              saved['source_clean'] and saved['git_worktree_clean'] and saved['state']=='CLEAN_COMMITTED_SOURCE',
              'approved baseline source manifest identity')
    before=saved['files'];normalized=[]
    root=package/'baseline-source'
    for row in before:
        E.safe(row['path']);path=root/row['path'];data=path.read_bytes()
        blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        E.require(E.sha(path)==row['sha256'] and len(data)==row['size'] and
                  blob==row['current_blob']==row['base_blob'] and row['tracked'] and
                  row['mode']==row['base_git_mode'] and row['mode'] in ('100644','100755') and
                  path.stat().st_mode&0o777==int(row['mode'][3:],8),'approved translation baseline bytes/modes')
        normalized.append({'path':row['path'],'mode':row['mode'],'current_blob':blob})
    E.require(len(before)==len({r['path'] for r in before}) and {r['path'] for r in before}==
              {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()},'baseline reconstruction completeness')
    E.require(module('package-memory-review').source_tree(normalized)==P.TREE,'approved translation baseline tree')
    current=json.loads((package/'source-manifest.json').read_text())
    old={r['path']:(r['sha256'],r['mode']) for r in before};new={r['path']:(r['sha256'],r['mode']) for r in current['files']}
    changed=sorted(n for n,v in new.items() if old.get(n)!=v)
    E.require(set(old)<=set(new) and set(changed)<=set(P.contract()['allowed_paths']),'portable unrelated translation edit')
    retained=json.loads((package/'preservation/observation-source-retention.json').read_text())
    observation=package/'observation-source';prior=[]
    for row in retained['files']:
        E.safe(row['path'])
        path=observation/row['path'];data=path.read_bytes()
        # The retention receipt records local permissions; transport preserves
        # their Git executable bit with literal canonical reconstruction modes.
        retained_mode=int(row['mode'],8)
        E.require(retained_mode in (0o644,0o664,0o755,0o775),'retained observation source mode')
        canonical_mode=0o755 if retained_mode&0o111 else 0o644
        E.require(E.sha(path)==row['sha256'] and len(data)==row['size'] and
                  path.stat().st_mode&0o777==canonical_mode,
                  'original observation source reconstruction')
        prior.append({'path':row['path'],'mode':'100755' if path.stat().st_mode&0o111 else '100644',
            'current_blob':hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()})
    E.require(len(prior)==len({r['path'] for r in prior}) and {r['path'] for r in prior}==
              {p.relative_to(observation).as_posix() for p in observation.rglob('*') if p.is_file()},
              'original observation source reconstruction completeness')
    E.require(module('package-memory-review').source_tree(prior)=='d0284110ab81d2ffd09583c5ff5d6bc55281ab77',
              'original observation reference tree')
    return changed

def source_fixtures(package):
    """Exercise prior-source semantics on small disposable copies, without Git."""
    cases=[]
    with tempfile.TemporaryDirectory(prefix='physical-source-fixtures-') as temporary:
        scratch=Path(temporary)
        for name in ('baseline-source','observation-source'):
            shutil.copytree(package/name,scratch/name)
        (scratch/'preservation').mkdir()
        for name in ('source-manifest.json','observation-source-retention.json'):
            shutil.copyfile(package/'preservation'/name,scratch/'preservation'/name)
        shutil.copyfile(package/'source-manifest.json',scratch/'source-manifest.json')
        source_identity(scratch)
        manifest=scratch/'preservation/source-manifest.json';saved=json.loads(manifest.read_text())
        retained=scratch/'preservation/observation-source-retention.json';original=json.loads(retained.read_text())
        def reject(name,boundary):
            try:source_identity(scratch)
            except ValueError as error:
                E.require(boundary in str(error),'prior-source fixture rejected at wrong boundary: '+name)
            else:raise ValueError('prior-source fixture accepted: '+name)
            cases.append({'name':name,'expected_boundary':boundary,'status':'REJECTED_AS_EXPECTED'})
        changed=copy.deepcopy(saved);changed['files'][0]['current_blob']='0'*40
        manifest.write_text(json.dumps(changed))
        reject('baseline-blob-forged','approved translation baseline bytes/modes')
        manifest.write_text(json.dumps(saved))
        path=scratch/'baseline-source'/saved['files'][0]['path'];mode=path.stat().st_mode&0o777
        os.chmod(path,0o664);reject('baseline-mode-changed','approved translation baseline bytes/modes');os.chmod(path,mode)
        changed=copy.deepcopy(original);changed['files'][0]['path']='../escape'
        retained.write_text(json.dumps(changed));reject('observation-path-escape','unsafe');retained.write_text(json.dumps(original))
        extra=scratch/'observation-source/synthetic-extra.txt';extra.write_text('Synthetic extra source.\n')
        reject('observation-unlisted-source','reconstruction completeness');extra.unlink()
        path=scratch/'observation-source'/original['files'][0]['path'];data=path.read_bytes();mode=path.stat().st_mode&0o777
        os.chmod(path,0o664);reject('observation-mode-changed','original observation source reconstruction');os.chmod(path,mode)
        path.write_bytes(data+b'\n# Synthetic prior-source mutation.\n')
        changed=copy.deepcopy(original);changed['files'][0].update(size=path.stat().st_size,sha256=E.sha(path))
        retained.write_text(json.dumps(changed))
        reject('observation-rebound-bytes','original observation reference tree')
        path.write_bytes(data);retained.write_text(json.dumps(original));source_identity(scratch)
    return {'positive':2,'negative':len(cases),'cases':cases,
            'boundary':'Synthetic source copies only; exact original Git trees and canonical modes remain required.'}

def replay(package,host_tests):
    os.chdir(ROOT);M=module('memory-portable-replay');transport=M.Transport(package)
    transport.verify_source_identity();changed=source_identity(package);source_proof=source_fixtures(package)
    campaign=package/'evidence/current-campaign'
    source=json.loads((package/'source-manifest.json').read_text())
    if source['source_clean']:
        closure=json.loads((package/'preservation/closure.json').read_text())
        E.require(closure['status']=='PASS' and closure['head']==source['head'] and
                  closure['tree']==source['head_tree'] and closure['branch']=='feat/memory-hardening' and
                  closure['parent']==P.BASE and closure['build_input_sha256']==E.identity(E.manifest()) and
                  closure['harness_sha256']==E.identity(E.manifest(harness=True)) and
                  closure['current_acceptance_receipt_sha256']==E.sha(campaign/'physical-acceptance.json') and
                  closure['git_status']=='' and closure['owned_live_processes']==[] and
                  closure['network_git_operations']==[] and closure['hardware']=='DEFERRED_TO_FINAL_CLOSURE' and
                  closure['next_increment']=='NOT_STARTED','local closure/source receipt contradiction')
        retained=json.loads((package/'preservation/storage-preservation.json').read_text())
        E.require(retained['status']=='PASS' and retained['files_removed']==0 and
                  retained['local_originals_retained'] and
                  sum(r['bytes_reclaimed'] for r in retained['receipts'])==retained['bytes_reclaimed'],
                  'local preservation receipt contradiction')
    G=module('physical-translation-gate')
    native=M.selected_native(campaign,transport)
    E.require(set(native)=={r['key'] for r in module('memory-current-regression').inventory()['requirements']},
              'physical native family inventory missing')
    native_fixtures=M.native_fixtures(campaign,transport)
    inherited_fixtures=M.current_fixtures(campaign,transport,host_tests=host_tests)
    derived=G.verify(campaign,transport)
    accepted=json.loads((campaign/'physical-acceptance.json').read_text())
    E.require(accepted['status']=='PASS' and accepted['build_input_sha256']==E.identity(E.manifest()) and
              accepted['harness_sha256']==E.identity(E.manifest(harness=True)) and accepted['result']==derived,
              'physical acceptance differs from complete replay')
    selected=json.loads((campaign/'tlb-fixtures.json').read_text())['result'];directory=campaign/selected['path']
    current=json.loads((campaign/'physical-fixtures.json').read_text())['result'];physical=campaign/current['path']
    with tempfile.TemporaryDirectory(prefix='physical-portable-fixtures-') as temp:
        dest=Path(temp)
        regenerated=module('tlb-shootdown-fixtures').run(campaign,dest/'invalidation')
        E.require(regenerated==json.loads((directory/'results.json').read_text()),'TLB negative replay differs')
        policy=module('cpu-control-fixtures').run(campaign,dest/'policy',package/'observation-source')
        E.require(policy==json.loads((directory/'policy/results.json').read_text()),'control negative replay differs')
        translation=module('physical-translation-fixtures').run(campaign,dest/'translation')
        E.require(translation==json.loads((physical/'results.json').read_text()),'translation negative replay differs')
        if host_tests:
            (dest/'host').mkdir()
            with contextlib.redirect_stdout(io.StringIO()):G.host(dest/'host')
            G.T.verify_host(dest/'host',True);G.verify_host(dest/'host')
    return {'current_acceptance':'PASS','portable_replay':'PASS','independent_review':'PENDING',
        'normal_boots':21,'nx_disabled_boots':3,'xapic_supplemental_boots':3,'native_families':sorted(native),
        'native_fixtures':native_fixtures,'inherited_fixture_groups':sorted(inherited_fixtures),
        'source_reconstruction_fixtures':source_proof,
        'source_delta':changed,'binary_limitation':transport.index['binary_replay_limit'],
        'hardware':'DEFERRED_TO_FINAL_CLOSURE'}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--host-tests',action='store_true');parser.add_argument('--materialized',action='store_true')
    args=parser.parse_args();package=args.package.resolve()
    if args.materialized:
        print(json.dumps(replay(package,args.host_tests),sort_keys=True));return
    Pkg=module('package-memory-review');Pkg.validate_tree(package)
    E.require(R.selection_audit(R.index(package))==json.loads((package/'selection-audit.json').read_text()),
              'focused dependency selection audit differs')
    before={p.relative_to(package).as_posix():(E.sha(p),p.stat().st_mode&0o777)
            for p in package.rglob('*') if p.is_file()}
    with tempfile.TemporaryDirectory(prefix='physical-review-replay-') as temporary:
        restored=Path(temporary)/'materialized';R.materialize(package,restored)
        argv=[sys.executable,'-B',str(restored/'source/scripts/physical-translation-portable-replay.py'),
              '--package',str(restored),'--materialized']
        if args.host_tests:argv.append('--host-tests')
        process=subprocess.run(argv,text=True,capture_output=True,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
        E.require(process.returncode==0,'materialized semantic/native replay failed:\n'+process.stdout+process.stderr)
        result=json.loads(process.stdout.splitlines()[-1])
    after={p.relative_to(package).as_posix():(E.sha(p),p.stat().st_mode&0o777)
           for p in package.rglob('*') if p.is_file()}
    E.require(before==after,'portable replay changed immutable package evidence');Pkg.validate_tree(package)
    result['exact_reconstruction']='PASS';result['immutable_original_recheck']='PASS'
    print(json.dumps(result,sort_keys=True))

if __name__=='__main__':main()
