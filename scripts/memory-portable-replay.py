#!/usr/bin/env python3
"""Replay selected raw evidence without a Git checkout, VM, or full kernel binary.

Local acceptance uses the complete native gates. Transport replay explicitly
substitutes saved binary identities and bounded structural fragments only where
the full binaries were intentionally omitted; runtime validators still consume
their original raw records. This establishes consistency, not run authenticity.
"""
import argparse
import contextlib
import functools
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import memory_evidence as E
import memory_elf_proof as ELF

ROOT = Path(__file__).resolve().parent.parent


def module(name):
    spec = importlib.util.spec_from_file_location('portable_' + name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    result = importlib.util.module_from_spec(spec); sys.modules[spec.name] = result; spec.loader.exec_module(result)
    return result


def read(path): return json.loads(path.read_text())


class Transport:
    def __init__(self, root):
        self.root = root.resolve(); self.index = read(root / 'content-selection.json')
        if self.index.get('schema') == 2:
            from review_transport import index
            self.index = index(root)
        self.omitted = {}
        self.paths = {}
        self.content = {}
        paths = set()
        for group in ('raw', 'local_binaries', 'synthetic_deltas'):
            for row in self.index[group]:
                name = row['path']; E.safe(name)
                self.content[name] = row
                if 'local_path' in row:
                    self.paths[row['local_path']] = root / name
                    marker = '/backups/'
                    if marker in row['local_path']:
                        self.paths['backups/' + row['local_path'].split(marker, 1)[1]] = root / name
                E.require(name not in paths, 'duplicate portable content path'); paths.add(name)
                if group == 'local_binaries':
                    E.require(not (root / name).exists(), 'omitted binary unexpectedly present')
                    E.require(Path(name).suffix.lower() not in ('.json', '.jsonl', '.log', '.txt', '.tsv', '.csv', '.env',
                                                               '.gdb', '.cmd', '.bin', '.ppm', '.c', '.h', '.py', '.sh', '.md'),
                              'raw proof cannot be reclassified as an omitted binary')
                    self.omitted[name] = row
                    E.require(row['facts']['sha256'] == row['sha256'] and row['facts']['size'] == row['size'], 'binary fact identity differs')
                else:
                    name = row.get('blob', name); E.safe(name); path = root / name
                    E.require(path.is_file() and path.stat().st_size == row['size'] and E.sha(path) == row['sha256'], 'portable raw/delta bytes differ: ' + name)
        for row in self.omitted.values():
            facts = row['facts']
            if 'layout_proof' in facts:
                proof = read(root / facts['layout_proof'])
                E.require(proof['elf_sha256'] == row['sha256'] and proof['elf_size'] == row['size'], 'ELF fragment candidate binding')
                ELF.replay(proof)
            for name in ('symbols', 'strings', 'headers', 'undefined', 'segments'):
                if name in facts: E.require(E.sha(root / facts[name]['path']) == facts[name]['sha256'], 'saved ELF tool fact hash')

    def binary(self, path, expected=None, size=None):
        name = path.resolve().relative_to(self.root).as_posix()
        E.require(name in self.omitted, 'binary omission is not declared: ' + name)
        row = self.omitted[name]
        E.require((expected is None or row['sha256'] == expected) and (size is None or row['size'] == size), 'saved binary receipt contradicts candidate: ' + name)
        return row['sha256']

    def facts(self, path, kind):
        row = self.omitted[path.resolve().relative_to(self.root).as_posix()]
        return (self.root / row['facts'][kind]['path']).read_text()

    def verify_source_identity(self):
        source = read(self.root / 'source-manifest.json')
        rows = source['files']
        E.require(len(rows) == len({r['path'] for r in rows}), 'portable source duplicates')
        for row in rows:
            E.safe(row['path'])
            path = ROOT / row['path']
            E.require(path.is_file() and path.stat().st_size == row['size'] and E.sha(path) == row['sha256'] and
                      ('100755' if path.stat().st_mode & 0o111 else '100644') == row['mode'], 'portable exported source differs')
        E.require({r['path'] for r in rows} == {p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*')
                  if p.is_file() and p.suffix not in E.GENERATED_SUFFIXES}, 'portable source inventory incomplete')
        scope = module('memory_current').current_contract(ROOT)
        for name, digest in scope['immutable_reviewed_anchors'].items():
            E.require(E.sha(ROOT / name) == digest, 'portable protected source changed')

    def source_manifest(self, path):
        return module('memory_current').source_manifest(path, ROOT)

    def elf_tool(self, digest, kind):
        rows = [r for r in self.omitted.values() if r['sha256'] == digest and kind in r['facts']]
        E.require(rows, 'missing saved ELF tool proof: ' + kind)
        hashes = {r['facts'][kind]['sha256'] for r in rows}
        E.require(len(hashes) == 1, 'contradictory ELF tool proof')
        return (self.root / rows[0]['facts'][kind]['path']).read_text()

    def mapped(self, name):
        E.require(name in self.paths, 'unlisted original evidence path: ' + name)
        return self.paths[name]

    def elf(self, path, expected=None):
        self.binary(path, expected)
        facts = self.omitted[path.resolve().relative_to(self.root).as_posix()]['facts']
        E.require('layout_proof' in facts, 'required bounded ELF proof missing')
        return ELF.replay(read(self.root / facts['layout_proof']))

    def json(self, path):
        if path.is_file(): return read(path)
        name = path.relative_to(self.root).as_posix()
        row = self.content[name]
        E.require(row['transport'] == 'SYNTHETIC_FILE_DELTA', 'missing raw JSON proof')
        return read(self.root / row['blob'])

    def foundation(self):
        validator = module('verify-foundation-regression')
        native = validator.input_address_map_errors
        def address_map(row, directory, symbols):
            path = directory.parent.parent / 'candidate/kernel.elf'
            digest = self.binary(path, row['candidate_sha256'])
            return native(row, directory, symbols,
                          elf_facts=(digest, self.facts(path, 'symbols'), self.facts(path, 'segments')))
        validator.input_address_map_errors = address_map
        return validator

    def replace_binary_error(self, errors, message, path, expected, size=None):
        self.binary(path, expected, size)
        E.require(errors.count(message) == 1, 'portable substitution boundary differs: ' + message)
        errors.remove(message)

    def materialize_deltas(self, destination, prefix=None):
        for row in self.index['synthetic_deltas']:
            if prefix is not None and not row['path'].startswith(prefix.rstrip('/') + '/'): continue
            target = destination / row['path']; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.root / row['blob'], target); os.chmod(target, row['os_permissions'])
            E.require(E.sha(target) == row['sha256'], 'materialized synthetic bytes differ')


