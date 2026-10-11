#!/usr/bin/env python3
"""Run actual owned-mapping and DMA C tests; this is not runtime acceptance."""
import argparse
import importlib.util
import os
import json
from pathlib import Path
import sys
import memory_evidence as E

ROOT = Path(__file__).resolve().parent.parent


def expected_summary(subject):
    if subject=='mapping':return 'OWNED_MAPPING_HOST checks=396079 status=PASS\n'
    E.require(subject=='dma','unknown device hosted subject')
    source=ROOT/'kernel/src/memory/pmem.c';text=source.read_text();checks=8672
    if 'PMM_OBSERVATION_FRAMES' in text:
        contract=json.loads((ROOT/'scripts/irq-latency-production-changes.json').read_text())
        row=contract['units']['kernel/src/memory/pmem.c']
        qualified_clock='$(KERNEL_DIR)/src/core/clocksource.c' in (ROOT/'makefile').read_text()
        E.require(contract['schema']==(2 if qualified_clock else 1) and row['kind']=='measured-local-correction' and
            row['current_sha256']==E.sha(source) and '#define PMM_OBSERVATION_FRAMES 4096u' in text,
            'device hosted PMM observation source contract')
        if qualified_clock:
            names={'kernel/src/core/clocksource.c','kernel/src/core/pvclock.c'}
            E.require(set(contract.get('new_units',{}))==names and all(
                contract['new_units'][n]['current_sha256']==E.sha(ROOT/n) and
                contract['new_units'][n]['kind']=='qualified-clocksource' for n in names),
                'device hosted clocksource production identity')
        # Four existing complete high-frame observations, two unchanged lock
        # assertions per additional chunk. Every original DMA assertion remains.
        checks+=4*2*(268435456//4096-1)
    return f'DEVICE_DMA_HOST checks={checks} logical_frames=268435456 privileged_operations=SUBSTITUTED status=PASS\n'


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = value
    spec.loader.exec_module(value)
    return value


def run(output, inherited=False):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    gate = module('memory-observation-gate')
    subjects = [
        ('mapping', ['scripts/device-mapping-host.c', 'kernel/src/memory/mapping_core.c'], [],
         'OWNED_MAPPING_HOST checks=396079 status=PASS\n'),
        ('dma', ['scripts/device-dma-host.c', 'kernel/src/utils/bitmap.c',
                 'kernel/src/memory/mapping_core.c', 'kernel/src/memory/dma_buffer.c',
                 'kernel/src/drivers/usb/xhci/xhci_dma.c'],
         ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-DHOBBYOS_DEBUG_ASSERT=0'],
         'DEVICE_DMA_HOST checks=8672 logical_frames=268435456 privileged_operations=SUBSTITUTED status=PASS\n')]
    results = []
    for mode, flags in [('normal', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all']),
                        ('asan', ['-fsanitize=address', '-fno-omit-frame-pointer'])]:
        for subject, sources, extra, expected in subjects:
            expected=expected_summary(subject)
            binary = output / (subject + '-' + mode)
            built = gate.run(output, binary.name + '-build', [
                'gcc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie',
                *extra, *flags, *sources, '-o', str(binary)])
            executed = gate.run(output, binary.name, [str(binary)],
                                {'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            E.require((executed / 'stdout.log').read_text() == expected, 'actual C test summary differs')
            results.append({'subject': subject, 'mode': mode, 'binary_sha256': E.sha(binary),
                            'build': built.relative_to(output).as_posix(),
                            'execute': executed.relative_to(output).as_posix(), 'summary': expected.strip()})
    if inherited:
        prior = output / 'inherited'
        prior.mkdir()
        gate.host(prior)
        module('cpu-control-state-gate').host(prior, inherited=False)
    result = {'schema': 1, 'status': 'PASS', 'authority': 'Actual hosted C only; privileged operations substituted',
              'build_input_sha256': E.identity(E.manifest()), 'harness_sha256': E.identity(E.manifest(harness=True)),
              'subjects': results, 'inherited_observation_allocator_mailbox_policy': inherited,
              'runtime_acceptance': 'NOT_ESTABLISHED'}
    E.write(output / 'device-host.json', result)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inherited', action='store_true')
    args = parser.parse_args()
    os.chdir(ROOT)
    print(E.canonical(run(args.output, args.inherited)).decode(), end='')
