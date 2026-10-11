#!/usr/bin/env python3
"""Explicit semantic controls for manifests, topology, aggregate policy and ledgers."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import memory_evidence as E
import memory_table_proof as T
import memory_current as C


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name + '.py'))
    value = importlib.util.module_from_spec(spec); sys.modules[spec.name] = value; spec.loader.exec_module(value)
    return value


V = module('verify-memory-observation')
R = module('memory-current-regression')


def command_path_fixtures():
    """Exercise the real portable matcher with empty and populated outputs."""
    from types import SimpleNamespace
    portable = module('memory-portable-replay')
    root = Path('/synthetic-extraction')
    original = Path('/synthetic-original/current/regression-attempts/selected')
    command = root / 'evidence/current-campaign/regression-attempts/selected/command-0.json'
    origin = str(original / command.name)
    command_name = command.relative_to(root).as_posix()
    field = 'HOBBYOS_STACK_NATIVE_AUDIT'
    transport = SimpleNamespace(root=root, content={command_name: {'local_path': origin}},
                                paths={origin: command})
    receipt = {'argv': ['native-validator', '--evidence', str(original / 'evidence')],
               'environment': {field: str(original / 'evidence/service-audits'), 'JOBS': '4'}}
    # The evidence directory itself has a selected file, while service-audits
    # deliberately does not. This is the failing canonical-builds geometry.
    transport.paths[str(original / 'evidence/result.json')] = command.parent / 'evidence/result.json'
    expected = (['native-validator', '--evidence', str(command.parent / 'evidence')],
                {field: str(command.parent / 'evidence/service-audits'), 'JOBS': '4'})

    def check(value):
        portable.regression_command_binding(value['transport'], command, value['receipt'],
                                            expected, 'canonical-builds')

    base = {'transport': transport, 'receipt': receipt}
    before = copy.deepcopy(base)
    check(base)
    populated = copy.deepcopy(base)
    populated['transport'].paths[str(original / 'evidence/service-audits/raw.jsonl')] = \
        command.parent / 'evidence/service-audits/raw.jsonl'
    check(populated)
    E.require(base == before, 'portable command matcher mutated original input')
    variants = [
        ('stack-audit-output-missing', 'output differs', lambda v: v['receipt']['environment'].pop(field)),
        ('stack-audit-output-wrong-attempt', 'output differs', lambda v: v['receipt']['environment'].update({field: str(original.parent / 'other/evidence/service-audits')})),
        ('stack-audit-output-parent', 'output differs', lambda v: v['receipt']['environment'].update({field: str(original / 'evidence')})),
        ('stack-audit-output-child', 'output differs', lambda v: v['receipt']['environment'].update({field: str(original / 'evidence/service-audits/extra')})),
        ('stack-audit-output-relative', 'output differs', lambda v: v['receipt']['environment'].update({field: 'evidence/service-audits'})),
        ('stack-audit-output-traversal', 'output differs', lambda v: v['receipt']['environment'].update({field: str(original / 'evidence') + '/other/../service-audits'})),
        ('stack-audit-origin-missing', 'origin differs', lambda v: v['transport'].content[command_name].pop('local_path')),
        ('stack-audit-origin-cross-command', 'origin differs', lambda v: v['transport'].paths.update({origin: command.with_name('command-1.json')})),
        ('stack-audit-origin-noncanonical', 'origin differs', lambda v: v['transport'].content[command_name].update(local_path=str(original) + '/../selected/command-0.json')),
        ('stack-command-extra-option', 'command/options differ', lambda v: v['receipt']['argv'].append('--unsafe-override')),
        ('stack-command-option-changed', 'command/options differ', lambda v: v['receipt']['environment'].update(JOBS='8')),
        ('stack-command-environment-pairs', 'command/options differ', lambda v: v['receipt'].update(environment=list(v['receipt']['environment'].items()))),
        ('stack-command-argv-tuple', 'command/options differ', lambda v: v['receipt'].update(argv=tuple(v['receipt']['argv']))),
    ]
    cases = []
    for name, reason, change in variants:
        value = copy.deepcopy(base); change(value)
        row = R.reject_case(name, reason, lambda value=value: check(value))
        row['boundary'] = 'Actual portable command matcher; exact selected origin and command options'
        cases.append(row)
    contract = os.environ.get('HOBBYOS_VALIDATION_CONTRACT')
    try:
        os.environ['HOBBYOS_VALIDATION_CONTRACT'] = 'kernel-image-protection'
        historical = copy.deepcopy(receipt); historical['environment'].pop(field)
        portable.regression_command_binding(transport, command, historical,
            (expected[0], {'JOBS': '4'}), 'canonical-builds')
    finally:
        if contract is None: os.environ.pop('HOBBYOS_VALIDATION_CONTRACT', None)
        else: os.environ['HOBBYOS_VALIDATION_CONTRACT'] = contract
    check(base)
    return {'schema': 1, 'status': 'PASS', 'positive': 4, 'negative': len(cases), 'cases': cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--replay', type=Path)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--series', type=Path)
    parser.add_argument('--production', type=Path)
    parser.add_argument('--portable-package', type=Path,
                        help='Replay the complete ledger through the explicit omitted-binary transport adapter.')
    args = parser.parse_args()
    E.require(bool(args.output) != bool(args.replay), 'choose output or replay')
    temporary = tempfile.TemporaryDirectory(prefix='memory-semantic-replay-') if args.replay else None
    if args.replay: args.output = Path(temporary.name) / 'fixtures'
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []

    def negative(name, reason, action, boundary):
        row = R.reject_case(name, reason, action)
        row['boundary'] = boundary
        cases.append(row)

    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'guarded-kernel-stacks':
        cases.extend(command_path_fixtures()['cases'])

    manifest = E.manifest()
    E.verify_manifest(manifest, E.identity(manifest))
    for name, change, reason in [
        ('manifest-row-changed', lambda m: m['files'][0].update(sha256='0' * 64), 'source export differs'),
        ('manifest-row-missing', lambda m: m['files'].pop(), 'missing or unlisted'),
        ('manifest-row-duplicated', lambda m: m['files'].insert(0, m['files'][0]), 'duplicate'),
        ('manifest-unsafe', lambda m: m['files'][0].update(path='../outside'), 'unsafe'),
    ]:
        m = copy.deepcopy(manifest); change(m)
        negative(name, reason, lambda value=m: E.verify_manifest(value, E.identity(value)), 'canonical manifest with recomputed synthetic digest')
    negative('manifest-digest-forged', 'source manifest digest mismatch', lambda: E.verify_manifest(manifest, '0' * 64), 'canonical digest')
    with tempfile.TemporaryDirectory(prefix='memory-source-export-') as temp:
        export = Path(temp)
        for row in manifest['files']:
            target = export / row['path']; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(E.ROOT / row['path'], target)
        changed = export / 'kernel/src/memory/pmem.c'
        changed.write_bytes(changed.read_bytes() + b'\n/* Synthetic export contradiction. */\n')
        negative('export-differs-from-tested-input', 'source export differs',
                 lambda: E.verify_manifest(manifest, E.identity(manifest), root=export),
                 'actual exported source bytes differ while the tested manifest and digest remain intact')
        changed.write_bytes((E.ROOT / 'kernel/src/memory/pmem.c').read_bytes())
        stack = os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'guarded-kernel-stacks'
        owned = stack or os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection')
        direct = stack or os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection')
        image = stack or os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'kernel-image-protection'
        low = image or os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'low-address-isolation'
        physical = os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'physical-translation'
        successor = stack or os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('tlb-coherency','physical-translation','owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection')
        revision_script = 'scripts/tlb-shootdown-gate.py' if successor else 'scripts/memory-portable-replay.py'
        contracts = ['scripts/memory-current-scope.json']
        if successor: contracts += ['scripts/cpu-control-scope.json','scripts/tlb-coherency-scope.json']
        if physical or owned:
            contracts.append('scripts/physical-translation-scope.json')
        if owned:
            contracts+=['scripts/device-mapping-scope.json','scripts/physical-conversion-sites.json']
            current = json.loads((E.ROOT / 'scripts/device-mapping-scope.json').read_text())
            contracts += list(current['historical_contracts']) + list(current['promoted_sources'])
        scope_file='scripts/stack-guard-scope.json' if stack else 'scripts/kernel-image-scope.json' if image else 'scripts/low-address-scope.json' if low else 'scripts/direct-map-scope.json'
        if direct:
            current = json.loads((E.ROOT / scope_file).read_text())
            contracts += [scope_file, *current['immutable_reviewed_anchors'],
                          *current['promoted_sources'], *current['protected_functions'],
                          *current['promoted_functions']]
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'cpu-control-state':
            contracts.append('scripts/cpu-control-scope.json')
        for name in (*contracts, revision_script, 'scripts/verify-lock-progress.py'):
            (export / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(E.ROOT / name, export / name)
        names = ('kernel/src/memory/pmem.c', revision_script, 'scripts/verify-lock-progress.py')
        anchors = export / 'captured.sha256'
        anchors.write_text(''.join(E.sha(export / name) + '  ' + name + '\n' for name in names))
        E.require(C.source_manifest(anchors, export) == [], 'current source anchor positive')
        script = export / names[1]; script.write_bytes(script.read_bytes() + b'\n# Synthetic harness revision.\n')
        if direct:
            # This labeled synthetic successor admits only the changed host
            # helper; linked and unrelated anchors remain the original bytes.
            current['promoted_sources'][names[1]]['candidate_sha256'] = E.sha(script)
            (export / scope_file).write_bytes(E.canonical(current))
        E.require(len(C.source_manifest(anchors, export)) == 1, 'authorized harness revision must remain explicit')
        changed.write_bytes(changed.read_bytes() + b'\n/* Synthetic linked change. */\n')
        protected_reason = ('RAM candidate anchor drift' if direct else 'device candidate anchor drift' if owned else 'physical candidate source drift' if physical else
                            'invalidation protected anchor drift' if successor else 'control protected anchor drift' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'cpu-control-state'
                            else 'current linked/protected source drift')
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='low-address-isolation':protected_reason='low-domain promoted source differs'
        if image:protected_reason='image protection protected file drift'
        if stack:protected_reason='stack protected file drift'
        negative('current-scope-linked-change', protected_reason,
                 lambda: C.source_manifest(anchors, export), 'actual current manifest validator; build input changed')
        changed.write_bytes((E.ROOT / names[0]).read_bytes())
        if successor or os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'cpu-control-state':
            linked = export / 'kernel/src/cpu/control_policy.c'
            with anchors.open('a') as stream:stream.write(E.sha(linked)+'  kernel/src/cpu/control_policy.c\n')
            linked.write_bytes(linked.read_bytes()+b'\n/* Synthetic linked control change. */\n')
            negative('current-scope-control-linked-change','stack protected file drift' if stack else 'image protection protected file drift' if image else 'low-domain protected file drift' if low else 'RAM protected file drift' if direct else 'device protected anchor drift' if owned else 'physical protected anchor drift' if physical else
                     'invalidation protected anchor drift' if successor else 'current linked/protected source drift',
                     lambda:C.source_manifest(anchors,export),'authorized path remains bound to tested build input bytes')
            linked.write_bytes((E.ROOT/'kernel/src/cpu/control_policy.c').read_bytes())
        unrelated = export / names[2]; unrelated.write_bytes(unrelated.read_bytes() + b'\n# Synthetic unrelated edit.\n')
        negative('current-scope-unrelated-change', 'stack protected file drift' if stack else 'image protection protected file drift' if image else 'low-domain protected file drift' if low else 'RAM protected file drift' if direct else 'current linked/protected source drift',
                 lambda: C.source_manifest(anchors, export), 'actual current manifest validator; unrelated oracle changed')

    def page(*entries):
        return struct.pack('<512Q', *(list(entries) + [0] * (512 - len(entries))))
    frames = {0x1000: page(0x2003, 0x2003), 0x2000: page(0x3003, 0x40000083),
              0x3000: page(0x4003, 0x200083), 0x4000: page(0x8003)}
    E.require(set(T.traverse(0x1000, frames, 36)) == set(frames), 'shared parent/huge leaf positive topology')
    variants = [
        ('omitted-root', lambda f: f.pop(0x1000), 'missing or malformed child'),
        ('missing-child', lambda f: f.pop(0x4000), 'missing or malformed child'),
        ('missing-table-bytes', lambda f: f.update({0x4000: bytes(4088)}), 'missing dump bytes'),
        ('topology-cycle', lambda f: f.update({0x2000: page(0x1003)}), 'cycle or inconsistent'),
        ('inconsistent-alias-level', lambda f: f.update({0x1000: page(0x2003, 0x3003), 0x3000: page()}), 'cycle or inconsistent'),
        ('malformed-child-address', lambda f: f.update({0x1000: page((1 << 40) | 0x2003)}), 'reserved child address'),
        ('huge-leaf-misalignment', lambda f: f.update({0x3000: page(0x2083)}), 'malformed huge leaf'),
        ('root-huge-link', lambda f: f.update({0x1000: page(0x2083)}), 'malformed root huge link'),
    ]
    for name, mutate, reason in variants:
        f = dict(frames); mutate(f)
        negative(name, reason, lambda value=f: T.traverse(0x1000, value, 36), 'actual portable topology traversal')
    original = V.verify_run(args.capture / 'qemu-serial.log')
    meta = R.read(args.capture / 'table-snapshot.json')
    observation = original['tables']
    with tempfile.TemporaryDirectory(prefix='memory-topology-fixtures-') as temp:
        target = Path(temp)
        shutil.copyfile(args.capture / 'table-frames.bin', target / 'table-frames.bin')
        def check(m, obs):
            (target / 'table-snapshot.json').write_text(json.dumps(m))
            return T.replay(target, original['candidate_id'], original['vm_id'], obs)
        check(meta, observation)
        for name, change, reason in [
            ('reachable-underreported', lambda m, o: o.update(reachable='1'), 'enumeration length'),
            ('reachable-overreported', lambda m, o: o.update(reachable=str(int(o['reachable']) + 1)), 'enumeration length'),
            ('duplicate-guest-table', lambda m, o: m.update(guest_reachable_hex=m['guest_reachable_hex'][:16] + m['guest_reachable_hex'][:16] + m['guest_reachable_hex'][32:]), 'reachable frame set/count'),
            ('huge-data-counted-table', lambda m, o: m.update(guest_reachable_hex=struct.pack('<Q', 0x80000000).hex() + m['guest_reachable_hex'][16:]), 'reachable frame set/count'),
            ('duplicate-dump-frame', lambda m, o: m['frame_addresses'].__setitem__(1, m['frame_addresses'][0]), 'duplicate frames'),
            ('mixed-topology-epoch', lambda m, o: m.update(epoch_after=m['epoch_after'] + 1), 'mixed epochs'),
            ('mixed-topology-candidate', lambda m, o: m.update(candidate_id='0' * 64), 'mixed candidate'),
        ]:
            m, o = copy.deepcopy(meta), copy.deepcopy(observation); change(m, o)
            negative(name, reason, lambda m=m, o=o: check(m, o), 'actual snapshot reconciliation; synthetic metadata consistent at entry boundary')

    lines = (args.capture / 'qemu-serial.log').read_text().splitlines()
    framebuffer_origin = 'origin=owned_BootInfo_framebuffer' if owned else 'origin=BootInfo_framebuffer'
    mmio_origin = 'origin=owned_madt_ioapic_base' if owned else 'origin=madt_ioapic_base'
    for name, old, new, reason in [
        ('framebuffer-mislabeled', framebuffer_origin, mmio_origin, 'framebuffer representative'),
        ('mmio-mislabeled', mmio_origin, framebuffer_origin, 'MMIO representative origin'),
    ]:
        text = '\n'.join(lines); E.require(old in text, 'fixture representative absent')
        negative(name, reason, lambda text=text.replace(old, new, 1): V.verify_representatives(text.splitlines()), 'actual representative semantic validator')

    attempts = []
    for profile, count in V.MATRIX.items():
        for repeat in range(count):
            i = len(attempts) + 1
            validated = {'profile': profile, 'candidate_id': 'candidate', 'trace': False, 'readback': False,
                         'launch_id': 'launch-' + str(i), 'vm_id': 'vm-' + str(i), 'boot_id': 'boot-' + str(i),
                         'process_identity': ['host-boot', i, i], 'firmware': ['firmware'], 'qemu_version': 'version',
                         'path': 'profiles/' + str(i), 'timing': {'duration_ns': i * 100},
                         'started_ns': i * 10 + 1, 'finished_ns': i * 10 + 8}
            attempts.append({'sequence': i, 'path': validated['path'], 'profile': profile, 'candidate_id': 'candidate',
                             'started_ns': i * 10, 'finished_ns': i * 10 + 9, 'status': 'PASS', 'validated': validated})
    selected = V.series_policy(attempts, 'full-matrix')
    E.require(len(selected['runs']) == sum(V.MATRIX.values()), 'aggregate positive')
    for name, change, reason in [
        ('same-run-three-times', lambda a: a[1].update(path=a[0]['path']), 'duplicate canonical'),
        ('renamed-boot-copy', lambda a: a[1]['validated'].update(vm_id=a[0]['validated']['vm_id']), 'duplicate independent VM'),
        ('copied-launch', lambda a: a[1]['validated'].update(launch_id=a[0]['validated']['launch_id']), 'duplicate independent launch'),
        ('copied-boot', lambda a: a[1]['validated'].update(boot_id=a[0]['validated']['boot_id']), 'duplicate independent boot'),
        ('hidden-failure', lambda a: a[1].update(status='FAIL'), 'required consecutive'),
        ('missing-sequence', lambda a: a.pop(1), 'missing/duplicate attempt'),
        ('duplicate-sequence', lambda a: a[1].update(sequence=1), 'missing/duplicate attempt'),
        ('wrong-profile', lambda a: a[1]['validated'].update(profile='q35-kvm-smp4'), 'attempt profile/candidate'),
        ('mixed-candidate', lambda a: a[1]['validated'].update(candidate_id='other'), 'attempt profile/candidate'),
        ('traced-baseline', lambda a: a[4]['validated'].update(trace=True), 'diagnostic run cannot'),
        ('readback-baseline', lambda a: a[4]['validated'].update(readback=True), 'diagnostic run cannot'),
        ('inconsistent-chronology', lambda a: a[1].update(started_ns=1), 'inconsistent attempt chronology'),
        ('missing-profile', lambda a: a.pop(), 'required consecutive'),
    ]:
        a = copy.deepcopy(attempts); change(a)
        negative(name, reason, lambda a=a: V.series_policy(a, 'full-matrix'), 'aggregate policy only; explicitly mocked already-validated run facts')

    fixed = R.inventory()
    ledger = {'schema': 1, 'inventory_sha256': E.sha(R.REQUIREMENTS), 'historical_unavailable': fixed['historical_unavailable'],
              'rows': [dict(key=r['key'], requirement=r, status='PASS', exit_status=0, build_input_sha256='source',
                            raw=[{'path': 'raw.log'}], commands=['command.json'], replay=[['validator']]) for r in fixed['requirements']]}
    R.ledger_policy(ledger, 'source')
    prior_replay = dict(copy.deepcopy(ledger), replay_harness_sha256='0' * 64)
    next_replay = dict(copy.deepcopy(ledger), replay_harness_sha256='1' * 64)
    R.verify_ledger_revision(prior_replay, next_replay)
    for name, change in [
        ('ledger-revision-omits-family', lambda value: value['rows'].pop()),
        ('ledger-revision-changes-candidate', lambda value: value['rows'][0].update(build_input_sha256='other')),
        ('ledger-revision-changes-raw', lambda value: value['rows'][0].update(raw=['different-raw-proof'])),
    ]:
        value = copy.deepcopy(next_replay); change(value)
        negative(name, 'ledger revision changed existing evidence or requirements',
                 lambda value=value: R.verify_ledger_revision(prior_replay, value),
                 'actual ledger revision policy; synthetic already-validated rows, no boot repetitions')
    for name, change, reason in [
        ('empty-ledger', lambda l: l.update(rows=[]), 'incomplete regression'),
        ('one-row-pass-ledger', lambda l: l.update(rows=l['rows'][:1]), 'incomplete regression'),
        ('removed-requirement', lambda l: l['rows'].pop(), 'incomplete regression'),
        ('duplicate-masks-omission', lambda l: l['rows'].__setitem__(1, l['rows'][0]), 'duplicate regression'),
        ('wrong-ledger-profile', lambda l: l['rows'][0]['requirement'].update(profiles=['host']), 'wrong regression mode/profile'),
        ('omitted-destructive-layout-role', lambda l: next(r for r in l['rows'] if r['key'] == 'section-layout')['requirement']['profiles'].remove('architecture-negative'), 'wrong regression mode/profile'),
        ('host-only-substitution', lambda l: l['rows'][1]['requirement'].update(mode='host'), 'wrong regression mode/profile'),
        ('changed-ledger-candidate', lambda l: l['rows'][0].update(build_input_sha256='other'), 'stale regression candidate'),
        ('missing-raw-output', lambda l: l['rows'][0].update(raw=[]), 'missing raw regression proof'),
        ('nonzero-hidden-pass', lambda l: l['rows'][0].update(exit_status=1), 'nonzero/unavailable'),
        ('historical-unavailable-counted-pass', lambda l: l['rows'][0].update(status='HISTORICAL_REPLAY_UNAVAILABLE'), 'nonzero/unavailable'),
    ]:
        value = copy.deepcopy(ledger); change(value)
        negative(name, reason, lambda value=value: R.ledger_policy(value, 'source'), 'ledger inventory policy only; raw replay tested separately')
    current_format = next(r for r in fixed['requirements'] if r['key'] == 'formatting')
    earlier_format = copy.deepcopy(current_format)
    earlier_format['profiles'] = [p.replace('modal-q35-kvm-smp4:2', 'modal-q35-kvm-smp4:3') for p in earlier_format['profiles']]
    E.require(R.requirement_binding('formatting', {'requirement': earlier_format}, {'requirement': earlier_format})['status'] ==
              'CORRECTED_DESCRIPTION', 'explicit inventory correction positive')
    wrong = copy.deepcopy(earlier_format); wrong['profiles'] = ['host']
    negative('unapproved-capture-requirement-revision', 'unapproved regression requirement revision',
             lambda: R.requirement_binding('formatting', {'requirement': wrong}, {'requirement': wrong}),
             'actual requirement binding; policy-only metadata fixture')
    negative('captured-requirement-contradiction', 'regression captured requirement changed',
             lambda: R.requirement_binding('formatting', {'requirement': current_format}, {'requirement': earlier_format}),
             'actual requirement binding; start/result contradiction')
    if args.series:
        replayed_series = V.verify_series(args.series)
        G = module('memory-observation-gate')
        G.verify_static(args.series)
        G.verify_baseline(args.series, replayed_series)
        with tempfile.TemporaryDirectory(prefix='memory-summary-synthetic-') as tmp:
            target = Path(tmp)
            for name in ('static.json', 'inventory.json', 'baseline.json'):
                shutil.copyfile(args.series / name, target / name)
            for receipt in (args.series / 'attempts').glob('*/command.json'):
                if receipt.parent.name.split('Z-', 1)[-1] in ('diff-check', 'shell-syntax', 'inventory'):
                    shutil.copytree(receipt.parent, target / 'attempts' / receipt.parent.name)
            G.verify_static(target)
            saved = R.read(target / 'static.json'); wrong = copy.deepcopy(saved); wrong['status'] = 'FAIL'
            (target / 'static.json').write_bytes(E.canonical(wrong))
            negative('reused-static-receipt-contradiction', 'static invariant receipt differs',
                     lambda: G.verify_static(target), 'actual reused-mode verifier; independent current source invariants')
            (target / 'static.json').write_bytes(E.canonical(saved))
            wrong = R.read(target / 'inventory.json'); wrong['counts']['paging_map'] -= 1
            (target / 'inventory.json').write_bytes(E.canonical(wrong))
            negative('reused-inventory-undercount', 'current source inventory differs',
                     lambda: G.verify_static(target), 'actual source scanner compared with synthetic summary')
            wrong = copy.deepcopy(replayed_series); wrong['baseline']['q35-tcg-smp4']['median_ns'] += 1
            (target / 'baseline.json').write_bytes(E.canonical(wrong))
            negative('reused-baseline-summary-contradiction', 'baseline receipt differs from replayed series',
                     lambda: G.verify_baseline(target, replayed_series), 'actual reused-mode validator; series independently replayed first')
        with tempfile.TemporaryDirectory(prefix='memory-series-synthetic-') as tmp:
            target = Path(tmp) / 'series'; target.mkdir()
            series_manifest = R.read(args.series / 'series.json')
            for group in ('campaign-attempts', 'profiles', 'diagnostics'):
                shutil.copytree(args.series / group, target / group, ignore=shutil.ignore_patterns('input-layout'))
            (target / 'series.json').write_text(json.dumps(series_manifest))
            if image:
                # A stopped collector's seal includes the exact host helper.
                # Ordinary successful captures do not consume that executable.
                for name in series_manifest['attempts']:
                    start = R.read(args.series / name)
                    finish = R.read((args.series / name).with_suffix('.result.json'))
                    if 'interruption' not in finish: continue
                    original = args.series / start['path']; copied = target / start['path']
                    seal = R.read(original / finish['interruption']['path'])
                    for item in seal['files']:
                        E.safe(item['path'])
                        source = original / item['path']; destination = copied / item['path']
                        if destination.exists(): continue
                        E.require(item['path'] == 'input-layout' and source.is_file() and
                            not source.is_symlink() and source.stat().st_size == item['size'] <= 65536 and
                            E.sha(source) == item['sha256'], 'interrupted fixture required helper differs')
                        shutil.copy2(source, destination)
                    helper = copied / 'input-layout'; saved_helper = helper.read_bytes()
                    helper.unlink()
                    try:
                        negative('interrupted-raw-closure-missing', 'interruption raw dependency changed',
                            lambda: V.attempt_clock(copied, start, finish),
                            'actual interruption matcher; required sealed host helper omitted from a synthetic copy')
                    finally:
                        helper.write_bytes(saved_helper); shutil.copymode(original / 'input-layout', helper)
            V.verify_series(target)
            wrong = copy.deepcopy(series_manifest); wrong['baseline']['q35-tcg-smp4']['median_ns'] += 1
            negative('incorrect-baseline-median', 'series selection or baseline median/samples differs',
                     lambda: V.verify_series(target, wrong), 'complete offline replay of copied real campaign')
            wrong = copy.deepcopy(series_manifest); wrong['attempts'].pop()
            negative('omitted-campaign-attempt', 'missing/unlisted campaign attempts',
                     lambda: V.verify_series(target, wrong), 'complete offline replay of copied real campaign')
            successful = {}
            for name in series_manifest['attempts']:
                start_path = target / name
                start = R.read(start_path)
                if R.read(start_path.with_suffix('.result.json'))['status'] == 'PASS':
                    successful.setdefault(start['profile'], []).append(start_path)
            pair = next((paths[:2] for paths in successful.values() if len(paths) >= 2), None)
            E.require(pair, 'duplicate-boot fixture requires two verified successful boots of one profile')
            first = R.read(pair[0]); second_path = pair[1]
            second = R.read(second_path); saved = copy.deepcopy(second)
            second['path'] = first['path']; second_path.write_bytes(E.canonical(second))
            finish_path = second_path.with_suffix('.result.json'); finish = R.read(finish_path)
            finish['start_sha256'] = E.sha(second_path); finish_path.write_bytes(E.canonical(finish))
            negative('same-real-run-path', 'duplicate/unsafe canonical run path', lambda: V.verify_series(target),
                     'complete verifier; real saved launch and VM receipts')
            second = saved; second_path.write_bytes(E.canonical(second))
            finish['start_sha256'] = E.sha(second_path)
            saved_second = target.parent / 'second-original'
            if image: (target / second['path']).rename(saved_second)
            else: shutil.rmtree(target / second['path'])
            shutil.copytree(target / first['path'], target / second['path'])
            finish['result_sha256'] = E.sha(target / second['path'] / 'result.json')
            finish_path.write_bytes(E.canonical(finish))
            negative('renamed-real-boot-alias', 'attempt host clock binding' if image else
                     'duplicate independent launch identity', lambda: V.verify_series(target),
                     'complete verifier; genuine receipt copied under a different directory; current host clock binding rejects the old launch time'
                     if image else 'complete verifier; genuine receipt copied under a different directory')
            if image:
                # Keep the real second boot's clocks and raw data, and give it
                # only the first launch identity to reach the aggregate check.
                second_run = target / second['path']; shutil.rmtree(second_run)
                saved_second.rename(second_run)
                identity = R.read(target / first['path'] / 'launch.json')['launch_id']
                originals = {name: (second_run / name).read_bytes() for name in
                             ('launch.json', 'boot-receipt.json', 'result.json')}
                for name, raw in originals.items():
                    row = json.loads(raw); row['launch_id'] = identity
                    (second_run / name).write_bytes(E.canonical(row))
                finish['result_sha256'] = E.sha(second_run / 'result.json')
                finish_path.write_bytes(E.canonical(finish))
                negative('reused-real-launch-identity', 'duplicate independent launch identity',
                    lambda: V.verify_series(target),
                    'complete verifier; only the three bound launch IDs and outer result digest are changed in a synthetic copy')
                for name, raw in originals.items(): (second_run / name).write_bytes(raw)
                finish['result_sha256'] = E.sha(second_run / 'result.json')
                finish_path.write_bytes(E.canonical(finish))
                V.verify_series(target)
            (target / 'series.json').unlink()
            try: V.verify_series(target)
            except FileNotFoundError:
                cases.append({'name': 'missing-series', 'synthetic': True, 'expected_exit': 1, 'actual_exit': 1,
                              'expected_boundary': 'required series.json', 'reason': 'series.json absent',
                              'boundary': 'complete aggregate entry point'})
            else: raise ValueError('missing series accepted')
    if args.production:
        V.verify_run(args.production / 'qemu-serial.log')
        for name, token in [('compiled-command', 'ptinspect'), ('compiled-hook', 'cpu_control_snapshot_irq'),
                            ('compiled-marker', '[CPU_CONTROL]')]:
            with tempfile.TemporaryDirectory(prefix='memory-production-negative-') as tmp:
                target = Path(tmp) / name
                shutil.copytree(args.production, target, ignore=shutil.ignore_patterns('input-layout'))
                strings = target / 'compiled-strings.txt'; strings.write_text(strings.read_text() + '\n' + token + '\n')
                absence = R.read(target / 'absence.json'); absence['compiled_strings_sha256'] = E.sha(strings)
                (target / 'absence.json').write_bytes(E.canonical(absence))
                negative(name, 'production compiled diagnostic surface present', lambda: V.verify_run(target / 'qemu-serial.log'),
                         'complete offline verifier; synthetic compiled facts and their receipt hash consistently recomputed')
    if args.series and (args.series / 'regression-ledger.json').is_file():
        # The real complete ledger is replayed first. Each synthetic case then
        # isolates an earlier raw-evidence boundary in the same public verifier;
        # later families are deliberately not materialized or counted as runs.
        if args.portable_package:
            portable = module('memory-portable-replay')
            transport = portable.Transport(args.portable_package)
            portable.regression_ledger(args.series, transport)
            portable.selected_native(args.series, transport)
        else: R.verify_ledger(args.series, E.identity(manifest))
        original_ledger = R.read(args.series / 'regression-ledger.json')
        for name, reason in [('ledger-raw-missing', 'missing or unlisted raw regression output'),
                             ('ledger-command-nonzero', 'failed/stale regression command receipt'),
                             ('ledger-command-stale', 'failed/stale regression command receipt')]:
            with tempfile.TemporaryDirectory(prefix='memory-ledger-negative-') as temp:
                target = Path(temp); value = copy.deepcopy(original_ledger); row = value['rows'][0]
                attempt = target / row['attempt']
                shutil.copytree(args.series / row['attempt'], attempt)
                command = target / row['commands'][0]
                if name == 'ledger-raw-missing': command.with_suffix('.stdout.log').unlink()
                else:
                    record = R.read(command)
                    if name == 'ledger-command-nonzero': record['exit_status'] = 1
                    else: record['build_input_sha256'] = '0' * 64
                    command.write_bytes(E.canonical(record))
                    for item in row['raw']:
                        if item['path'] == row['commands'][0]:
                            item.update(size=command.stat().st_size, sha256=E.sha(command))
                (target / 'regression-ledger.json').write_bytes(E.canonical(value))
                negative(name, reason, lambda: R.verify_ledger(target, E.identity(manifest)),
                         'public ledger verifier; first raw row isolated after complete positive replay; outer fixture hashes recomputed')
    result = {'schema': 1, 'status': 'PASS', 'negative': len(cases), 'cases': cases,
              'capture_sha256': E.sha(args.capture / 'result.json'), 'verifier_sha256': E.sha(Path(__file__).with_name('verify-memory-observation.py'))}
    if args.replay:
        E.require(R.read(args.replay) == result, 'semantic recipe replay differs')
        temporary.cleanup()
    else: E.write(args.output / 'result.json', result)
    print(json.dumps({'status': 'PASS', 'negative': len(cases), 'kind': 'synthetic-semantic-fixtures'}))


if __name__ == '__main__': main()
