"""Native stage predicates used for resource admission and exact resumption."""
import json
import os
import memory_evidence as E
import validation_resources as R
import validation_candidates as candidates


def read(path):
    return json.loads(path.read_text())


def static_base(evidence):
    """Select a sealed current static successor without changing old receipts."""
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') not in ('kernel-image-protection','guarded-kernel-stacks'):return evidence
    revisions=sorted((evidence/'stage-revisions/current-static').glob('*/complete.json'))
    if not revisions:return evidence
    path=revisions[-1];row=read(path);base=path.parent
    E.require(row.get('schema')==1 and row.get('status')=='PASS' and
        row.get('build_input_sha256')==E.identity(E.manifest()) and
        row.get('harness_sha256')==E.identity(E.manifest(harness=True)), 'static successor source/status differs')
    E.require(row.get('predecessor_sha256')==E.sha(evidence/'image-static.json'),
        'static successor predecessor changed')
    names=set()
    for item in row['files']:
        name=item['path'];E.safe(name);target=base/name
        E.require(name not in names and target.is_file() and not target.is_symlink() and
            target.resolve().is_relative_to(base.resolve()) and target.stat().st_size==item['size'] and
            E.sha(target)==item['sha256'],'static successor raw dependency differs')
        names.add(name)
    E.require(names=={p.relative_to(base).as_posix() for p in base.rglob('*') if p.is_file()}-{'complete.json'} and
        'image-static.json' in names and 'static.json' in names,'static successor closure incomplete')
    return base


def host_base(evidence):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') not in ('kernel-image-protection','guarded-kernel-stacks'):return evidence
    revisions=sorted((evidence/'stage-revisions/direct-host').glob('*/complete.json'))
    if not revisions:return evidence
    path=revisions[-1];row=read(path);base=path.parent
    E.require(row['schema']==1 and row['status']=='PASS' and
        row['build_input_sha256']==E.identity(E.manifest()) and
        E.sha(base/'host-inputs.json')==row['host_inputs_sha256'] and
        E.sha(base/'direct-host.json')==row['direct_host_sha256'],'image inherited host successor binding')
    inputs=read(base/'host-inputs.json')
    E.require(E.identity(inputs['build'])==row['build_input_sha256'] and
        E.identity(inputs['harness'])==row['harness_sha256'],'image inherited host tested input identity')
    return base


def seal_static(evidence,base):
    E.require(base.parent==evidence/'stage-revisions/current-static','static successor publication path')
    files=[{'path':p.relative_to(base).as_posix(),'size':p.stat().st_size,'sha256':E.sha(p)}
        for p in sorted(base.rglob('*')) if p.is_file()]
    E.require(sum(r['size'] for r in files)<=16*R.MIB,'static successor bound')
    E.write(base/'complete.json',{'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
        'harness_sha256':E.identity(E.manifest(harness=True)),'files':files,
        'predecessor_sha256':E.sha(evidence/'image-static.json')})
    E.require(static_base(evidence)==base,'static successor publication failed')


def runtime_receipt(evidence, prefix):
    E.require(prefix in ('physical', 'device', 'direct', 'low') or
        prefix=='image' and os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks') or
        prefix=='stack' and os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks',
        'unknown runtime receipt family')
    revisions = sorted((evidence / 'stage-revisions' / (prefix + '-runtime')).glob('*/complete.json'))
    return revisions[-1] if revisions else evidence / (prefix + '-runtime.json')


def runtime_predecessor(evidence, row):
    if 'previous_receipt' not in row:
        return
    seen = set()
    while 'previous_receipt' in row:
        previous = row['previous_receipt']; E.safe(previous['path'])
        E.require(previous['path'] not in seen, 'runtime receipt ancestry cycle')
        seen.add(previous['path'])
        path = evidence / previous['path']
        E.require(E.sha(path) == previous['sha256'], 'previous runtime receipt changed')
        old = read(path)
        strip = lambda value: {k: v for k, v in value.items() if k not in ('harness_sha256', 'previous_receipt')}
        E.require(strip(old) == strip(row), 'runtime revision changed captured facts')
        row = old


