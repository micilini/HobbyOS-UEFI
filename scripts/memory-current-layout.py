#!/usr/bin/env python3
"""Current layout authority: structure, identical-object relink and native fixtures.

The original reference-binary campaign and its historical modes remain separate.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import memory_evidence as E
import memory_current

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('layout_validator', ROOT / 'scripts/verify-section-layout.py')
V = importlib.util.module_from_spec(spec); sys.modules[spec.name] = V; spec.loader.exec_module(V)
ROLES = V.REQUIRED_CANDIDATE_ROLES | {'architecture-instrumented', 'architecture-negative'}
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
    ROLES=ROLES|{'framebuffer-reference'}


def verify(root):
    campaign = V.load_json(root / 'campaign.json')
    E.require(campaign['kind'] == 'section-layout-current-evidence' and campaign['references'] == [],
              'current layout must not impersonate historical references')
    summaries = []
    for profile in ('debug-on', 'debug-off'):
        pair = V.load_json(root / campaign['pairs'][profile])
        objects = V.load_json(root / pair['objects'])['objects']
        E.require(objects and len({r['path'] for r in objects}) == len(objects), 'layout object inventory')
        for row in objects:
            V.require_hash(root / row['copy_path'], row['sha256'])
        for side in ('before', 'after'):
            record = pair[side]
            V.require_hash(root / record['elf'], record['sha256'])
            V.require_hash(root / record['map'], record['map_sha256'])
            V.validate_layout(V.ElfFile.from_path(root / record['elf']))
            V.validate_map_policy(root / record['map'], record['elf'], [r['path'] for r in objects], True)
        E.require(pair['before']['sha256'] == pair['after']['sha256'] == pair['after']['canonical_sha256'],
                  'same-object current relink differs')
        V.require_hash(root / pair['after']['canonical_elf'], pair['after']['canonical_sha256'])
        V.compare_trampoline(V.ElfFile.from_path(root / pair['before']['elf']), V.ElfFile.from_path(root / pair['after']['elf']))
        summaries.append({'profile': profile, 'elf_sha256': pair['after']['sha256'], 'objects': len(objects)})
    V.validate_orphan_probe(root, V.load_json(root / campaign['orphan_probe']))
    rows = campaign['candidates']
    E.require(len(rows) == len(ROLES) and {r['role'] for r in rows} == ROLES, 'missing/duplicate current layout role')
    for row in rows:
        V.verify_candidate(root, row)
        flags = row['configuration']
        if row['role'].startswith('architecture-'):
            E.require(flags.get('ARCH_TEST') == 1 and flags.get('SELFTEST') == 1 and flags.get('DEBUG_ASSERT') == 1,
                      'architecture layout role flags')
            E.require(flags.get('ARCH_NEGATIVE_GP', 0) == int(row['role'] == 'architecture-negative'), 'architecture destructive layout role')
        if row['role']=='framebuffer-reference':
            E.require(flags=={'SELFTEST':1,'DEBUG_ASSERT':1,'SELFTEST_AUTORUN':0,'FRAMEBUFFER_REFERENCE':1},
                      'framebuffer reference layout role flags')
            symbols={s['name'] for s in V.ElfFile.from_path(root/row['elf']).symbols()}
            E.require('mmio_framebuffer_reference' in symbols,'framebuffer reference code absent from layout role')
    return {'status': 'PASS', 'contract': 'current-layout', 'pairs': summaries,
            'roles': sorted(ROLES), 'historical': 'HISTORICAL_REPLAY_UNAVAILABLE'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('collect', 'verify'))
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--candidate-manifest', type=Path)
    args = parser.parse_args(); root = args.evidence.resolve()
    if args.mode == 'collect':
        scope = memory_current.scope('section-layout')
        E.require(args.candidate_manifest and args.candidate_manifest.is_file(), 'current role manifest required')
        V.collect_core(ROOT, root, scope['base_commit'], [], current=True)
        V.collect_candidates(ROOT, root, args.candidate_manifest.resolve())
        result = verify(root)
        V.run_fixtures(root, root / 'fixtures')
        E.write(root / 'current-result.json', result)
        E.write(root / 'current-scope.json', scope)
    else:
        result = verify(root)
        E.require(V.load_json(root / 'current-result.json') == result, 'current layout replay differs')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__': main()