def formatting(root, transport):
    V = module('verify-libc-format-evidence')
    result = read(root / 'result.json')
    errors = V.verify_evidence(root)
    manifest = read(root / result['candidate_manifest'])
    for group, values in (('candidate', manifest['files']), ('payload', manifest['payloads'])):
        for name, row in values.items():
            path = root / row['path']
            if not path.exists(): transport.replace_binary_error(errors, f'{group} receipt differs: {name}', path, row['sha256'], row['bytes'])
    for row in result['host_runs']:
        path = root / row['directory'] / 'libc-format-host'
        transport.replace_binary_error(errors, f"host {row['name']} executable identity differs", path, row['binary_sha256'])
    for row in result['qemu_profiles']:
        transport.replace_binary_error(errors, f"QEMU {row['name']} final run image differs", root / row['run_image_path'], row['run_image_final_sha256'])
    consumer = read(root / result['consumer_result'])
    for group in ('candidate', 'payloads'):
        for name, row in consumer[group].items():
            path = root / row['path']
            if not path.exists():
                label = 'candidate' if group == 'candidate' else 'payload'
                transport.replace_binary_error(errors, f'consumer {label} {name} receipt differs', path, row['sha256'], row['bytes'])
    for directory in [*(root / 'consumers/modal-heap').iterdir(), *(root / 'consumers/visual').iterdir()]:
        profile = read(directory / 'profile.json')
        transport.replace_binary_error(errors, f"consumer {profile['name']} final run image differs", root / profile['run_image_path'], profile['run_image_final_sha256'])
    E.require(not errors, 'native formatting replay: ' + '; '.join(errors))
    return {'native_entry': 'verify_evidence', 'modal_profiles': consumer['modal_heap_profiles']}


def assertion_runtime(root, transport):
    V = module('verify-assert-list-evidence'); campaign = read(root / 'campaign.json')
    candidate = campaign['candidate']
    transport.binary(root / candidate['elf_path'], candidate['elf_sha256'])
    transport.binary(root / candidate['image_path'], candidate['image_sha256'])
    V.verify_build(root, candidate['elf_sha256'], candidate['image_sha256']); V.verify_host(root)
    names = [r['name'] for r in campaign['planned_runs']]
    E.require(len(names) == 25 and len(set(names)) == 25 and set(names) == {p.name for p in (root / 'runtime').iterdir()}, 'assertion matrix coverage')
    native_artifacts = V.verify_artifacts
    def captured_artifacts(directory, result):
        # Partition only the binary-file identity check. All mandatory raw files
        # remain with the unchanged native validator and its exact parser.
        retained = {}
        for name, receipt in result['artifacts'].items():
            path = directory / name
            if path.exists(): retained[name] = receipt
            else: transport.binary(path, receipt['sha256'], receipt['bytes'])
        native_artifacts(directory, dict(result, artifacts=retained))
    V.verify_artifacts = captured_artifacts
    for row in campaign['planned_runs']: V.verify_run(root / 'runtime' / row['name'], row, candidate['elf_sha256'])
    production = read(root / 'production/result.json')
    for role, field in (('debug-on', 'debug_on_sha256'), ('debug-off', 'debug_off_sha256'), ('final', 'final_sha256')):
        transport.binary(root / 'production' / role / 'kernel.elf', production[field])
    E.require(production['debug_on_sha256'] != production['debug_off_sha256'] == production['final_sha256'], 'assertion production pair identity')
    off = root / 'production/debug-off/kernel.elf'; on = root / 'production/debug-on/kernel.elf'
    off_symbols = transport.facts(off, 'symbols'); off_strings = transport.facts(off, 'strings')
    on_symbols = transport.facts(on, 'symbols'); on_strings = transport.facts(on, 'strings')
    for name in ('assertion_warn_report', 'assertion_bug_report'):
        E.require(name in on_symbols and name not in off_symbols, 'assertion compiled helper isolation')
    for name in ('assertion_test_timer_hook', 'g_assertion_test_state', 'g_assertion_test_fixture'):
        E.require(name not in off_symbols, 'assertion production test helper present')
    for name in ('new_node == NULL', 'prev->next != next', '[ASSERT][WARN]', '[ASSERT][BUG]'):
        E.require(name not in off_strings, 'assertion production callsite text')
    E.require(any(x in on_strings for x in ('[ASSERT][WARN]', '[ASSERT][BUG]')), 'debug assertion strings absent')
    sections = production['section_comparison']
    on_elf = transport.elf(on); off_elf = transport.elf(off)
    # The native producer's size-A contract includes bootstrap storage even
    # though this linker represents .boot_data without SHF_ALLOC.
    totals = [sum(s['size'] for s in elf.sections if s['name'].startswith('.') and
                  not s['name'].startswith(('.debug', '.comment', '.symtab', '.strtab', '.shstrtab')))
              for elf in (on_elf, off_elf)]
    E.require(sections['status'] == 'PASS' and sections['debug_on_allocated_bytes'] == totals[0] >
              sections['debug_off_allocated_bytes'] == totals[1], 'assertion production section accounting differs')
    final = root / 'production/final'
    for name in ('hobbyos.img', 'BOOTX64.EFI'):
        transport.binary(final / name, production['final_artifacts'][name]['sha256'])
    E.require(set(production['payloads']) == {'kernel.elf', 'BOOTX64.EFI', 'zap-light16.psf', 'logo.bmp', 'startup.nsh'}, 'assertion production payload coverage')
    for name, receipt in production['payloads'].items():
        path = final / 'payloads' / name
        if path.exists(): E.require(E.sha(path) == receipt['sha256'], 'assertion production asset differs')
        else: transport.binary(path, receipt['sha256'])
    E.require(production['payloads']['kernel.elf']['sha256'] == production['final_sha256'], 'assertion production image kernel differs')
    native_file = V.require_file; native_hash = V.sha256
    V.require_file = lambda path, expected_hash=None: (native_file(path, expected_hash) if path.exists()
                                                      else transport.binary(path, expected_hash))
    V.sha256 = lambda path: native_hash(path) if path.exists() else transport.binary(path)
    # Only binary byte/symbol operations above use saved measured facts. The
    # complete native scenario/profile/host/runtime aggregation runs unchanged.
    V.verify_production = lambda directory: None
    matrix = V.verify_all(root)
    return {'native_entries': ['verify_all', 'verify_build', 'verify_host', 'verify_run'], 'runs': 25, 'matrix': matrix}


