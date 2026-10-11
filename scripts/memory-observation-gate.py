#!/usr/bin/env python3
"""Explicit memory observation gate modes; all evidence is retained under backups."""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import memory_evidence as E
import validation_candidates as candidates
import validation_retention as retention

ROOT = Path(__file__).resolve().parent.parent
PROTECTED = {
 'kernel/src/graphics/graphics.c': '2f7a316d7ea69b44e9bb07442de8f6ffd4f719dd91c931661c9729ef05e37ad5',
 'kernel/src/graphics/graphics.h': 'e147856001d2ed1f555ac681f95f0ed09d28dff1bbc23ebc2a4bb3c28a17ed31',
 'kernel/src/core/task_format.c': '2ba46851fd9246060e3cf64056e0013d9237a25b7f41d321b3cf5433daa19863',
 'kernel/src/core/task_format.h': '404285390c07e56a8baa31838e8479464d6ae7c2d6ffaedde4ebea9231f9fd8b',
 'kernel/src/shell/commands/cmd_mem.c': '2318831fc4697376d4e24b3223e6c6671ffa0bcf04256821cd8212e180264368',
 'kernel/src/apic/lapic.c': 'f9b9c55e05cd0eff44269336a7ddaff151626ed12e31a25ea99120507843e474'}
SURFACE = ('cmd_ptinspect', 'cmd_cpustate', 'cmd_memmap', 'cpu_control_snapshot_irq',
           'g_slot_publication', 'g_active_generation', 'g_capture', 'memory_observation_init',
           'cmd_tlbtest', 'tlb_fixture', 'tlb_proof', 'cpu_request_test_delay', 'g_delayed_slot')


def stamp(): return datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for part in iter(lambda: f.read(1024 * 1024), b''): h.update(part)
    return h.hexdigest()

def write(path, value):
    E.write(path, value)


def source_manifest():
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard']).decode().split('\0')
    names = sorted(set(n for n in names if n and Path(n).suffix not in ('.su', '.o', '.d') and
                       (n.startswith(('kernel/', 'bootloader/', 'shared/')) or n == 'makefile')))
    return [{'path': n, 'size': (ROOT / n).stat().st_size, 'sha256': sha(ROOT / n)} for n in names]


def run(evidence, subject, argv, environment=None):
    attempt = evidence / 'attempts' / (stamp() + '-' + subject)
    attempt.mkdir(parents=True)
    receipt = {'schema': 2, 'argv': [str(a) for a in argv], 'cwd': str(ROOT), 'start_utc': stamp(),
               'started_ns': time.monotonic_ns(), 'environment': environment or {},
               'build_input_sha256': E.identity(E.manifest()), 'harness_sha256': E.identity(E.manifest(harness=True))}
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(environment or {}))
    with (attempt / 'stdout.log').open('w') as out:
        proc = subprocess.run(argv, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, env=env)
        out.flush(); os.fsync(out.fileno())
    receipt.update(exit_status=proc.returncode, end_utc=stamp(), finished_ns=time.monotonic_ns(), stdout_sha256=sha(attempt / 'stdout.log'))
    write(attempt / 'command.json', receipt)
    print(json.dumps({'command': subject, 'exit_status': proc.returncode, 'attempt': str(attempt)}), flush=True)
    if proc.returncode: raise RuntimeError(f'{subject} failed; see {attempt}')
    return attempt


