#!/usr/bin/env python3
"""Replay source, scoped native evidence and acknowledged invalidation offline."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess

os.environ['HOBBYOS_VALIDATION_CONTRACT']='tlb-coherency'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E
import tlb_current as T

ROOT=Path(__file__).resolve().parent.parent

def module(name):
    spec=importlib.util.spec_from_file_location('tlb_portable_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def baseline_source(package):
    root=package/'baseline-source';rows=json.loads((package/'preservation/source-manifest.json').read_text())['files']
    normalized=[]
    for row in rows:
        E.safe(row['path']);path=root/row['path'];data=path.read_bytes()
        blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        E.require(len(data)==row['size'] and E.sha(path)==row['sha256'] and blob==row['blob'] and
                  ('100755' if path.stat().st_mode&0o111 else '100644')==row['git_mode'],
                  'approved pre-invalidation source identity differs')
        normalized.append({'path':row['path'],'mode':row['git_mode'],'current_blob':blob})
    E.require(len(rows)==len({r['path'] for r in rows}) and {r['path'] for r in rows}==
              {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()},'approved source inventory differs')
    E.require(module('package-memory-review').source_tree(normalized)==T.contract()['base_tree'],
              'approved pre-invalidation tree differs')
    current=json.loads((package/'source-manifest.json').read_text())['files']
    old={r['path']:(r['sha256'],r['git_mode']) for r in rows}
    new={r['path']:(r['sha256'],r['mode']) for r in current}
    E.require(set(old)<=set(new),'approved source unexpectedly removed')
    changed=sorted(name for name,value in new.items() if old.get(name)!=value)
    E.require(set(changed)<=set(T.contract()['allowed_paths']),'portable unrelated source edit')
    return changed

def partial(campaign,transport):
    """A blocked delivery replays only named completed work, never acceptance."""
    G=module('tlb-shootdown-gate');V=module('verify-tlb-shootdown')
    T.audit_sites();T.caller_semantics();G.G.verify_static(campaign)
    host=G.verify_host(campaign)
    builds={name:G.verify_codegen(campaign/'candidates'/name,name!='selftest',transport)
            for name in ('selftest','debug','production')}
    attempts=[];sequence=0
    for path in sorted((campaign/'campaign-attempts').glob('*.json')):
        if path.name.endswith('.result.json'):continue
        start=json.loads(path.read_text());finish=json.loads(path.with_suffix('.result.json').read_text());sequence+=1
        E.require(start['sequence']==sequence and finish['start_sha256']==E.sha(path),'partial attempt identity differs')
        directory=campaign/start['path'];E.safe(start['path'])
        if finish['status']=='PASS':
            actual=V.verify_run(directory)
            E.require(actual['policy']['candidate_id']==start['candidate_id'] and
                      E.sha(directory/'result.json')==finish['result_sha256'],'partial successful attempt differs')
            attempts.append({'path':start['path'],'status':'PASS','result':actual})
        else:
            E.require(finish['status']=='FAIL' and finish.get('reason'),'partial failure reason missing')
            attempts.append({'path':start['path'],'status':'FAIL','reason':finish['reason']})
    diagnostics=[]
    for path in sorted((campaign/'diagnostics').glob('*/result.json')):
        row=json.loads(path.read_text())
        if row['status']=='COLLECTED' and row['readback']:diagnostics.append(V.verify_run(path.parent))
    production=[]
    if (campaign/'production.json').is_file():
        for name in json.loads((campaign/'production.json').read_text())['runs']:
            production.append(V.verify_run(campaign/name))
        E.require(len(production)==2 and {p['policy']['profile'] for p in production}=={'q35-tcg-smp1','q35-kvm-smp4'},
                  'partial production coverage contradicted')
    failures={}
    if (campaign/'tlb-failure-experiments.json').is_file():
        for kind,name in json.loads((campaign/'tlb-failure-experiments.json').read_text()).items():
            failures[kind]=G.O.verify_run(campaign/name/'qemu-serial.log',failure_experiment=kind)
    focused=None
    if (campaign/'focused-fixtures.json').is_file():
        receipt=json.loads((campaign/'focused-fixtures.json').read_text())
        E.require(receipt['harness_sha256']==E.identity(E.manifest(harness=True)) and
                  receipt['build_input_sha256']==E.identity(E.manifest()),'focused fixture version changed')
        selected=receipt['result'];base=campaign/selected['path'];capture=campaign/selected['capture']
        for path,key in [('invalidation/results.json','invalidation_sha256'),('policy/results.json','policy_sha256'),
                         ('capture-recipes/result.json','capture_sha256')]:
            E.require(E.sha(base/path)==selected[key],'focused fixture receipt differs')
        with tempfile.TemporaryDirectory(prefix='tlb-focused-replay-') as tmp:
            dest=Path(tmp)
            invalidation=module('tlb-shootdown-fixtures').run(campaign,dest/'invalidation')
            E.require(invalidation==json.loads((base/'invalidation/results.json').read_text()),'focused invalidation replay differs')
            policy=module('cpu-control-fixtures').run(campaign,dest/'policy',transport.root/'observation-source')
            E.require(policy==json.loads((base/'policy/results.json').read_text()),'focused policy replay differs')
            for name,script,option in [('decoder','memory-observation-fixtures.py','--evidence'),
                                       ('interrupt','memory-interrupt-fixtures.py','--output'),
                                       ('packaging','memory-packaging-fixtures.py','--output')]:
                process=subprocess.run([sys.executable,'-B',str(ROOT/'scripts'/script),option,str(dest/name)],text=True,capture_output=True)
                E.require(process.returncode==0,'focused '+name+' replay failed: '+process.stdout+process.stderr)
            process=subprocess.run([sys.executable,'-B',str(ROOT/'scripts/memory-replay-fixtures.py'),
                '--source',str(capture),'--replay',str(base/'capture-recipes/result.json')],text=True,capture_output=True)
            E.require(process.returncode==0,'focused real-capture recipes failed: '+process.stdout+process.stderr)
            focused={'invalidation_negatives':len(invalidation['cases']),'policy_negatives':len(policy['cases']),
                     'real_capture_recipes':'PASS','decoder_interrupt_packaging':'PASS'}
    return {'current_acceptance':'INCOMPLETE','portable_evidence_replay':'PASS','host':host,
            'build_profiles':list(builds),'attempts':attempts,'diagnostics':diagnostics,'production':production,
            'isolated_failures':failures,'focused_fixtures':focused,'independent_review':'PENDING'}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True);parser.add_argument('--host-tests',action='store_true')
    parser.add_argument('--incomplete',action='store_true');args=parser.parse_args()
    package=args.package.resolve();os.chdir(ROOT)
    M=module('memory-portable-replay');transport=M.Transport(package);transport.verify_source_identity()
    changed=baseline_source(package);campaign=package/'evidence/current-campaign'
    G=module('tlb-shootdown-gate')
    if args.incomplete:
        E.require(json.loads((package/'delivery-state.json').read_text())['current_acceptance']=='INCOMPLETE',
                  'partial replay cannot certify acceptance')
        result=partial(campaign,transport)
        if args.host_tests:
            with tempfile.TemporaryDirectory(prefix='tlb-partial-host-') as temp:
                target=Path(temp)
                with contextlib.redirect_stdout(io.StringIO()):G.host(target)
                G.verify_host(target,True)
    else:
        native=M.selected_native(campaign,transport)
        E.require(set(native)=={r['key'] for r in module('memory-current-regression').inventory()['requirements']},
                  'native regression family inventory differs')
        native_fixtures=M.native_fixtures(campaign,transport)
        current_fixtures=M.current_fixtures(campaign,transport,host_tests=args.host_tests)
        derived=G.verify(campaign,transport)
        accepted=json.loads((campaign/'tlb-acceptance.json').read_text())
        E.require(accepted['status']=='PASS' and accepted['build_input_sha256']==E.identity(E.manifest()) and
                  accepted['harness_sha256']==E.identity(E.manifest(harness=True)) and accepted['result']==derived,
                  'invalidation acceptance differs from independent replay')
        selected=json.loads((campaign/'tlb-fixtures.json').read_text())['result'];directory=campaign/selected['path']
        F=module('tlb-shootdown-fixtures');C=module('cpu-control-fixtures')
        with tempfile.TemporaryDirectory(prefix='tlb-portable-fixtures-') as temp:
            dest=Path(temp)
            regenerated=F.run(campaign,dest/'invalidation')
            E.require(regenerated==json.loads((directory/'results.json').read_text()),'invalidation negative replay differs')
            policy=C.run(campaign,dest/'policy',package/'observation-source')
            E.require(policy==json.loads((directory/'policy/results.json').read_text()),'retained policy negatives differ')
            if args.host_tests:
                (dest/'host').mkdir()
                with contextlib.redirect_stdout(io.StringIO()):G.host(dest/'host')
                E.require(G.verify_host(dest/'host',True)==derived['host'],'real C host replay differs')
        result={'current_acceptance':'PASS','portable_replay':'PASS','independent_review':'PENDING',
                'normal_boots':derived['normal_boots'],'nx_disabled_boots':derived['nx_disabled_boots'],
                'xapic_supplemental_boots':derived['xapic_supplemental_boots'],
                'native_families':sorted(native),'native_fixtures':native_fixtures,'current_fixtures':current_fixtures}
    result.update(source_delta=changed,binary_limitation=transport.index['binary_replay_limit'])
    print(json.dumps(result,sort_keys=True))

if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