def statistics_linker(root, base_commit):
    import low_current
    import kernel_image_current
    import stack_current
    if kernel_image_current.enabled():low_current=kernel_image_current
    if stack_current.enabled():low_current=stack_current
    linker = root / 'source/kernel/link.ld'
    baseline = root / 'static/base-link.ld'
    if low_current.enabled():
        promotion = low_current.contract()['promoted_sources']['kernel/link.ld']
        E.require(base_commit == low_current.BASE and
                  read(root / 'static/linker-successor.json') == promotion and
                  E.sha(baseline) == promotion['baseline_sha256'] and
                  E.sha(linker) == promotion['candidate_sha256'] == E.sha(ROOT / 'kernel/link.ld'),
                  'portable statistics linker successor differs')
    else:
        E.require(linker.read_bytes() == baseline.read_bytes() == (ROOT / 'kernel/link.ld').read_bytes(),
                  'current statistics linker continuity')


def statistics(root, transport):
    V = module('verify-taskman-stats-evidence'); R = module('memory-current-regression')
    result = read(root / 'result.json')
    candidate = transport.binary(root / 'candidate/kernel.elf', result['candidate_sha256'])
    # This native entry point checks captured source bytes without consulting Git.
    # The original base linker receipt and the exported input closure are checked
    # separately; no historical record or historical success is synthesized.
    errors = V.verify_sources(root, result['implementation_base_commit'], True)
    rows = read(root / 'source-manifest.json')['files']
    E.require(len(rows) == len({r['path'] for r in rows}), 'duplicate statistics source input')
    started = read(root.parent / 'started.json')
    E.require(E.identity(started['harness']) == started['harness_sha256'], 'statistics captured harness digest')
    captured = {r['path']: r['sha256'] for r in started['harness']['files']}
    inputs = {r['path']: r['sha256'] for r in started['build_inputs']['files']}
    for row in rows:
        if row['path'] in inputs:
            E.require(E.sha(ROOT / row['path']) == row['sha256'] == inputs[row['path']], 'statistics source export differs')
        else:
            E.require(captured.get(row['path']) == row['sha256'], 'statistics captured harness source differs')
    statistics_linker(root, result['implementation_base_commit'])
    errors.extend(V.verify_host(root))
    for profile in V.HOST_PROFILES:
        path = root / 'host' / profile / 'taskman-stats-host'
        command = read(path.parent / 'command.json')
        transport.replace_binary_error(errors, f'host {profile} binary identity differs', path, command['binary_sha256'])
    E.require(not errors, 'statistics source/host replay: ' + '; '.join(errors))
    native = transport.foundation()
    def replay(directory, record, serial):
        errors = native.verify_transport_command(directory, record, serial)
        return subprocess.CompletedProcess([], int(bool(errors)), '\n'.join(errors))
    current = R.statistics_runtime_records(root, candidate, transport_replay=replay)
    E.require(current == read(root / 'current-result.json'), 'fresh statistics mutation replay differs')
    return {'native_entries': ['verify_sources', 'verify_host', 'verify_qemu_profile', 'parse_stats_record'], 'runs': 6}


def architecture(root, transport, *, validator=None, binary_base=None):
    V = validator or module('verify-arch-contract'); campaign = read(root / 'campaign.json')
    binary_base = binary_base or root
    V.validate_sources(root, campaign['source_manifest'])
    state = V.validate_static(root, campaign['static_result']); V.validate_run.static_result = state
    hashes = {}
    V.require(campaign.get('schema') == 1 and set(campaign['candidates']) == {'instrumented', 'negative', 'production'}, 'CANDIDATE_SET_INVALID')
    for role, row in campaign['candidates'].items():
        hashes[role] = transport.binary(binary_base / row['elf']['path'], row['elf_sha256'], row['elf']['bytes'])
        transport.binary(binary_base / row['image']['path'], row['image_sha256'], row['image']['bytes'])
    V.require(state['raw_layout_hashes'] == {'debug': hashes['instrumented'], 'production': hashes['production']}, 'STATIC_LAYOUT_CANDIDATE_MISMATCH')
    V.require(len(campaign['host_runs']) == 4 and {r['name'] for r in campaign['host_runs']} ==
              {'normal-debug', 'ubsan-debug', 'asan-debug', 'normal-disabled'}, 'HOST_MATRIX_INCOMPLETE')
    for row in campaign['host_runs']: V.validate_host(root, row)
    profiles = set()
    V.require(len(campaign['runtime_runs']) == 4, 'QEMU_MATRIX_INCOMPLETE')
    for row in campaign['runtime_runs']:
        expected = (*row['profile'], False, False); profiles.add(tuple(row['profile']))
        V.validate_run(root, row['result'], expected, hashes['instrumented'])
    E.require(profiles == {('q35', 'tcg', 'Haswell', 1), ('q35', 'tcg', 'Haswell', 4),
                           ('q35', 'kvm', 'host', 4), ('q35', 'kvm', 'host', 24)}, 'architecture profile coverage')
    V.validate_run(root, campaign['negative_run'], ('q35', 'tcg', 'Haswell', 1, True, False), hashes['negative'])
    V.validate_run(root, campaign['production_run'], ('q35', 'kvm', 'host', 4, False, True), hashes['production'])
    V.validate_contract_summary(campaign['contract_summary'])
    return {'native_entries': ['validate_static', 'validate_host', 'validate_run'], 'runs': 6}


