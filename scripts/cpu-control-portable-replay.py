#!/usr/bin/env python3
"""Replay control acceptance and the retained native observation contracts.

No kernel build, VM, network or original absolute path is used. Optional actual
C compilation is confined to hosted substituted-primitive tests and fixtures.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

os.environ.setdefault('HOBBYOS_VALIDATION_CONTRACT','cpu-control-state')
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E
import cpu_current

ROOT=Path(__file__).resolve().parent.parent


def module(name):
    spec=importlib.util.spec_from_file_location('control_portable_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value


def baseline_source(package):
    """The approved tree is reconstructed from bytes, not a self-asserted label."""
    root=package/'baseline-source'
    rows=json.loads((package/'preservation/source-manifest.json').read_text())['files']
    normalized=[]
    for row in rows:
        E.safe(row['path']);path=root/row['path'];data=path.read_bytes()
        blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        E.require(path.stat().st_size==row['size'] and E.sha(path)==row['sha256'] and
                  blob==row['blob'] and ('100755' if path.stat().st_mode&0o111 else '100644')==row['git_mode'],
                  'approved baseline source/mode/blob differs')
        normalized.append({'path':row['path'],'mode':row['git_mode'],'current_blob':blob})
    E.require(len(rows)==len({r['path'] for r in rows}) and {r['path'] for r in rows}==
              {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()},'approved source inventory differs')
    tree=module('package-memory-review').source_tree(normalized)
    E.require(tree=='d0284110ab81d2ffd09583c5ff5d6bc55281ab77','approved pre-control tree differs')
    current=json.loads((package/'source-manifest.json').read_text())['files']
    old={r['path']:(r['sha256'],r['git_mode']) for r in rows}
    new={r['path']:(r['sha256'],r['mode']) for r in current}
    E.require(set(old)<=set(new),'approved source unexpectedly removed')
    changed={name for name,value in new.items() if old.get(name)!=value}
    E.require(changed<=set(cpu_current.contract()['allowed_paths']),'portable unrelated source change')
    return root,sorted(changed)


def previous_capture(campaign,transport):
    receipt=json.loads((campaign/'previous-observation.json').read_text())
    root=campaign/receipt['copy'];names=set()
    for row in receipt['files']:
        E.safe(row['path']);E.require(row['path'] not in names,'duplicate baseline artifact');names.add(row['path'])
        path=root/row['path']
        if path.exists():E.require(path.stat().st_size==row['size'] and E.sha(path)==row['sha256'],'approved observation changed')
        else:transport.binary(path,row['sha256'],row['size'])
    prefix=root.relative_to(transport.root).as_posix()+'/'
    E.require({p[len(prefix):] for p in transport.content if p.startswith(prefix)}==names,'baseline member inventory differs')
    E.require(E.sha(root/'candidate.json')==receipt['candidate_id']==
              'ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c','approved baseline candidate identity')


def native_output(result, expected_runs):
    """Native validators may print progress before their final JSON summary."""
    E.require(result.returncode == 0, 'native portable acceptance failed: ' + result.stdout + result.stderr)
    try:
        value = json.loads(result.stdout.strip().splitlines()[-1])
    except (IndexError, json.JSONDecodeError) as error:
        raise ValueError('native portable final summary malformed') from error
    E.require(isinstance(value, dict) and value.get('package_content_replay') == 'PASS' and
              value.get('current_acceptance') == 'PASS', 'native portable final summary status differs')
    required = {row['key'] for row in json.loads((ROOT/'scripts/memory-current-requirements.json').read_text())['requirements']}
    E.require(isinstance(value.get('native'), dict) and set(value['native']) == required,
              'native portable family inventory differs')
    E.require(type(value.get('series_runs')) is int and value['series_runs'] == expected_runs,
              'native portable series count differs')
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True)
    p.add_argument('--host-tests',action='store_true')
    a=p.parse_args();package=a.package.resolve();campaign=package/'evidence/current-campaign'
    os.chdir(ROOT)
    M=module('memory-portable-replay');transport=M.Transport(package);transport.verify_source_identity()
    baseline,changed=baseline_source(package);previous_capture(campaign,transport)
    # Native validators retain their full current family/role inventory and
    # distinguish omitted binary facts from runtime measurements.
    args=[sys.executable,'-B',str(ROOT/'scripts/memory-portable-replay.py'),'--package',str(package),
          '--require-acceptance','--native-fixtures','--current-fixtures']
    if a.host_tests:args.append('--host-tests')
    native=subprocess.run(args,text=True,capture_output=True)
    E.require(native.returncode==0,'native portable acceptance failed: '+native.stdout+native.stderr)
    C=module('cpu-control-state-gate');derived=C.verify(campaign,transport)
    native_summary=native_output(native,derived['normal_boots'])
    accepted=json.loads((campaign/'control-acceptance.json').read_text())
    E.require(accepted['status']=='PASS' and accepted['build_input_sha256']==E.identity(E.manifest()) and
              accepted['harness_sha256']==E.identity(E.manifest(harness=True)) and accepted['result']==derived,
              'control acceptance differs from independent replay')
    F=module('cpu-control-fixtures')
    selected=json.loads((campaign/'control-fixtures.json').read_text())['result']
    original=json.loads((campaign/selected['path']/'results.json').read_text());F.verify_receipt(original)
    with tempfile.TemporaryDirectory(prefix='cpu-control-portable-') as temp:
        destination=Path(temp)
        regenerated=F.run(campaign,destination/'negative',baseline)
        E.require(regenerated==original,'control negative recipes/raw outcomes changed')
        if a.host_tests:
            (destination/'host').mkdir()
            with contextlib.redirect_stdout(io.StringIO()):C.host(destination/'host',inherited=False)
            E.require(C.verify_host(destination/'host')==derived['host'],'actual C control host replay differs')
    print(json.dumps({'current_acceptance':'PASS','portable_replay':'PASS','independent_review':'PENDING',
        'normal_boots':derived['normal_boots'],'nx_disabled_boots':derived['nx_disabled_boots'],
        'production_boots':derived['production_boots'],'inherited_requirements':derived['inherited']['requirements'],
        'policy_negative_controls':len(original['cases']),'source_delta':changed,
        'native_replay':native_summary,
        'native_process':{'argv':args,'exit_status':native.returncode,'stdout':native.stdout,'stderr':native.stderr},
        'binary_limitation':transport.index['binary_replay_limit']},sort_keys=True))


if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