def revalidate_runtime(evidence, prefix, validate):
    """Publish a new verifier result only after replaying the unchanged capture."""
    path = runtime_receipt(evidence, prefix)
    state = validate(path)
    if state['state'] == 'REUSABLE':
        return state
    E.require(state['state'] == 'STALE', 'runtime revalidation needs an existing stale receipt: ' + str(state))
    row = read(path)
    E.require(row['build_input_sha256'] == E.identity(E.manifest()), 'runtime revalidation changed linked inputs')
    runtime_predecessor(evidence, row)
    directory = evidence / 'stage-revisions' / (prefix + '-runtime') / R.stamp()
    directory.mkdir(parents=True)
    row = dict(row, harness_sha256=E.identity(E.manifest(harness=True)),
               previous_receipt={'path': path.relative_to(evidence).as_posix(), 'sha256': E.sha(path)})
    pending = directory / 'candidate.json'; E.write(pending, row)
    result = validate(pending)
    E.write(directory / 'replay.json', result)
    E.require(result['state'] == 'REUSABLE', 'current runtime replay failed: ' + str(result))
    pending.rename(directory / 'complete.json')
    return result


def candidate_role(profile, role):
    expected={'selftest':['DEBUG_ASSERT=1','SELFTEST=1','SELFTEST_AUTORUN=0'],
              'debug':['DEBUG_ASSERT=1','SELFTEST=0'],
              'production':['DEBUG_ASSERT=0','SELFTEST=0'],
              'framebuffer-reference':['DEBUG_ASSERT=1','SELFTEST=1','SELFTEST_AUTORUN=0','FRAMEBUFFER_REFERENCE=1']}
    E.require(role in expected and profile==expected[role], 'candidate role/options mismatch')


def production_roles(rows, candidate):
    E.require(len(rows)==2 and {r['profile'] for r in rows}=={'q35-tcg-smp1','q35-kvm-smp4'} and
              all(r['production'] is True and r['candidate_id']==candidate for r in rows) and
              len({(r['vm_id'],r['boot_id']) for r in rows})==2,
              'production role/profile/boot identity mismatch')


def ledger(gate, evidence):
    native = gate.module('memory-current-regression')
    def header(row):
        rows = row.get('rows', [])
        E.require(rows and len({r['build_input_sha256'] for r in rows}) == 1,
                  'native ledger source closure missing')
        return {'schema': row.get('schema'),
                'status': 'PASS' if all(r.get('status') == 'PASS' for r in rows) else 'FAIL',
                'build_input_sha256': rows[0]['build_input_sha256'],
                'harness_sha256': row.get('replay_harness_sha256')}
    source = E.identity(E.manifest())
    return R.classify(evidence / 'regression-ledger.json',
                      lambda row: native.verify_ledger(evidence, source, ledger=row),
                      source=source, harness=E.identity(E.manifest(harness=True)), header=header)


def static_result(gate, evidence, prefix):
    if prefix == 'device':
        E.require(read(evidence / 'device-test-plan.json') == gate.plan(), 'current command plan changed')
        result = gate.D.scope('owned-device-mapping')
        result['guard'] = gate.D.source_guard(); result['instructions'] = gate.D.audit_sites()
        result['worker'] = gate.module('device-mapping-order').fixtures(
            (gate.ROOT / 'kernel/src/core/kernel_init.c').read_text())
    else:
        result = gate.P.scope('physical-boundary'); result['boundary'] = gate.P.boundary_semantics()
        result['conversion_inventory'] = gate.module('physical-source-inventory').verify()
    return result