def foundation(root, transport):
    V = transport.foundation()
    candidate = transport.binary(root / 'candidate/kernel.elf')
    image = transport.binary(root / 'candidate/hobbyos.img')
    profiles = sorted((root / 'profiles').iterdir())
    E.require({p.name for p in profiles} == set(V.EXPECTED_PROFILES), 'foundation profile coverage')
    errors = []
    for profile in profiles:
        E.require(V.read_env(profile / 'qemu/launch.env')['IMAGE_SHA256'] == image, 'foundation launch image differs')
        errors.extend(V.verify_profile(root, profile, candidate))
    errors.extend(V.verify_rate(root)); errors.extend(V.verify_soak(root / 'profiles/q35-kvm-smp24'))
    for name in ('production-contract/production', 'negative-contract/diagnostic-negative'):
        child = root / name
        digest = transport.binary(child / 'candidate/kernel.elf')
        image = transport.binary(child / 'candidate/hobbyos.img')
        for launch in (child / 'profiles').glob('*/qemu/launch.env'):
            E.require(V.read_env(launch)['IMAGE_SHA256'] == image, 'foundation child launch image differs')
        verifier = V.verify_production if name.startswith('production-contract/') else V.verify_diagnostic_negative
        errors.extend(verifier(child, portable_candidate_hash=digest))
    E.require(not errors, 'native foundation replay: ' + '; '.join(errors))
    return {'native_entries': ['verify_profile', 'verify_rate', 'verify_soak', 'verify_production', 'verify_diagnostic_negative'],
            'functional_runs': 6, 'production_runs': len(V.PRODUCTION_PROFILES), 'negative_runs': 1}


def interrupts(root, transport, timer=False):
    V = module('verify-timer-clockevent' if timer else 'verify-interrupt-bringup')
    V.ARTIFACT = root
    if timer:
        V.FAIR_ARTIFACT = root / 'fair-dispatch'; V.AUTONOMOUS_ARTIFACT = root / 'boot-to-shell'
    native = V.read_rows
    def rows():
        values = native()
        E.require(len(values) == len({r['stage'] for r in values}), 'duplicate inherited evidence stage')
        return [dict(r, log=str(transport.mapped(r['log']))) if r.get('log') else r for r in values]
    V.read_rows = rows
    with contextlib.redirect_stdout(io.StringIO()): V.verify('matrix', portable=transport)
    return {'native_entry': 'verify(matrix)', 'stages': len(rows())}


def canonical_builds(campaign, transport):
    E.require(read(campaign / 'build.json')['status'] == 'PASS', 'canonical build incomplete')
    for profile in ('debug', 'selftest', 'production'):
        directory = campaign / 'candidates' / profile
        candidate = read(directory / 'candidate.json'); receipt = read(directory / 'build-receipt.json')
        E.verify_candidate(candidate, receipt)
        for name, value in candidate['artifacts'].items():
            transport.binary(directory / name, value['sha256'], value['size'])
        elf = transport.elf(directory / 'kernel.elf', candidate['artifacts']['kernel.elf']['sha256'])
        code = read(directory / 'integrity-codegen.json')
        E.require(code['status'] == 'PASS' and code['undefined_symbols'] == [] and
                  code['elf_sha256'] == candidate['artifacts']['kernel.elf']['sha256'], 'integrity code generation binding')
        E.require(not transport.facts(directory / 'kernel.elf', 'undefined').strip(), 'undefined final kernel symbol')
        E.require(set(code['disassembly']) == {'bitmap_validate_allocation', 'pmm_validate_integrity'}, 'integrity disassembly coverage')
        for function, digest in code['disassembly'].items():
            path = directory / (function + '.disassembly.txt'); text = path.read_text()
            E.require(E.sha(path) == digest and '<' + function + '>:' in text and
                      not re.search(r'\b(?:popcnt\w*|[xyz]mm\d+|bitmap_get|__popcount\w*)\b', text),
                      'unapproved integrity scan lowering')
    found = set()
    for base in (campaign / 'attempts', campaign / 'reused-build-commands'):
        for path in base.glob('*/command.json'):
            name = path.parent.name.split('Z-', 1)[-1]
            if name not in ('deps-check', 'debug-stack', 'production-stack', 'selftest-stack'): continue
            row = read(path)
            E.require(row['exit_status'] == 0, 'canonical prerequisite failed')
            if name.endswith('-stack'): E.require('stack-check: PASS' in path.with_name('stdout.log').read_text(), 'stack proof absent')
            found.add(name)
    E.require(found == {'deps-check', 'debug-stack', 'production-stack', 'selftest-stack'}, 'canonical required checks missing')
    return {'profiles': ['debug', 'selftest', 'production'], 'checks': sorted(found)}


def production_policy(root, campaign, transport):
    candidate = read(campaign / 'candidates/production/candidate.json')
    build = read(campaign / 'candidates/production/build-receipt.json')
    E.verify_candidate(candidate, build)
    for name in ('kernel.elf', 'BOOTX64.EFI'):
        transport.binary(root / name, candidate['artifacts'][name]['sha256'])
    proof = read(root / 'policy-proof.json')
    transport.binary(root / 'hobbyos.img', proof['image_sha256'])
    E.require(proof['status'] == 'PASS' and proof['build_input_sha256'] == candidate['source_sha256'] and
              proof['payloads'] == build['payload']['files'], 'production policy payload/source receipt differs')
    absent = proof['absence']
    E.require(absent['status'] == 'PASS' and absent['elf_sha256'] == candidate['artifacts']['kernel.elf']['sha256'] and
              absent['compiled_strings_sha256'] == E.sha(root / 'compiled-strings.txt'), 'production absence binding')
    G = module('memory-observation-gate')
    expected = list(G.SURFACE) + ['[CPU_CONTROL]', '[MEMORY_MAP]', '[MTRR]', 'ptinspect', 'cpustate', 'memmap']
    E.require(absent['forbidden'] == expected, 'production forbidden inventory differs')
    strings = transport.facts(root / 'kernel.elf', 'strings'); symbols = transport.facts(root / 'kernel.elf', 'symbols')
    E.require(strings == (root / 'compiled-strings.txt').read_text(), 'production compiled strings differ')
    E.require(not any(name in strings for name in expected), 'production diagnostic surface')
    E.require(not any(name in symbols for name in ('paging_inspect', 'page_table_decode', 'g_observer_tables', 'cpu_control_read_local')),
              'production diagnostic helper')
    return {'native_contract': 'actual-binary-absence-with-saved-tool-facts', 'payloads': 5}


