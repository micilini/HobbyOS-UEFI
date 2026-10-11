#!/usr/bin/env python3
"""Replay this blocked diagnostic handoff, never full increment acceptance."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import zipfile
import stat

sys.dont_write_bytecode = True
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
ROOT = None
ADAPTER = Path(__file__).resolve().parent


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe(name):
    p = PurePosixPath(name)
    require(name and not p.is_absolute() and str(p) == name and '..' not in p.parts and
            '\\' not in name and ':' not in name, 'unsafe path')
    return name


def verify_payloads(manifest):
    seen = set()
    for row in manifest['files']:
        name = safe(row['path']); p = ROOT/name
        require(name not in seen and not p.is_symlink() and p.is_file(), 'payload membership/type')
        seen.add(name)
        require(p.stat().st_mode & 0o777 == row['mode'] and row['mode'] in (0o644, 0o755), 'payload exact mode')
        require(p.stat().st_size == row['size'] and sha(p) == row['sha256'], 'payload changed: '+name)
    actual = {p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*') if p.is_file() or p.is_symlink()}
    require(actual == seen | {'transport-manifest.json'}, 'unexpected or missing package member')
    return len(seen)


def extract(archive, destination, expected_sha):
    require(sha(archive) == expected_sha, 'archive identity')
    require(not destination.exists(), 'extraction destination exists')
    with zipfile.ZipFile(archive) as z:
        members = z.infolist()
        names = [safe(m.filename) for m in members]
        require(len(names) == len(set(names)), 'duplicate archive member')
        require(shutil.disk_usage(destination.parent).free >=
                sum(m.file_size for m in members) + 4*1024**3 + 64*1024**2, 'BLOCKED_STORAGE extraction')
        destination.mkdir()
        for member in members:
            mode = member.external_attr >> 16
            require(stat.S_ISREG(mode) and stat.S_IMODE(mode) in (0o644, 0o755), 'archive regular path/mode')
            path = destination/member.filename
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open('xb') as stream:
                stream.write(z.read(member))  # ZIP CRC is checked by this read.
            path.chmod(stat.S_IMODE(mode))
    return destination


def module(name):
    spec = importlib.util.spec_from_file_location('diagnostic_'+name.replace('-', '_'), ROOT/'source/scripts'/(name+'.py'))
    value = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = value
    spec.loader.exec_module(value)
    return value


def git_tree(files):
    tree = {}
    for name, data, executable in files:
        parts = PurePosixPath(name).parts; parent = tree
        for part in parts[:-1]: parent = parent.setdefault(part, {})
        parent[parts[-1]] = (data, executable)
    def digest(kind, data): return hashlib.sha1(kind+b' '+str(len(data)).encode()+b'\0'+data).digest()
    def visit(node):
        body = b''
        for name, value in sorted(node.items(), key=lambda item: (item[0]+('/' if isinstance(item[1], dict) else '')).encode()):
            if isinstance(value, dict): mode, child = b'40000', visit(value)
            else: mode, child = (b'100755' if value[1] else b'100644'), digest(b'blob', value[0])
            body += mode+b' '+name.encode()+b'\0'+child
        return digest(b'tree', body)
    return visit(tree).hex()


def main(output):
    require(not output.exists(), 'output already exists')
    output.mkdir(parents=True)
    manifest = json.loads((ROOT/'transport-manifest.json').read_text())
    checked = verify_payloads(manifest)
    source_manifest=json.loads((ROOT/'source-manifest.json').read_text())
    source_files=[];source_names=set()
    for row in source_manifest['files']:
        name=safe(row['path']);path=ROOT/'source'/name
        require(name not in source_names and path.stat().st_size==row['size'] and
                sha(path)==row['sha256'] and path.stat().st_mode & 0o777==row['mode'], 'source identity')
        source_names.add(name);source_files.append((name,path.read_bytes(),bool(row['mode'] & 0o111)))
    require(source_names=={p.relative_to(ROOT/'source').as_posix() for p in (ROOT/'source').rglob('*') if p.is_file()},
            'source member inventory')
    source_tree=git_tree(source_files)
    if manifest.get('role')=='OBSERVER_CORRECTION_DIAGNOSTIC':
        require(source_tree==source_manifest['tree'],'corrected dirty source tree')
    session = output/'session'; session.mkdir()
    mapping = json.loads((ROOT/'session-map.json').read_text())
    destinations = set()
    for row in mapping['files']:
        name = safe(row['path']); require(name not in destinations, 'duplicate materialization')
        destinations.add(name)
        src = ROOT/safe(row['storage']); dst = session/name
        require(src.stat().st_size == row['size'] and sha(src) == row['sha256'], 'materialization input')
        dst.parent.mkdir(parents=True, exist_ok=True)
        with dst.open('xb') as stream: stream.write(src.read_bytes())
        dst.chmod(row['mode'])
    baseline = output/'baseline-source'; baseline.mkdir()
    original = []
    with tarfile.open(session/'before/starting-source.tar.gz') as archive:
        for member in archive.getmembers():
            name = safe(member.name); require(member.isfile(), 'baseline archive type')
            data = archive.extractfile(member).read(); path = baseline/name
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open('xb') as stream: stream.write(data)
            path.chmod(0o755 if member.mode & 0o111 else 0o644)
            original.append((name, data, bool(member.mode & 0o111)))
    require(len(original) == 759 and git_tree(original) == '3ce6be4fcae7c23fd41adb439e081af72ea4717c', 'approved baseline tree')
    commit = (ROOT/'baseline-commit.txt').read_bytes()
    require(hashlib.sha1(b'commit '+str(len(commit)).encode()+b'\0'+commit).hexdigest() ==
            'd68a3b4a2c97f55dfdcc445bfe42317e09e1c55e', 'approved commit object')
    sys.path.insert(0, str(ROOT/'source/scripts'))
    import irq_latency_evidence as V
    import irq_latency_workload as W
    import irq_latency_authority as A
    os.chdir(ROOT/'source')
    if manifest.get('role')=='OBSERVER_CORRECTION_DIAGNOSTIC':
        return corrected(output,session,baseline,manifest,checked,source_tree)
    print('START actual hosted C/assembly', flush=True)
    hosted = module('irq-latency-host').run(output/'hosted')
    run = session/'attempts/components-kvm8'
    serial = run/'qemu-serial.log'
    print('START raw clocks, maxima, components and workload', flush=True)
    raw = V.inspect(serial.read_text())
    require(raw == json.loads((session/'evidence/components-raw-replay.json').read_text()), 'raw replay differs')
    work = W.verify(V.records(serial.read_text()), 8)
    require(work == json.loads((session/'evidence/components-workload.json').read_text()), 'workload replay differs')
    authority = A.verify_series([json.loads((run/name).read_text()) for name in
        ('host-running.json','host-after-stress.json','host-terminal.json')], json.loads((run/'launch.json').read_text()))
    require(authority == json.loads((session/'evidence/components-host-authority.json').read_text()), 'host authority differs')
    print('START semantic diagnostic mutations', flush=True)
    fixtures = module('irq-latency-fixtures').run(serial)
    require(fixtures == json.loads((session/'evidence/components-extended-raw-fixtures.json').read_text()), 'raw fixtures differ')
    work_fixtures = module('irq-latency-workload-fixtures').run(serial.read_text(), 8)
    require(work_fixtures == json.loads((session/'evidence/components-workload-fixtures.json').read_text()), 'workload fixtures differ')
    host_fixtures = module('irq-latency-authority-fixtures').run(run/'host-running.json',run/'launch.json')
    require(host_fixtures == json.loads((session/'evidence/host-authority-fixtures.json').read_text()), 'host fixtures differ')
    print('START local production pair comparison; original reproduction is separate', flush=True)
    production = json.loads((session/'evidence/production-code-identity/result.json').read_text())
    spec = importlib.util.spec_from_file_location('portable_production_comparison', ADAPTER/'irq_latency_production.py')
    paired = importlib.util.module_from_spec(spec); spec.loader.exec_module(paired)
    paired_result = paired.compare(baseline, ROOT/'source', production, output/'production-pairs')
    require(verify_payloads(manifest) == checked, 'post-replay payload integrity')
    result = {'schema': 1, 'status': 'DIAGNOSTIC_REPLAY_PASS', 'implementation_acceptance': False,
        'complete_semantic_native_replay': False, 'blocked_reason': 'POST_STRESS_OBSERVER_ENVELOPE_NOT_QUALIFIED',
        'observer_post_failures': raw['observer_post_failures'], 'payloads_rechecked': checked,
        'baseline_tree': '3ce6be4fcae7c23fd41adb439e081af72ea4717c',
        'actual_hosted_executions': len(hosted['executions']), 'production_objects_recompiled_each_role':len(production['rows']),
        'production_comparison_mode': paired_result['mode'],
        'recorded_build': paired_result['recorded_build'],
        'adapter_sha256': sha(Path(__file__).resolve()),
        'comparison_adapter_sha256': sha(ADAPTER/'irq_latency_production.py'),
        'raw_negative_cases': len(fixtures['negative_cases']), 'workload_negative_cases': work_fixtures['negative'],
        'host_negative_cases': len(host_fixtures['negative_cases']), 'new_qemu_boots': 0,
        'hardware': False, 'scope': 'Current diagnostic subset only; 39 boots and 17 native families are not replayed.'}
    (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)


def corrected(output, session, baseline, manifest, checked, source_tree):
    """The successor diagnostic uses current code; complete acceptance is separate."""
    import memory_evidence as E
    import irq_latency_evidence as V
    import irq_latency_authority as A
    import irq_latency_workload as W
    import irq_latency_current as I
    import stack_current as C
    os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
    os.environ['HOBBYOS_MEMORY_OBSERVER_ROLE']=I.ROLE
    print('START corrected source semantics and role controls',flush=True)
    C.contract();C.audit_sites();C.source_guard();C.boundary_semantics()
    scope=module('irq-latency-scope-fixtures').run(baseline,output/'scope-controls')
    print('START 24 corrected hosted executions and inherited hosted chain',flush=True)
    hosted=module('irq-latency-host').run(output/'hosted')
    (output/'inherited-host').mkdir()
    stack=module('stack-guard-gate');stack.host(output/'inherited-host')
    inherited=stack.verify_host(output/'inherited-host')
    commands=[json.loads(p.read_text()) for p in (output/'inherited-host').rglob('command.json')]
    executed=[r for r in commands if r['argv'] and Path(r['argv'][0]).is_absolute() and
              Path(r['argv'][0]).is_relative_to(output/'inherited-host')]
    require(len(executed)==111 and all(r['exit_status']==0 for r in executed),'inherited hosted execution inventory')
    run=session/'attempts/components-kvm8';serial=run/'qemu-serial.log'
    print('START historical raw replay under corrected checker',flush=True)
    raw=V.inspect(serial.read_text())
    require(raw==json.loads((session/'evidence/components-raw-replay.json').read_text()),'historical raw facts changed')
    workload=W.verify(V.records(serial.read_text()),8)
    require(workload==json.loads((session/'evidence/components-workload.json').read_text()),'historical workload changed')
    authority=A.verify_series([json.loads((run/name).read_text()) for name in
        ('host-running.json','host-after-stress.json','host-terminal.json')],json.loads((run/'launch.json').read_text()))
    require(authority==json.loads((session/'evidence/components-host-authority.json').read_text()),'historical capacity changed')
    fixtures=module('irq-latency-fixtures').run(serial)
    work=module('irq-latency-workload-fixtures').run(serial.read_text(),8)
    host=module('irq-latency-authority-fixtures').run(run/'host-running.json',run/'launch.json')
    for result,name in ((fixtures,'components-extended-raw-fixtures'),(work,'components-workload-fixtures'),
                        (host,'host-authority-fixtures')):
        require(result==json.loads((session/'evidence'/(name+'.json')).read_text()),'historical negatives changed: '+name)
    envelope=module('irq-latency-envelope-fixtures').run(serial)
    scheduling=module('irq-latency-scheduling-fixtures').run()
    admission_path=session/'correction/experiments/token-observer'
    admission=V.calibration_diagnostic(V.records((admission_path/'runtime/qemu-serial.log').read_text()))
    require(admission==json.loads((admission_path/'admission-diagnostic.json').read_text()),'failed admission changed')
    print('START current paired compilation and actual compiler controls',flush=True)
    controlled=output/'production-controls';controlled.mkdir()
    shutil.copytree(baseline,controlled/'baseline');shutil.copytree(ROOT/'source',controlled/'current')
    recorded=json.loads((session/'evidence/production-code-identity/result.json').read_text())
    production=module('irq-latency-production-fixtures').controls(controlled/'baseline',controlled/'current',recorded,controlled)
    require(verify_payloads(manifest)==checked,'post-correction replay immutable inputs')
    result={'schema':2,'status':'CORRECTED_DIAGNOSTIC_REPLAY_PASS','source_tree':source_tree,
        'implementation_acceptance':False,'complete_portable_acceptance':False,
        'timing_authority':'BLOCKED_TIMING_AUTHORITY','independent_review':'PENDING',
        'payloads_rechecked':checked,'actual_hosted_executions':len(hosted['executions']),
        'inherited_hosted_executions':len(executed),'inherited_hosted':inherited,
        'raw_negatives':len(fixtures['negative_cases']),'workload_negatives':work['negative'],
        'capacity_negatives':len(host['negative_cases']),'envelope_controls':envelope,
        'scheduling_controls':scheduling,'scope_controls':scope,'production_controls':production,
        'historical_post_failures':raw['observer_post_failures'],'finite_diagnostic_admission':admission,
        'new_qemu_boots':0,'hardware':False,'full_images_rehashed_or_executed':False,
        'omissions':['39 current complete functional boots','three qualified timing passes',
            '17 current native families','complete current canonical aggregate','hardware closure']}
    E.write(output/'result.json',result)
    print(json.dumps({k:v for k,v in result.items() if k not in ('inherited_hosted','envelope_controls',
        'scheduling_controls','scope_controls','production_controls','finite_diagnostic_admission')}),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--package', type=Path)
    inputs.add_argument('--archive', type=Path)
    parser.add_argument('--archive-sha256')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.archive:
        require(args.archive_sha256 is not None, 'archive SHA-256 required')
        ROOT = extract(args.archive.resolve(), args.output.resolve().with_name(args.output.name+'-inputs'), args.archive_sha256)
    else:
        ROOT = args.package.resolve()
    require(not args.output.resolve().is_relative_to(ROOT), 'scratch overlaps immutable package')
    main(args.output.resolve())