def stage(gate, evidence, prefix, mode, *, receipt_override=None):
    """Re-run the stage's own semantic predicate, never a presence surrogate."""
    E.require(prefix in ('physical', 'device'), 'unknown native stage family')
    physical = gate.P if prefix == 'device' else gate
    tlb = physical.T
    def verify(row):
        if 'previous_receipt' in row:
            previous = row['previous_receipt']; E.safe(previous['path'])
            E.require(E.sha(evidence / previous['path']) == previous['sha256'], 'previous replay receipt changed')
            original = read(evidence / previous['path'])
            excluded = {'harness_sha256', 'previous_receipt'}
            if mode == 'static' and row.get('derivation') == 'fresh-static-recheck':
                excluded.update(('derivation', 'result'))
                E.require(original['build_input_sha256'] == E.identity(E.manifest()),
                          'static recheck changed linked inputs')
            strip = lambda value: {k: v for k, v in value.items() if k not in excluded}
            E.require(strip(original) == strip(row), 'stage revision changed raw source/result facts')
        if mode == 'host':
            reused = row.get('derivation') == 'source-bound-host-replay'
            if reused:
                E.require(row['tested_sources'] and all(E.sha(E.ROOT / name) == digest
                          for name, digest in row['tested_sources'].items()), 'host tested harness changed')
                E.require(all(E.sha(evidence / name) == digest for name, digest in row['original_receipts'].items()),
                          'original hosted receipt changed')
            physical.verify_host(evidence, not reused)
            tlb.verify_host(evidence, not reused)
            return gate.verify_host(evidence)
        if mode == 'runtime':
            if prefix == 'device':
                result = gate.verify_runtime_recorded(evidence, row['result'])
            else:
                tlb.verify_runtime(evidence)
                result = [{'path': p.relative_to(evidence).as_posix(), 'result': physical.V.verify_run(p)}
                          for p in physical.selected_runs(evidence)]
            E.require(row['result'] == result, 'runtime receipt differs from complete native replay')
            return result
        if mode == 'production':
            index = read(evidence / 'production.json')
            candidate_role(read(evidence / 'candidates/production/candidate.json')['profile'],'production')
            candidate = gate.G.verify_local_candidate(evidence / 'candidates/production')
            E.require(index['status'] == 'PASS' and index['candidate_id'] == candidate and
                      len(set(index['runs'])) == 2, 'production selection/candidate incomplete')
            results = []
            for name in index['runs']:
                E.safe(name)
                E.require(read(evidence / name / 'result.json')['candidate_id'] == candidate,
                          'production run candidate differs')
                results.append(gate.V.verify_run(evidence / name))
            production_roles([read(evidence / name / 'result.json') for name in index['runs']],candidate)
            E.require(row['result'] == results, 'production semantic result differs')
            return results
        if mode == 'build':
            profiles = ('selftest', 'debug', 'production') + (('framebuffer-reference',) if prefix == 'device' else ())
            E.require(set(row['result']) == set(profiles), 'build role coverage')
            for profile in profiles:
                candidate_role(read(evidence / 'candidates' / profile / 'candidate.json')['profile'],profile)
            gate.module('memory-current-regression').canonical_builds(evidence)
            for profile in profiles:
                directory = candidates.resolve(evidence / 'candidates' / profile)
                candidate = gate.G.verify_local_candidate(directory)
                tlb.verify_codegen(directory, profile in ('debug', 'production'))
                fact = row['result'][profile]
                if prefix == 'device':
                    E.require(fact['candidate_id'] == candidate, 'build candidate identity differs')
                    if profile != 'framebuffer-reference':
                        E.require(fact['symbols_sha256'] == E.sha(directory / 'symbols.txt'), 'build symbols changed')
                else:
                    symbols = (directory / 'symbols.txt').read_text()
                    expected = {'physical_boot_adapt','physical_pointer_range','physical_address_range',
                                'bitmap_find_zero_run_below','paging_kernel_root_physical','paging_query_range'}
                    E.require(fact['elf_sha256'] == E.sha(directory / 'kernel.elf') and
                              fact['symbols_sha256'] == E.sha(directory / 'symbols.txt') and
                              set(fact['boundary_symbols']) == expected and
                              all(fact['boundary_symbols'][k] and k in symbols for k in expected),
                              'translation build enforcement differs')
                if profile in ('debug', 'production'):
                    gate.V.production((directory / 'symbols.txt').read_text(),
                                      (directory / 'compiled-strings.txt').read_text())
            return {'profiles': profiles}
        if mode == 'fixtures':
            relative = row['result']['path']; E.safe(relative)
            directory = evidence / relative
            E.require(E.sha(directory / 'results.json') == row['result']['sha256'], 'fixture raw receipt changed')
            name = 'device-mapping-fixtures' if prefix == 'device' else 'physical-translation-fixtures'
            gate.module(name).verify_receipt(read(directory / 'results.json'))
            if prefix == 'device':
                E.require(E.sha(directory / 'resumption.json') == row['result']['resumption_sha256'],
                          'resumption raw fixture binding differs')
                gate.module('validation-resumption-fixtures').verify_receipt(read(directory / 'resumption.json'))
                E.require(E.sha(directory / 'retention.json') == row['result']['retention_sha256'],
                          'retention fixture raw binding differs')
                gate.module('validation-retention-fixtures').verify_receipt(read(directory / 'retention.json'))
            if prefix == 'physical':
                E.require(E.sha(directory / 'physical-host.json') == row['result']['host_receipt_sha256'],
                          'fixture hosted dependency differs')
                physical.verify_host(directory, True)
            return {'status': 'PASS'}
        if mode == 'static':
            result = static_result(gate, evidence, prefix)
            E.require(row['result'] == result, 'static native receipt differs')
            return result
        raise ValueError('unknown native stage mode')
    receipt = (static_base(evidence) if mode=='static' else evidence) / (prefix + '-' + mode + '.json')
    if mode == 'host' and (evidence / (prefix + '-host-reuse.json')).exists():
        receipt = evidence / (prefix + '-host-reuse.json')
    revisions = sorted((evidence / 'stage-revisions' / (prefix + '-' + mode)).glob('*/complete.json'))
    if revisions: receipt = revisions[-1]
    if receipt_override is not None: receipt = receipt_override
    return R.classify(receipt, verify,
                      source=E.identity(E.manifest()),
                      harness=None if mode in ('build', 'production') else E.identity(E.manifest(harness=True)))