def taskman_visual(root, campaign, transport):
    V = module('verify-taskman-visual-evidence')
    result = read(root / 'current-result.json')
    candidate = read(campaign / 'candidates/selftest/candidate.json')
    build = read(campaign / 'candidates/selftest/build-receipt.json')
    E.require(result['candidate_id'] == E.verify_candidate(candidate, build) and result['status'] == 'PASS', 'visual candidate differs')
    E.require(result['checks'] == V.verify_active_evidence(root, False), 'visual raw checks differ')
    E.require(result['container']['raw_image_sha256'] == candidate['artifacts']['hobbyos.img']['sha256'], 'visual image identity')
    rows = result['payloads']
    E.require(len(rows) == 5 and {r['name'] for r in rows} == set(build['payload']['files']), 'visual payload inventory')
    for row in rows:
        E.require(row['identical'] is True and {'size': row['size'], 'sha256': row['sha256']} == build['payload']['files'][row['name']], 'visual payload proof differs')
    human = (ROOT / 'docs/test-reports/TASKMAN_VISUAL_HUMAN_CHECKLIST.md').read_text()
    E.require(result['human_review'] == 'PENDING' and 'STATUS: AWAITING_HUMAN_REVIEW' in human and '[x]' not in human.lower(), 'visual human authority changed')
    return {'native_entry': 'verify_active_evidence', 'payload_receipts': 5, 'human_review': 'PENDING'}


def section_layout(root, transport):
    V = ELF.V; current = module('memory-current-layout')
    campaign = read(root / 'campaign.json')
    E.require(campaign['kind'] == 'section-layout-current-evidence' and campaign['references'] == [], 'layout historical authority changed')
    pairs = []
    for profile in ('debug-on', 'debug-off'):
        pair = read(root / campaign['pairs'][profile]); objects = read(root / pair['objects'])['objects']
        E.require(objects and len(objects) == len({r['path'] for r in objects}), 'layout object inventory')
        for row in objects: transport.binary(root / row['copy_path'], row['sha256'])
        elfs = []
        for side in ('before', 'after'):
            record = pair[side]
            elfs.append(transport.elf(root / record['elf'], record['sha256']))
            V.require_hash(root / record['map'], record['map_sha256'])
            V.validate_map_policy(root / record['map'], record['elf'], [r['path'] for r in objects], True)
        E.require(pair['before']['sha256'] == pair['after']['sha256'] == pair['after']['canonical_sha256'], 'same-object relink differs')
        transport.binary(root / pair['after']['canonical_elf'], pair['after']['canonical_sha256'])
        V.compare_trampoline(*elfs)
        pairs.append({'profile': profile, 'elf_sha256': pair['after']['sha256'], 'objects': len(objects)})
    orphan = read(root / campaign['orphan_probe'])
    for name in ('source', 'map'): V.require_hash(root / orphan[name], orphan[name + '_sha256'])
    transport.binary(root / orphan['object'], orphan['object_sha256'])
    orphan_elf = transport.elf(root / orphan['elf'], orphan['elf_sha256'])
    V.validate_orphan_binary(orphan_elf, bytes.fromhex(orphan['payload_hex']))
    E.require('.layout_probe' in (root / orphan['map']).read_text() and 'orphan section' in (root / orphan['stderr']).read_text(), 'orphan diagnostics absent')
    rows = campaign['candidates']
    E.require(len(rows) == len(current.ROLES) and {r['role'] for r in rows} == current.ROLES, 'layout role coverage')
    for row in rows:
        role_elf=transport.elf(root / row['elf'], row['elf_sha256'])
        V.require_hash(root / row['receipt'], row['receipt_sha256'])
        E.require(row['elf_sha256'] in (root / row['receipt']).read_text(), 'layout candidate receipt contradicts ELF')
        flags = row['configuration']
        E.require(flags.get('DEBUG_ASSERT') == int(row['role'] != 'production-debug-off'), 'layout assertion role flags')
        if row['role'].startswith('architecture-'):
            E.require(flags.get('ARCH_TEST') == flags.get('SELFTEST') == 1 and flags.get('ARCH_NEGATIVE_GP', 0) == int(row['role'] == 'architecture-negative'), 'layout architecture roles')
        if row['role']=='framebuffer-reference':
            E.require(flags=={'SELFTEST':1,'DEBUG_ASSERT':1,'SELFTEST_AUTORUN':0,'FRAMEBUFFER_REFERENCE':1},
                      'framebuffer reference layout role flags')
            E.require('mmio_framebuffer_reference' in {s['name'] for s in role_elf.symbols()},
                      'framebuffer reference code absent from layout role')
    expected = {'status': 'PASS', 'contract': 'current-layout', 'pairs': pairs, 'roles': sorted(current.ROLES), 'historical': 'HISTORICAL_REPLAY_UNAVAILABLE'}
    E.require(read(root / 'current-result.json') == expected, 'layout structural replay differs')
    fixtures = ELF.replay_native_fixtures(root, elfs[1], orphan_elf,
                                       expected_result=transport.json(root / 'fixtures/results.json'))
    return {'native_entries': ['validate_layout', 'validate_map_policy', 'compare_trampoline', 'validate_orphan_binary'],
            'roles': sorted(current.ROLES), 'fixtures': fixtures}