def save_candidate(directory, profile, inputs_before, build_attempts):
    directory.mkdir(parents=True, exist_ok=False)
    manifest = E.manifest()
    E.require(E.identity(manifest) == inputs_before, 'build inputs changed during compilation')
    harness = E.manifest(harness=True)
    info = {'schema': 2, 'profile': profile, 'created_utc': stamp(),
            'base_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
            'build_inputs': manifest, 'source_sha256': E.identity(manifest),
            'harness_inputs': harness, 'harness_sha256': E.identity(harness),
            'artifacts': {}, 'symbols': {}}
    for name in ('kernel.elf', 'hobbyos.img', 'BOOTX64.EFI'):
        if (ROOT / name).is_file():
            subprocess.run(['cp','--sparse=always','--preserve=mode,timestamps',
                            str(ROOT/name),str(directory/name)],check=True)
            E.require(sha(ROOT/name)==sha(directory/name),'frozen artifact copy differs')
            info['artifacts'][name] = {'sha256': sha(directory / name), 'size': (directory / name).stat().st_size}
    for name, args in [('symbols.txt', ['nm', '-an']), ('sections.txt', ['size', '-A']),
                       ('elf.txt', ['readelf', '-W', '-S', '-l'])]:
        data = subprocess.check_output(args + [str(directory / 'kernel.elf')], text=True)
        (directory / name).write_text(data)
    for line in (directory / 'symbols.txt').read_text().splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ('_kernel_start', '_kernel_end', '_text_start', '_text_end'):
            info['symbols'][parts[2]] = int(parts[0], 16)
    payload = {'status': 'PASS', 'image_sha256': info['artifacts']['hobbyos.img']['sha256'], 'files': {}}
    with tempfile.TemporaryDirectory(prefix='memory-payload-') as tmp:
        for name, guest, source in [('kernel.elf', '::/kernel.elf', directory / 'kernel.elf'),
                ('BOOTX64.EFI', '::/EFI/BOOT/BOOTX64.EFI', directory / 'BOOTX64.EFI'),
                ('zap-light16.psf', '::/EFI/fonts/zap-light16.psf', ROOT / 'bootloader/fonts/zap-light16.psf'),
                ('logo.bmp', '::/EFI/images/logo.bmp', ROOT / 'bootloader/images/logo.bmp'),
                ('startup.nsh', '::/startup.nsh', ROOT / 'bootloader/startup.nsh')]:
            target = Path(tmp) / name
            subprocess.run(['mcopy', '-i', str(directory / 'hobbyos.img'), guest, str(target)], check=True)
            E.require(sha(target) == sha(source) and target.stat().st_size == source.stat().st_size, 'FAT payload mismatch')
            payload['files'][name] = {'size': target.stat().st_size, 'sha256': sha(target)}
    commands = []
    for i, attempt in enumerate(build_attempts):
        target = directory / 'build-commands' / str(i)
        target.mkdir(parents=True)
        for name in ('command.json', 'stdout.log'):
            shutil.copyfile(attempt / name, target / name)
        r = json.loads((target / 'command.json').read_text())
        E.require(r['exit_status'] == 0, 'failed build command')
        commands.append({'receipt': f'build-commands/{i}/command.json', 'receipt_sha256': sha(target / 'command.json'),
                         'output': f'build-commands/{i}/stdout.log', 'output_sha256': sha(target / 'stdout.log')})
    receipt = {'schema': 2, 'status': 'PASS', 'profile': profile, 'build_input_sha256': E.identity(manifest),
               'inputs_before': inputs_before, 'artifacts': info['artifacts'], 'payload': payload, 'commands': commands,
               'compiler': subprocess.check_output(['gcc', '--version'], text=True),
               'linker': subprocess.check_output(['ld', '--version'], text=True)}
    E.write(directory / 'build-receipt.json', receipt)
    info['build_receipt_sha256'] = E.identity(receipt)
    E.write(directory / 'candidate.json', info)
    E.verify_candidate(info, receipt)
    return info


def absent(directory, debug=False):
    directory = candidates.resolve(directory)
    data = (directory / 'kernel.elf').read_bytes()
    symbols = subprocess.check_output(['nm', '-a', str(directory / 'kernel.elf')], text=True)
    strings = subprocess.check_output(['strings', '-a', str(directory / 'kernel.elf')], text=True)
    forbidden = list(SURFACE) + ['[CPU_CONTROL]', '[MEMORY_MAP]', '[MTRR]', 'ptinspect', 'cpustate', 'memmap']
    for name in forbidden:
        if name.encode() in data: raise ValueError('production diagnostic surface: ' + name)
    if not debug:
        for name in ('paging_inspect', 'page_table_decode', 'g_observer_tables', 'cpu_control_read_local'):
            if name in symbols: raise ValueError('production diagnostic helper: ' + name)
    path = directory / 'compiled-strings.txt'
    if path.exists(): E.require(path.read_text() == strings, 'compiled string facts changed')
    else: path.write_text(strings)
    return {'status': 'PASS', 'elf_sha256': sha(directory / 'kernel.elf'), 'debug_without_selftest': debug,
            'compiled_strings_sha256': sha(path), 'scan': 'entire_actual_ELF_and_nm_symbols', 'forbidden': forbidden}