def revalidate_host_tools(gate, evidence, prefix):
    """Recheck current static facts and unchanged host/runtime captures explicitly."""
    for mode in ('static', 'host', 'runtime'):
        state = stage(gate, evidence, prefix, mode)
        if state['state'] != 'STALE': continue
        path = __import__('pathlib').Path(state['path']); row = read(path)
        E.require(row['build_input_sha256'] == E.identity(E.manifest()), 'revalidation cannot change linked inputs')
        if mode == 'host':
            E.require(row.get('derivation') == 'source-bound-host-replay', 'host revalidation needs tested source closure')
        directory = evidence / 'stage-revisions' / (prefix + '-' + mode) / R.stamp()
        directory.mkdir(parents=True)
        row = dict(row, harness_sha256=E.identity(E.manifest(harness=True)),
                   previous_receipt={'path': path.relative_to(evidence).as_posix(), 'sha256': E.sha(path)})
        if mode == 'static':
            row.update(derivation='fresh-static-recheck', result=static_result(gate, evidence, prefix))
        pending = directory / 'candidate.json'; E.write(pending, row)
        verified = stage(gate, evidence, prefix, mode, receipt_override=pending)
        E.write(directory / 'replay.json', verified)
        E.require(verified['state'] == 'REUSABLE', 'successor stage replay failed: ' + str(verified))
        pending.rename(directory / 'complete.json')


def series(gate, evidence, scope, candidate, validation):
    path = evidence / 'series.json'
    if not path.exists():
        return False
    try:
        value = gate.verifier().verify_series(evidence)
        E.require(value['scope'] == scope, 'resumed series scope differs')
        manifest = read(path)
        identity = gate.verify_local_candidate(candidate)
        for name in manifest['runs'] + manifest['diagnostics']:
            E.safe(name)
            directory = evidence / name
            E.require(read(directory / 'result.json')['candidate_id'] == identity, 'resumed series candidate differs')
            if validation is not None:
                validation(directory)
    except (ValueError, KeyError, OSError, AssertionError) as error:
        record = {'schema': 1, 'state': 'INVALID', 'path': str(path), 'sha256': E.sha(path),
                  'reason': str(error), 'action': 'Retain originals; collect a successor series in a new destination.'}
        (evidence / 'reuse-decisions').mkdir(exist_ok=True)
        E.write(evidence / 'reuse-decisions' / (R.stamp() + '.json'), record)
        raise ValueError('nonreusable series requires a new successor destination: ' + str(error)) from error
    return True