def selected_native(campaign, transport):
    selected = read(campaign / 'regression-selection.json'); results = {}
    for key, relative in selected.items():
        E.safe(relative); attempt = campaign / relative
        result = module('memory-current-regression').family_result(attempt, transport); root = attempt / 'evidence'
        E.require(result['status'] == 'PASS' and result['key'] == key, 'uncompleted selected native family')
        E.require(result['build_input_sha256'] == E.identity(E.manifest()), 'native tested source differs from export')
        started = read(attempt / 'started.json')
        module('memory-current-regression').requirement_binding(key, started, result)
        E.verify_manifest(started['build_inputs'], started['build_input_sha256'])
        E.require(E.identity(started['harness']) == started['harness_sha256'], 'native captured harness digest')
        for name in result['commands']:
            receipt = read(attempt / name); output = (attempt / name).with_suffix('.stdout.log')
            E.require(receipt['exit_status'] == 0 and receipt['build_input_sha256'] == result['build_input_sha256'] and
                      E.sha(output) == receipt['stdout_sha256'], 'native command exit/source/output proof')
        if key == 'canonical-builds': results[key] = canonical_builds(campaign, transport)
        elif key == 'foundation': results[key] = foundation(root, transport)
        elif key in ('interrupts', 'clockevent'): results[key] = interrupts(root, transport, key == 'clockevent')
        elif key == 'section-layout': results[key] = section_layout(root, transport)
        elif key == 'taskman-visual': results[key] = taskman_visual(root, campaign, transport)
        elif key == 'production-policy': results[key] = production_policy(root, campaign, transport)
        elif key == 'build-profile':
            V = module('verify-build-profile')
            V.verify_manifest(root / 'source'); V.verify_source(ROOT); V.verify_build(root); V.verify_runtime(root); V.verify_production(root)
            results[key] = {'native_entries': ['verify_source', 'verify_build', 'verify_runtime', 'verify_production']}
        elif key == 'architecture': results[key] = architecture(root, transport)
        elif key == 'assertion-context': results[key] = assertion_runtime(root, transport)
        elif key == 'assertion-record':
            R = module('memory-current-regression')
            assertions = campaign / selected['assertion-context'] / 'evidence'
            matrix = results.get('assertion-context') or assertion_runtime(assertions, transport)
            current = R.assertion_records(root, assertions, validated_matrix=matrix['matrix'])
            E.require(read(root / 'current-result.json') == current, 'fresh assertion record mutation replay differs')
            results[key] = {'native_entries': ['assertion_records', 'validate_transcript'], 'fresh_records': len(current['records'])}
        elif key == 'taskman-statistics': results[key] = statistics(root, transport)
        elif key == 'formatting': results[key] = formatting(root, transport)
        elif key == 'modal-heap':
            format_root = campaign / selected['formatting'] / 'evidence'
            if 'formatting' not in results: formatting(format_root, transport)
            current = read(root / 'current-result.json')
            consumer = read(format_root / read(format_root / 'result.json')['consumer_result'])
            E.require(current['profiles'] == consumer['modal_heap_profiles'] and current['status'] == 'PASS' and
                      current['consumer_result_sha256'] == E.sha(format_root / read(format_root / 'result.json')['consumer_result']),
                      'modal shared evidence differs')
            results[key] = {'native_entries': ['verify_consumers', 'verify_memory_checkpoint'], 'runs': len(current['profiles'])}
        elif key == 'panic':
            V = module('verify-panic-evidence'); runs = sorted((root / 'runtime').glob('*/result.json'))
            E.require({p.parent.name for p in runs} == V.CURRENT_RUNS, 'panic runtime matrix coverage')
            for path in runs:
                errors = V.verify_run(path.parent)
                for name, record in read(path)['artifacts'].items():
                    artifact = path.parent / name
                    if not artifact.exists():
                        transport.replace_binary_error(errors, 'recorded artifact is missing: ' + name,
                                                       artifact, record['sha256'], record['bytes'])
                E.require(not errors, 'native panic replay: ' + '; '.join(errors))
            results[key] = {'native_entry': 'verify_run', 'runs': len(runs)}
        elif key in ('lock-progress', 'allocator'):
            V = module('verify-' + ('lock-progress' if key == 'lock-progress' else 'alloc-hardening'))
            candidate = transport.binary(root / 'candidate/kernel.elf')
            V.verify_hash_manifest(root / 'source', root / 'source/SHA256SUMS')
            for line in (root / 'candidate/SHA256SUMS').read_text().splitlines():
                digest, name = line.split('  ', 1); path = root / 'candidate' / name
                if path.exists(): E.require(E.sha(path) == digest, 'allocator/lock candidate raw proof differs')
                else: transport.binary(path, digest)
            for name in ('deps-check', 'kernel-check', 'stack-check', 'image'):
                E.require((root / 'build' / (name + '.exit-code.txt')).read_text().strip() == '0', 'allocator/lock build failed')
            if key == 'allocator': V.verify_runtime(root, candidate)
            else:
                V.verify_profile(root, 8, 9000, candidate); V.verify_profile(root, 24, 1000, candidate)
            for profile in ('normal', 'ubsan'):
                log = (root / 'host' / profile / 'run.log').read_text()
                if key == 'allocator': V.parse_host(log)
                else: E.require(len([l for l in log.splitlines() if V.HOST_RE.fullmatch(l)]) == 1, 'lock host proof')
                E.require((root / 'host' / profile / 'exit-code.txt').read_text().strip() == '0', 'allocator/lock host exit')
            for name in ('kernel.strings.txt', 'kernel.symbols.txt'):
                E.require(('locktest' if key == 'lock-progress' else 'alloctest') not in (root / 'production' / name).read_text().lower(), 'stress helper in production')
            for name in ('production-image', 'policy'):
                E.require((root / 'production' / (name + '.exit-code.txt')).read_text().strip() == '0', 'allocator/lock production failed')
            E.require('[BOOT][PRODUCTION_TEST_POLICY] PASS' in (root / 'production/policy.log').read_text(), 'allocator/lock policy proof absent')
            with tempfile.TemporaryDirectory(prefix='memory-stress-fixtures-') as temp:
                output = Path(temp) / 'fixtures'; V.make_fixtures(output)
                # The native fixture producer deterministically invokes the same
                # semantic parser, including expected rejection codes.
                recorded = next(r for r in transport.index['synthetic_deltas'] if r['path'] ==
                                (root / 'fixtures/result.json').relative_to(transport.root).as_posix())
                E.require(read(output / 'result.json') == read(transport.root / recorded['blob']), 'stress fixture replay differs')
            results[key] = {'native_entries': ['verify_profile' if key == 'lock-progress' else 'verify_runtime']}
        else:
            results[key] = {'native_replay': 'PENDING_PORTABLE_ADAPTER'}
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            import stack_native_audit
            actual=stack_native_audit.verify_family(root,
                required=key in module('memory-current-regression').STACK_RUNTIME_FAMILIES)
            E.require(result.get('stack_services')==actual,'portable native stack terminal launch inventory differs')
            results[key]['stack_services']=actual
    return results