def static_invariants():
    result = {}
    current_tlb = os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('tlb-coherency','physical-translation','owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks')
    if current_tlb:
        import tlb_current
        tlb_current.contract()
    for name, expected in PROTECTED.items():
        actual = sha(ROOT / name)
        if actual != expected and not (current_tlb and name == 'kernel/src/apic/lapic.c'):
            raise ValueError('protected source changed: ' + name)
        result[name] = actual
    text = (ROOT / 'makefile').read_text()
    for flag in ('-O2', '-fno-delete-null-pointer-checks', '-Wframe-larger-than=2048', '-Werror'):
        if flag not in text: raise ValueError('required compiler flag absent: ' + flag)
    diagnostics = [p for p in (ROOT / 'kernel/src/memory').glob('*') if p.name.startswith(('page_table_decode', 'memory_observ', 'paging_inspect')) and p.suffix in ('.c', '.h')]
    diagnostics += list((ROOT / 'kernel/src/cpu').glob('cpu_control_snapshot.[ch]'))
    diagnostics += list((ROOT / 'kernel/src/shell/commands').glob('cmd_memory_observation.[ch]'))
    forbidden = re.compile(r'\b(paging_map|paging_load_map|invlpg|wbinvd|invd|wrmsr|cpu_write_msr|pmm_alloc|kmalloc|kfree)\b')
    for path in diagnostics:
        src = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
        if forbidden.search(src): raise ValueError('diagnostic mutation API: ' + str(path))
        if re.search(r'mov[^"\n]*,\s*%%cr[034]', src): raise ValueError('diagnostic control write')
    interrupts = (ROOT / 'kernel/src/core/interrupts.c').read_text()
    if not current_tlb and sha(ROOT / 'kernel/src/core/interrupts.c') != 'ab86c9528733a581e0b12bebfeba9db2dfe0602d237ac0d302cc82c3e8323304':
        raise ValueError('fixed interrupt anchor mismatch')
    if interrupts.count('cpu_control_snapshot_irq();') != 1: raise ValueError('wake hook cardinality')
    return {'status': 'PASS', 'protected': result}


def static(evidence):
    result = static_invariants()
    run(evidence, 'diff-check', ['git', 'diff', '--check'])
    run(evidence, 'shell-syntax', ['bash', '-n', 'scripts/qemu-run.sh', 'scripts/qemu-agent.sh', 'scripts/test-page-table-inspector.sh'])
    run(evidence, 'inventory', ['python3', 'scripts/memory-source-inventory.py', '--output', str(evidence / 'inventory.json')])
    write(evidence / 'static.json', result)