def regression_command_binding(transport, command, receipt, expected, key):
    """Relocate indexed command paths and the exact stack audit output slot."""
    E.require(type(receipt['argv']) is list and type(receipt['environment']) is dict,
              'portable regression command/options differ: ' + key)
    # Logs and receipts remain immutable. An empty output directory has no
    # selected descendant, so its authority is the selected command's origin.
    environment = dict(receipt['environment'])
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'guarded-kernel-stacks':
        name = command.relative_to(transport.root).as_posix()
        row = transport.content.get(name, {})
        origin = row.get('local_path')
        E.require(isinstance(origin, str) and Path(origin).is_absolute() and
                  str(Path(origin)) == origin and '..' not in Path(origin).parts and
                  transport.paths.get(origin) == command,
                  'portable stack audit command origin differs')
        field = 'HOBBYOS_STACK_NATIVE_AUDIT'
        E.require(environment.get(field) == str(Path(origin).parent / 'evidence/service-audits'),
                  'portable stack audit output differs from selected command')
        environment[field] = str(command.parent / 'evidence/service-audits')

    def logical(value):
        if isinstance(value, list): return [logical(v) for v in value]
        if isinstance(value, dict): return {k: logical(v) for k, v in value.items()}
        if not isinstance(value, str): return value
        if value in transport.paths: return str(transport.paths[value])
        for old, path in transport.paths.items():
            if old.startswith('/') and old.startswith(value.rstrip('/') + '/'):
                suffix = old[len(value.rstrip('/')) + 1:]
                target = path
                for _ in Path(suffix).parts: target = target.parent
                return str(target)
        return value

    E.require((logical(receipt['argv']), logical(environment)) == expected,
              'portable regression command/options differ: ' + key)


def regression_ledger(campaign, transport):
    R = module('memory-current-regression'); ledger = read(campaign / 'regression-ledger.json')
    R.ledger_policy(ledger, E.identity(E.manifest()))
    E.require(ledger['replay_harness_sha256'] == E.identity(E.manifest(harness=True)), 'portable regression verifier version changed')
    selected = read(campaign / 'regression-selection.json')
    for row in ledger['rows']:
        E.require(selected[row['key']] == row['attempt'] and row['replay'] == {'family': row['key'], 'attempt': row['attempt']},
                  'portable ledger selected attempt differs')
        prefix = campaign.relative_to(transport.root).as_posix() + '/'
        actual = {p.removeprefix(prefix) for p in transport.content if p.startswith(prefix + row['attempt'] + '/')}
        names = [r['path'] for r in row['raw']]
        E.require(len(names) == len(set(names)) and set(names) == actual, 'portable missing/unlisted/duplicate raw ledger proof')
        for item in row['raw']:
            E.safe(item['path']); recorded = transport.content[prefix + item['path']]
            E.require(recorded['sha256'] == item['sha256'] and recorded['size'] == item['size'], 'portable ledger raw identity differs')
        attempt = campaign / row['attempt']; result = R.family_result(attempt, transport)
        E.require(row['requirement_binding'] == R.requirement_binding(row['key'], read(attempt / 'started.json'), result),
                  'portable regression requirement authority differs')
        E.require(row['commands'] == [row['attempt'] + '/' + name for name in result['commands']], 'portable ledger command inventory')
        expected = R.family_commands(row['key'], attempt / 'evidence', campaign)
        E.require(len(expected) == len(row['commands']), 'portable ledger command count')
        for name, (argv, environment) in zip(row['commands'], expected):
            receipt = read(campaign / name)
            E.require(receipt['exit_status'] == 0 and receipt['build_input_sha256'] == row['build_input_sha256'] and
                      receipt['started_ns'] < receipt['finished_ns'], 'portable failed/stale regression command')
            regression_command_binding(transport, campaign / name, receipt, (argv, environment), row['key'])
    return {'status': 'PASS', 'requirements': len(ledger['rows'])}


def native_fixtures(campaign, transport):
    selected = read(campaign / 'regression-selection.json'); results = {}
    with tempfile.TemporaryDirectory(prefix='memory-native-fixture-replay-') as temp:
        base = Path(temp)
        for key, entry, output_name in (
            ('build-profile', 'create_fixtures', 'result.json'),
            ('assertion-context', 'fixtures', 'results.json'),
            ('taskman-statistics', 'run_fixtures', 'result.json'),
            ('formatting', 'run_fixtures', 'fixture-results.json'),
            ('foundation', 'fixture_selftest', None),
            ('panic', 'fixture_selftest', None)):
            script = {'build-profile': 'verify-build-profile', 'assertion-context': 'verify-assert-list-evidence',
                      'taskman-statistics': 'verify-taskman-stats-evidence', 'formatting': 'verify-libc-format-evidence',
                      'foundation': 'verify-foundation-regression', 'panic': 'verify-panic-evidence'}[key]
            V = module(script); output = base / key
            if key == 'formatting':
                # The historical generator asks Git to describe its synthetic
                # source copy. Replaying its exact stored deltas instead keeps
                # the native parser intact and needs no invented repository.
                original = campaign / selected[key] / 'evidence/oracle-fixtures'
                prefix = original.relative_to(transport.root).as_posix()
                transport.materialize_deltas(base, prefix)
                generated = base / prefix; value = transport.json(original / output_name)
                E.require(value['status'] == 'PASS' and value['valid_controls'] == 1 and
                          value['negative_controls'] == 18 and len(value['records']) == 19, 'formatter fixture inventory')
                for row in value['records']:
                    E.safe(row['name']); errors = V.verify_evidence(generated / row['name'])
                    E.require(errors == row['errors'] and (not errors if row['expected'] == 'PASS' else
                              any(row['expected'] in error for error in errors)), 'materialized formatter rejection differs: ' + row['name'])
                results[key] = {'valid_controls': 1, 'negative_controls': 18, 'native_entry': 'verify_evidence on exact synthetic deltas'}
                continue
            with contextlib.redirect_stdout(io.StringIO()): result = getattr(V, entry)(output)
            E.require(result is None or isinstance(result, dict) or result == 0, 'native fixture replay failed: ' + key)
            if output_name:
                value = read(output / output_name); E.require(value['status'] == 'PASS', 'native fixture status: ' + key)
                results[key] = {k: v for k, v in value.items() if k in ('valid_controls', 'negative_controls', 'negative_cases', 'negative_fixtures')}
            else: results[key] = {'status': 'PASS', 'native_generator': entry}
        original = campaign / selected['architecture'] / 'evidence'
        source = base / 'architecture-base'
        shutil.copytree(original, source, ignore=shutil.ignore_patterns('oracle-fixtures'))
        V = module('verify-arch-contract')
        V.validate_all = lambda root: architecture(root, transport, validator=V, binary_base=original)
        with contextlib.redirect_stdout(io.StringIO()): generated = V.run_fixtures(base / 'architecture-fixtures', source)
        saved = transport.json(original / 'oracle-fixtures/results.json')
        def outcomes(value):
            return [{k: r[k] for k in ('id', 'status', 'expected_code', 'observed_code') if k in r} for r in value['records']]
        E.require(outcomes(generated) == outcomes(saved), 'native architecture fixture rejection boundaries differ')
        results['architecture'] = {k: generated[k] for k in ('valid_controls', 'summary_negative_controls', 'evidence_negative_controls')}
    return results


def current_fixtures(campaign, transport, *, host_tests=False):
    """Regenerate focused fixtures outside the immutable extracted package."""
    acceptance = read(campaign / 'acceptance.json'); receipt = acceptance['fixtures']
    E.safe(receipt['directory']); original = campaign / receipt['directory']
    E.require(read(original / 'mode.json') == receipt and receipt['status'] == 'PASS' and
              receipt['source_sha256'] == E.identity(E.manifest()) and
              receipt['harness_sha256'] == E.identity(E.manifest(harness=True)), 'current fixture source/version differs')
    series = read(campaign / 'series.json'); capture = campaign / series['diagnostics'][0]
    production = campaign / read(campaign / 'production.json')['runs'][0]
    result = {}; prior = Path.cwd()
    with tempfile.TemporaryDirectory(prefix='memory-current-fixture-replay-') as temp:
        destination = Path(temp)
        try:
            os.chdir(ROOT)
            def execute(name, argv):
                proc = subprocess.run([sys.executable, '-B', *map(str, argv)], text=True, capture_output=True,
                                      env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
                E.require(proc.returncode == 0, 'portable current fixture ' + name + ': ' + proc.stdout + proc.stderr)
                result[name] = {'status': 'PASS', 'exit_status': proc.returncode, 'stdout': proc.stdout.strip()}
            for name, script, option in (
                    ('decoder-verifier', 'memory-observation-fixtures.py', '--evidence'),
                    ('interrupt', 'memory-interrupt-fixtures.py', '--output'),
                    ('packaging', 'memory-packaging-fixtures.py', '--output')):
                execute(name, ['scripts/' + script, option, destination / name])
            execute('capture-recipes', ['scripts/memory-replay-fixtures.py', '--source', capture,
                    '--replay', original / 'capture-recipes/result.json'])
            execute('semantic', ['scripts/memory-integrity-fixtures.py', '--capture', capture,
                    '--series', campaign, '--production', production, '--portable-package', transport.root,
                    '--replay', original / 'semantic/result.json'])
            proof = transport.json(original / 'elf-structure/proof.json')
            E.require(ELF.fixture_controls(proof) == transport.json(original / 'elf-structure/result.json'), 'ELF control replay differs')
            result['ELF-fragments'] = {'status': 'PASS', 'negative': 5}
            if host_tests:
                G = module('memory-observation-gate')
                (destination / 'actual-c').mkdir()
                with contextlib.redirect_stdout(io.StringIO()): G.host(destination / 'actual-c')
                generated = read(destination / 'actual-c/host.json')
                E.require(generated == read(original / 'host.json'), 'actual C host source/coverage differs')
                result['actual-C'] = generated
        finally:
            os.chdir(prior)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--require-acceptance', action='store_true')
    parser.add_argument('--native-fixtures', action='store_true')
    parser.add_argument('--current-fixtures', action='store_true')
    parser.add_argument('--host-tests', action='store_true', help='Compile and run only the isolated hosted C tests, never a kernel.')
    args = parser.parse_args(); root = args.package.resolve(); transport = Transport(root)
    transport.verify_source_identity()
    campaign = root / 'evidence/current-campaign'
    V = module('verify-memory-observation')
    series = V.verify_series(campaign)
    G = module('memory-observation-gate')
    G.verify_static(campaign)
    G.verify_baseline(campaign, series)
    production = read(campaign / 'production.json')
    for relative in production['runs']: V.verify_run(campaign / relative / 'qemu-serial.log')
    natives = selected_native(campaign, transport)
    fixture_replay = native_fixtures(campaign, transport) if args.native_fixtures else None
    if args.require_acceptance:
        acceptance = read(campaign / 'acceptance.json')
        E.require(acceptance['status'] == 'PASS' and acceptance['contract'] == 'memory-current-regression-v1' and
                  acceptance['series'] == series and acceptance['hardware'] == 'DEFERRED_TO_FINAL_CLOSURE', 'current acceptance is incomplete or contradicted')
        E.require(acceptance['baseline_replay'] == G.verify_baseline(campaign, series), 'portable baseline replay identity differs')
        fixture = acceptance['fixtures']; E.safe(fixture['directory'])
        E.require(fixture == read(campaign / fixture['directory'] / 'mode.json') and
                  fixture['harness_sha256'] == E.identity(E.manifest(harness=True)) and
                  fixture['source_sha256'] == E.identity(E.manifest()), 'acceptance fixture verifier/source differs')
        regression_ledger(campaign, transport)
        E.require(all(v.get('native_replay') != 'PENDING_PORTABLE_ADAPTER' for v in natives.values()), 'required portable native adapter missing')
    focused = current_fixtures(campaign, transport, host_tests=args.host_tests) if args.current_fixtures or args.host_tests else None
    print(json.dumps({'package_content_replay': 'PASS', 'current_acceptance': 'PASS' if args.require_acceptance else 'NOT_ASSERTED',
                      'series_runs': series['runs'], 'native': natives, 'native_fixture_replay': fixture_replay, 'current_fixture_replay': focused,
                      'binary_limitation': transport.index['binary_replay_limit']}, sort_keys=True))


if __name__ == '__main__': main()