def verify_static(evidence):
    import validation_stages
    evidence=validation_stages.static_base(evidence)
    E.require(json.loads((evidence / 'static.json').read_text()) == static_invariants(),
              'static invariant receipt differs')
    spec = importlib.util.spec_from_file_location('memory_inventory_replay', ROOT / 'scripts/memory-source-inventory.py')
    inventory = importlib.util.module_from_spec(spec); spec.loader.exec_module(inventory)
    actual = inventory.scan()
    E.require(json.loads((evidence / 'inventory.json').read_text()) == actual and
              json.loads((ROOT / ('docs/memory/stack-guard-source-inventory.json' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks' else 'docs/memory/kernel-image-source-inventory.json' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='kernel-image-protection' else 'docs/memory/low-address-source-inventory.json' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='low-address-isolation' else 'docs/memory/direct-map-source-inventory.json' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='verified-ram-direct-map' else 'docs/memory/source-inventory.json')).read_text()) == actual,
              'current source inventory differs')
    commands = {'diff-check': ['git', 'diff', '--check'],
                'shell-syntax': ['bash', '-n', 'scripts/qemu-run.sh', 'scripts/qemu-agent.sh', 'scripts/test-page-table-inspector.sh'],
                'inventory': ['python3', 'scripts/memory-source-inventory.py', '--output']}
    seen = set()
    for path in (evidence / 'attempts').glob('*/command.json'):
        name = path.parent.name.split('Z-', 1)[-1]
        if name not in commands: continue
        record = json.loads(path.read_text()); argv = record['argv']
        expected = commands[name]
        E.require(name not in seen and (argv[:3] == expected and len(argv) == 4
                  and Path(argv[3]).name == 'inventory.json' if name == 'inventory' else argv == expected),
                  'static command inventory/options differ')
        E.require(record['exit_status'] == 0 and record['build_input_sha256'] == E.identity(E.manifest()) and
                  record['started_ns'] < record['finished_ns'] and
                  sha(path.with_name('stdout.log')) == record['stdout_sha256'], 'static command raw proof differs')
        seen.add(name)
    E.require(seen == set(commands), 'missing static command proof')
    return {'status': 'PASS', 'checks': sorted(seen), 'source_inventory_sha256': actual['source_sha256']}


def verify_baseline(evidence, replayed_series):
    recorded = json.loads((evidence / 'baseline.json').read_text())
    current = dict(replayed_series)
    prior_verifier = recorded.pop('replay_verifier_sha256')
    current_verifier = current.pop('replay_verifier_sha256')
    # A harness revision can replay unchanged measurements. Preserve both
    # verifier identities while requiring every measurement and scope to agree.
    E.require(re.fullmatch(r'[0-9a-f]{64}', prior_verifier) and recorded == current,
              'baseline receipt differs from replayed series')
    return {'collected_replay_verifier_sha256': prior_verifier,
            'current_replay_verifier_sha256': current_verifier}


def host(evidence):
    directory = evidence / 'host'
    directory.mkdir(exist_ok=False)
    for name, flags in [('normal', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all']),
                        ('asan', ['-fsanitize=address', '-fno-omit-frame-pointer'])]:
        binary = directory / name
        run(evidence, 'host-' + name + '-build', ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            *flags, '-I.', 'scripts/memory-observation-host.c', 'kernel/src/memory/page_table_decode.c',
            'kernel/src/memory/memory_observe_decode.c', '-o', str(binary)])
        run(evidence, 'host-' + name, [str(binary)], {'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
        for subject, sources, extra in [
            ('bitmap-integrity', ['scripts/bitmap-integrity-host.c', 'kernel/src/utils/bitmap.c'], []),
            ('pmem-integrity', ['scripts/pmem-integrity-host.c', 'kernel/src/utils/bitmap.c'],
             ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']),
            ('cpu-mailbox', ['scripts/cpu-control-mailbox-host.c'], ['-pthread']),
            ('allocator-reference', ['scripts/alloc-hardening-host.c', 'kernel/src/utils/bitmap.c'], []),
        ]:
            binary = directory / (subject + '-' + name)
            run(evidence, subject + '-' + name + '-build', ['gcc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                *flags, *extra, *sources, '-o', str(binary)])
            run(evidence, subject + '-' + name, [str(binary)], {'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
    E.write(evidence / 'host.json', {'status': 'PASS', 'profiles': ['normal', 'ubsan', 'asan'],
            'subjects': ['shared-decoder', 'bitmap-integrity', 'pmem-integrity', 'cpu-mailbox', 'allocator-reference'],
            'harness_sha256': E.identity(E.manifest(harness=True)), 'source_sha256': E.identity(E.manifest())})


def verify_local_candidate(directory):
    directory = candidates.resolve(directory)
    candidate = json.loads((directory / 'candidate.json').read_text())
    receipt = json.loads((directory / 'build-receipt.json').read_text())
    candidate_id = E.verify_candidate(candidate, receipt)
    for name, r in candidate['artifacts'].items():
        E.require((directory / name).stat().st_size == r['size'] and sha(directory / name) == r['sha256'], 'frozen binary changed')
    with tempfile.TemporaryDirectory(prefix='memory-verify-payload-') as tmp:
        for name, guest in [('kernel.elf', '::/kernel.elf'), ('BOOTX64.EFI', '::/EFI/BOOT/BOOTX64.EFI'),
                ('zap-light16.psf', '::/EFI/fonts/zap-light16.psf'), ('logo.bmp', '::/EFI/images/logo.bmp'),
                ('startup.nsh', '::/startup.nsh')]:
            target = Path(tmp) / name
            subprocess.run(['mcopy', '-i', str(directory / 'hobbyos.img'), guest, str(target)], check=True)
            E.require({'size': target.stat().st_size, 'sha256': sha(target)} == receipt['payload']['files'][name], 'actual frozen FAT payload differs')
    for item in receipt['commands']:
        E.require(sha(directory / item['receipt']) == item['receipt_sha256'] and
                  sha(directory / item['output']) == item['output_sha256'] and
                  json.loads((directory / item['receipt']).read_text())['exit_status'] == 0, 'build raw command proof')
    return candidate_id


def code_generation(directory):
    directory = candidates.resolve(directory)
    undefined = subprocess.check_output(['nm', '-u', str(directory / 'kernel.elf')], text=True)
    E.require(not undefined.strip(), 'undefined final kernel symbol')
    results = {}
    for function in ('bitmap_validate_allocation', 'pmm_validate_integrity'):
        text = subprocess.check_output(['objdump', '-d', '--disassemble=' + function, str(directory / 'kernel.elf')], text=True)
        E.require('<' + function + '>:' in text and not re.search(r'\b(?:popcnt\w*|[xyz]mm\d+|bitmap_get|__popcount\w*)\b', text),
                  'unapproved integrity scan lowering')
        path = directory / (function + '.disassembly.txt')
        path.write_text(text)
        results[function] = sha(path)
    E.write(directory / 'integrity-codegen.json', {'status': 'PASS', 'elf_sha256': sha(directory / 'kernel.elf'),
            'undefined_symbols': [], 'disassembly': results})


def adopt_build(evidence, origin):
    """Reuse an independently replayed complete build, retaining its raw commands."""
    import validation_resources
    original = json.loads((origin / 'build.json').read_text())
    E.require(original == {'status': 'PASS', 'candidates': ['debug', 'production', 'selftest']}, 'reused build coverage')
    identities = {}
    for profile in original['candidates']:
        source = origin / 'candidates' / profile
        identities[profile] = verify_local_candidate(source)
        target = evidence / 'candidates' / profile
        if not target.exists(): shutil.copytree(source, target, copy_function=validation_resources.sparse_copy)
        E.require(verify_local_candidate(target) == identities[profile], 'reused candidate receipt differs')
        if not (target / 'integrity-codegen.json').exists(): code_generation(target)
    required = {'deps-check', 'debug-stack', 'production-stack', 'selftest-stack'}
    proofs = []
    destination = evidence / 'reused-build-commands'; destination.mkdir(exist_ok=False)
    for path in sorted((origin / 'attempts').glob('*/command.json')):
        name = path.parent.name.split('Z-', 1)[-1]
        if name not in required: continue
        record = json.loads(path.read_text())
        E.require(record['exit_status'] == 0, 'reused build check failed')
        if name.endswith('-stack'): E.require('stack-check: PASS' in path.with_name('stdout.log').read_text(), 'stack proof absent')
        target = destination / name; shutil.copytree(path.parent, target)
        proofs.append({'name': name, 'receipt_sha256': sha(target / 'command.json'), 'stdout_sha256': sha(target / 'stdout.log')})
    E.require({p['name'] for p in proofs} == required, 'reused canonical prerequisite/stack coverage')
    E.write(evidence / 'build-reuse.json', {'status': 'PASS', 'origin': str(origin), 'candidate_ids': identities, 'proofs': proofs})
    E.write(evidence / 'build.json', original)


def build(evidence, run_host=True):
    run(evidence, 'deps-check', ['make', 'deps-check'])
    if run_host: host(evidence)
    for name, flags in [('debug', ['DEBUG_ASSERT=1', 'SELFTEST=0']),
                        ('production', ['DEBUG_ASSERT=0', 'SELFTEST=0']),
                        ('selftest', ['DEBUG_ASSERT=1', 'SELFTEST=1', 'SELFTEST_AUTORUN=0'])]:
        import irq_latency_current as observer
        flags += observer.build_flags(name)
        inputs_before = E.identity(E.manifest())
        attempts = [run(evidence, name + '-clean', ['make', 'clean'])]
        if name == 'production':
            attempts.append(run(evidence, name + '-image', ['make', 'production-image', 'JOBS=4']))
        else:
            attempts.append(run(evidence, name + '-kernel', ['make', 'kernel-check', 'JOBS=4', *flags]))
            attempts.append(run(evidence, name + '-image', ['make', 'image', 'JOBS=4', *flags]))
        directory = evidence / 'candidates' / name
        save_candidate(directory, flags, inputs_before, attempts)
        code_generation(directory)
        if name != 'selftest': write(directory / 'absence.json', absent(directory, name == 'debug'))
        run(evidence, name + '-stack', ['make', 'stack-check', 'JOBS=4', *flags])
    write(evidence / 'build.json', {'status': 'PASS', 'candidates': ['debug', 'production', 'selftest']})


def runtime_command_arguments(commands, scope, serial_backend):
    selected = ['rammap high' if command == 'rammap' and scope in ('high-ram','high-ram-fallback') else command for command in (commands or [])]
    arguments = [value for command in selected for value in ('--command', command)]
    if scope in ('high-ram','high-ram-fallback'):
        arguments += ['--ram-profile', 'high' if scope == 'high-ram' else 'high-fallback']
    if scope == 'hub-keyboard': arguments += ['--device-profile', 'hub-keyboard']
    if serial_backend is not None:
        E.require(serial_backend in ('socket', 'file'), 'unknown serial evidence backend')
        arguments += ['--serial-backend', serial_backend]
    return arguments


def runtime(evidence, baseline=False, nx_disabled=False, candidate=None, validation=None, scope=None, commands=None, serial_backend=None):
    import validation_resources as resources
    import validation_stages as stages
    E.require(not (baseline and nx_disabled), 'NX supplemental boots are not baseline samples')
    candidate = candidates.resolve(candidate or evidence / 'candidates/selftest')
    if not (candidate / 'candidate.json').is_file(): raise ValueError('explicit built SELFTEST candidate is required')
    spec = importlib.util.spec_from_file_location('memory_verifier', ROOT / 'scripts/verify-memory-observation.py')
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
    frozen = json.loads((candidate / 'candidate.json').read_text())
    candidate_id = E.verify_candidate(frozen, json.loads((candidate / 'build-receipt.json').read_text()))
    scope = scope or ('nx-disabled' if nx_disabled else ('baseline' if baseline else 'full-matrix'))
    E.require(scope in verifier.SERIES_SCOPES, 'unknown collection scope')
    if stages.series(sys.modules[__name__], evidence, scope, candidate, validation):
        return
    contract = verifier.SERIES_SCOPES[scope]
    control_profile = 'nx-disabled' if scope == 'nx-disabled' else 'xapic' if scope == 'xapic' else 'normal'
    command_args = runtime_command_arguments(commands, scope, serial_backend)
    journal = evidence / 'campaign-attempts'
    journal.mkdir(exist_ok=True)
    attempts, paths = [], []
    # Reuse completed immutable attempts only after replay. Every new VM gets a
    # new directory; failures remain in the chronology and reset its streak.
    for start_path in sorted(journal.glob('*.json')):
        if start_path.name.endswith('.result.json'): continue
        start = json.loads(start_path.read_text())
        finish = json.loads(start_path.with_suffix('.result.json').read_text())
        E.require(start['candidate_id'] == candidate_id and finish['start_sha256'] == sha(start_path),
                  'resumed attempt candidate or start receipt differs')
        attempt = dict(start, status=finish['status'], finished_ns=finish['finished_ns'],
                       **verifier.attempt_clock(evidence / start['path'], start, finish))
        if finish['status'] == 'PASS':
            directory = evidence / start['path']
            validated = verifier.verify_run(directory / 'qemu-serial.log')
            if validation is not None: validation(directory)
            E.require(finish['result_sha256'] == sha(directory / 'result.json'), 'resumed result changed')
            validated['path'] = start['path']; attempt['validated'] = validated
        attempts.append(attempt); paths.append(start_path.relative_to(evidence).as_posix())
    E.require([a['sequence'] for a in attempts] == list(range(1,len(attempts)+1)), 'resumed attempt sequence missing')
    order=list(contract)
    if scope=='tlb-matrix':
        first=['q35-tcg-smp1','q35-tcg-smp4','q35-kvm-smp4']
        order=first+[name for name in order if name not in first]
    for profile in order:
        repetitions=contract[profile]
        machine, accel, cpu = profile.split('-')[:3]; smp = int(cpu[3:])
        streak = 0
        for attempt in attempts:
            if attempt['profile'] == profile:
                streak = streak + 1 if attempt['status'] == 'PASS' else 0
        for repeat in range(streak + 1, repetitions + 1):
            resources.before_step(evidence, 'runtime')
            sequence = len(attempts) + 1
            name = f'{profile}-{repeat}'
            if (evidence / 'profiles' / name).exists(): name += '-' + stamp()
            relative = 'profiles/' + name; directory = evidence / relative
            start = {'schema': 1, 'sequence': sequence, 'path': relative, 'profile': profile,
                     'candidate_id': candidate_id, 'started_ns': time.monotonic_ns(), 'started_utc': stamp()}
            if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
                start['host_boot_id'] = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
            start_path = journal / f'{sequence:04d}.json'
            E.write(start_path, start)
            finish = {'start_sha256': sha(start_path), 'status': 'FAIL'}
            try:
                run(evidence, name, ['python3', 'scripts/memory-qemu.py', '--evidence', str(directory),
                    '--candidate', str(candidate), '--machine', machine, '--accel', accel, '--smp', str(smp),
                    '--control-profile', control_profile, *command_args])
                validated = verifier.verify_run(directory / 'qemu-serial.log')
                if validation is not None: validation(directory)
                validated['path'] = relative
                finish.update(status='PASS', result_sha256=sha(directory / 'result.json'))
            except Exception as error:
                finish['reason'] = str(error)
                raise
            finally:
                finish['finished_ns'] = time.monotonic_ns()
                E.write(start_path.with_suffix('.result.json'), finish)
                if (directory / 'result.json').exists():
                    raw = json.loads((directory / 'result.json').read_text())
                    if raw.get('cleanup'): retention.retire_run(directory)
            attempts.append(dict(start, status='PASS', finished_ns=finish['finished_ns'], validated=validated,
                                 **verifier.attempt_clock(directory, start, finish)))
            paths.append(start_path.relative_to(evidence).as_posix())
    diagnostics = []
    if scope in ('full-matrix', 'tlb-matrix'):
        for completed in sorted((evidence / 'diagnostics').glob('*/result.json')):
            observed = json.loads(completed.read_text())
            if (observed.get('status') == 'COLLECTED' and observed.get('readback') and
                observed.get('candidate_id') == candidate_id and observed.get('profile') == 'q35-kvm-smp4'):
                try:
                    verifier.verify_run(completed.parent / 'qemu-serial.log')
                    if validation is not None: validation(completed.parent)
                except (ValueError, KeyError, OSError, AssertionError) as error:
                    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') not in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'): raise
                    E.write(evidence / 'diagnostic-rejections' / (stamp()+'.json'), {
                        'path':completed.parent.relative_to(evidence).as_posix(),
                        'result_sha256':sha(completed), 'status':'REJECTED', 'reason':str(error),
                        'verifier_sha256':E.identity(E.manifest(harness=True))})
                    continue
                diagnostics.append(completed.parent.relative_to(evidence).as_posix())
        if diagnostics:
            diagnostics = diagnostics[-1:]
    if scope in ('full-matrix', 'tlb-matrix') and not diagnostics:
        resources.before_step(evidence, 'runtime')
        name = 'diagnostics/q35-kvm-smp4-readback-' + stamp()
        directory = evidence / name
        run(evidence, 'independent-readback', ['python3', 'scripts/memory-qemu.py', '--evidence', str(directory),
            '--candidate', str(candidate), '--machine', 'q35', '--accel', 'kvm', '--smp', '4', '--readback',
            *(['--low-bootstrap-readback'] if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else []), *command_args])
        verifier.verify_run(directory / 'qemu-serial.log'); diagnostics.append(name)
        if validation is not None: validation(directory)
        retention.retire_run(directory)
    selected = verifier.series_policy(attempts, scope)
    harness = E.manifest(harness=True)
    diagnostic_attempts = [{'path': p.parent.relative_to(evidence).as_posix(), 'result_sha256': sha(p),
                            'status': json.loads(p.read_text())['status']}
                           for p in sorted((evidence / 'diagnostics').glob('*/result.json'))]
    manifest = {'schema': 2, 'scope': scope, 'attempts': paths, **selected, 'diagnostics': diagnostics,
                'diagnostic_attempts': diagnostic_attempts,
                'verifier_sources': harness, 'verifier_sha256': E.identity(harness)}
    verifier.verify_series(evidence, manifest)
    E.write(evidence / 'series.json', manifest)
    replay = verifier.verify_series(evidence)
    E.write(evidence / ('baseline.json' if baseline else 'runtime.json'), replay)


def verifier():
    spec = importlib.util.spec_from_file_location('memory_verify_current', ROOT / 'scripts/verify-memory-observation.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def production(evidence):
    E.require(not (evidence / 'production.json').exists(), 'production mode already finalized; use a new evidence directory')
    directory = evidence / 'candidates/production'
    candidate_id = verify_local_candidate(directory)
    destination = evidence / 'production-attempts' / stamp()
    destination.mkdir(parents=True, exist_ok=False)
    E.write(destination / 'absence.json', absent(directory))
    runs = []
    for accel, smp in [('tcg', 1), ('kvm', 4)]:
        name = f'q35-{accel}-smp{smp}'; target = destination / name
        run(evidence, 'production-' + name, ['python3', 'scripts/memory-qemu.py', '--evidence', str(target),
            '--candidate', str(directory), '--machine', 'q35', '--accel', accel, '--smp', str(smp), '--production'])
        verifier().verify_run(target / 'qemu-serial.log')
        runs.append(target.relative_to(evidence).as_posix())
    E.write(evidence / 'production.json', {'status': 'PASS', 'candidate_id': candidate_id, 'runs': runs})


def fixtures(evidence):
    directory = evidence / 'fixture-attempts' / stamp(); directory.mkdir(parents=True)
    host(directory)
    run(directory, 'decoder-verifier-fixtures', ['python3', 'scripts/memory-observation-fixtures.py', '--evidence', str(directory / 'decoder-verifier')])
    run(directory, 'interrupt-fixtures', ['python3', 'scripts/memory-interrupt-fixtures.py', '--output', str(directory / 'interrupt')])
    run(directory, 'packaging-fixtures', ['python3', 'scripts/memory-packaging-fixtures.py', '--output', str(directory / 'packaging')])
    run(directory, 'ELF-fragment-fixtures', ['python3', 'scripts/memory_elf_proof.py', '--elf', str(candidates.artifact(evidence / 'candidates/selftest/kernel.elf')), '--output', str(directory / 'elf-structure')])
    series = json.loads((evidence / 'series.json').read_text())
    capture = evidence / series['diagnostics'][0]
    run(directory, 'capture-mutations', ['python3', 'scripts/memory-replay-fixtures.py', '--source', str(capture), '--output', str(directory / 'capture-recipes')])
    run(directory, 'semantic-and-aggregate-fixtures', ['python3', 'scripts/memory-integrity-fixtures.py', '--capture', str(capture),
        '--series', str(evidence), '--production', str(evidence / json.loads((evidence / 'production.json').read_text())['runs'][0]),
        '--output', str(directory / 'semantic')])
    result = {'status': 'PASS', 'directory': directory.relative_to(evidence).as_posix(),
              'harness_sha256': E.identity(E.manifest(harness=True)), 'source_sha256': E.identity(E.manifest())}
    E.write(directory / 'mode.json', result)
    return result


def all_modes(evidence, reuse_build=None):
    if not (evidence / 'static.json').exists(): static(evidence)
    verify_static(evidence)
    if not (evidence / 'build.json').exists():
        if reuse_build: adopt_build(evidence, reuse_build.resolve())
        else: build(evidence)
    for name in ('debug', 'production', 'selftest'): verify_local_candidate(evidence / 'candidates' / name)
    if not (evidence / 'series.json').exists(): runtime(evidence)
    series = verifier().verify_series(evidence)
    if not (evidence / 'baseline.json').exists(): E.write(evidence / 'baseline.json', series)
    verify_baseline(evidence, series)
    if not (evidence / 'production.json').exists(): production(evidence)
    run(evidence, 'current-inherited-regression', ['python3', 'scripts/memory-current-regression.py', 'all', '--evidence', str(evidence)])
    fixture_result = fixtures(evidence)
    # Every final result is derived again after the collector and fixtures finish.
    replay = verifier().verify_series(evidence)
    for name in json.loads((evidence / 'production.json').read_text())['runs']:
        verifier().verify_run(evidence / name / 'qemu-serial.log')
    run(evidence, 'current-regression-replay', ['python3', 'scripts/memory-current-regression.py', 'verify',
        '--evidence', str(evidence), '--source', E.identity(E.manifest())])
    E.require(fixture_result['harness_sha256'] == E.identity(E.manifest(harness=True)), 'verifier changed after fixtures')
    E.write(evidence / 'acceptance.json', {'status': 'PASS', 'contract': 'memory-current-regression-v1',
            'series': replay, 'baseline_replay': verify_baseline(evidence, replay),
            'fixtures': fixture_result, 'hardware': 'DEFERRED_TO_FINAL_CLOSURE'})


def verify_acceptance(evidence):
    result = json.loads((evidence / 'acceptance.json').read_text())
    E.require(result['status'] == 'PASS' and result['contract'] == 'memory-current-regression-v1', 'current acceptance receipt missing')
    verify_static(evidence)
    for name in ('debug', 'production', 'selftest'): verify_local_candidate(evidence / 'candidates' / name)
    E.require(result['series'] == verifier().verify_series(evidence), 'final series replay differs')
    E.require(result['baseline_replay'] == verify_baseline(evidence, result['series']), 'baseline replay identity differs')
    for name in json.loads((evidence / 'production.json').read_text())['runs']:
        verifier().verify_run(evidence / name / 'qemu-serial.log')
    fixture = result['fixtures']; E.safe(fixture['directory'])
    E.require(fixture == json.loads((evidence / fixture['directory'] / 'mode.json').read_text()) and
              fixture['harness_sha256'] == E.identity(E.manifest(harness=True)) and
              fixture['source_sha256'] == E.identity(E.manifest()), 'acceptance fixture verifier/source differs')
    subprocess.run(['python3', 'scripts/memory-current-regression.py', 'verify', '--evidence', str(evidence),
                    '--source', E.identity(E.manifest())], cwd=ROOT, check=True)
    return {'status': 'PASS', 'contract': result['contract']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('static', 'build', 'runtime', 'baseline', 'production', 'fixtures', 'verify', 'all'))
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--logfile', type=Path)
    parser.add_argument('--reuse-build', type=Path)
    args = parser.parse_args()
    os.chdir(ROOT)
    evidence = args.evidence.resolve()
    if args.mode == 'verify':
        if not args.logfile:
            print(json.dumps(verify_acceptance(evidence))); return 0
        cmd = [sys.executable, str(ROOT / 'scripts/verify-memory-observation.py')]
        cmd += ['--logfile', str(args.logfile)] if args.logfile else ['--series', str(evidence)]
        return subprocess.call(cmd)
    evidence.relative_to(ROOT / 'backups')
    evidence.mkdir(parents=True, exist_ok=True)
    try:
        if args.mode == 'static': static(evidence)
        elif args.mode == 'build':
            if args.reuse_build: adopt_build(evidence, args.reuse_build.resolve())
            else: build(evidence)
        elif args.mode == 'runtime': runtime(evidence)
        elif args.mode == 'baseline':
            if (evidence / 'series.json').exists(): E.write(evidence / 'baseline.json', verifier().verify_series(evidence))
            else: runtime(evidence, baseline=True)
        elif args.mode == 'fixtures': fixtures(evidence)
        elif args.mode == 'production': production(evidence)
        else: all_modes(evidence, args.reuse_build)
        print(json.dumps({'mode': args.mode, 'status': 'PASS', 'evidence': str(evidence)}))
        return 0
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print(json.dumps({'mode': args.mode, 'status': 'FAIL', 'reason': str(error)}))
        return 1


if __name__ == '__main__': sys.exit(main())
