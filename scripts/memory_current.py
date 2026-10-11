"""Narrow current-source boundary for rolling memory diagnostic validation.

Historical wrappers retain their original scope and literal anchors. Only named
current entry points use this boundary; runtime and semantic validators are shared.
"""
import json
import os
import subprocess
import sys
from pathlib import Path
import memory_evidence as E

ROOT = Path(__file__).resolve().parent.parent


def current_contract(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.contract(root)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'physical-translation':
        import physical_current
        return physical_current.contract(root)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'tlb-coherency':
        import tlb_current
        return tlb_current.contract(root)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'cpu-control-state':
        import cpu_current
        return cpu_current.contract(root)
    return json.loads((root / 'scripts/memory-current-scope.json').read_text())

MAPPINGS = {
    'assertion-record': 'original whole-worktree scope and unrelated build-file anchors',
    'taskman-statistics': 'original whole-worktree scope and unrelated makefile anchor',
    'section-layout': 'lost reference ELFs replaced by current structure and same-object relink',
    'taskman-visual': 'original branch, candidate hashes and exact historical frame count',
    'interrupts': 'original campaign branch and evidence destination',
    'clockevent': 'original campaign branch and evidence destination',
    'modal-heap': 'legacy failure classification and superseded internal global-heap equality replaced by the maintained ownership and outer memory-checkpoint validators',
}


def source_manifest(path, root=ROOT):
    """Compare captured source anchors under the named rolling contract.

    Linked inputs and unrelated files must remain identical. An authorized
    harness/document revision is recorded separately and requires fresh oracle
    fixtures plus replay in the top-level acceptance collector.
    """
    contract = current_contract(root)
    linked = set(E.input_paths(root)); seen = set(); revisions = []
    for line in path.read_text().splitlines():
        digest, separator, name = line.partition('  ')
        E.require(separator and len(digest) == 64 and name not in seen, 'current source anchor format/duplicate')
        E.safe(name); seen.add(name)
        actual = E.sha(root / name)
        if actual == digest: continue
        E.require(name in contract['allowed_paths'] and name not in linked and
                  (name.startswith(('scripts/', 'docs/memory/')) or name == 'docs/foundation/validation-policy.md'),
                  'current linked/protected source drift: ' + name)
        revisions.append({'path': name, 'captured_sha256': digest, 'replay_sha256': actual})
    E.require(seen, 'current source anchors empty')
    for name, digest in contract['immutable_reviewed_anchors'].items():
        E.require(E.sha(root / name) == digest, 'current protected anchor drift: ' + name)
    return revisions


def scope(family):
    E.require(family in MAPPINGS, 'unnamed current authority mapping')
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.scope(family)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'physical-translation':
        import physical_current
        return physical_current.scope(family)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'tlb-coherency':
        import tlb_current
        return tlb_current.scope(family)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'cpu-control-state':
        import cpu_current
        return cpu_current.scope(family)
    contract = json.loads((ROOT / 'scripts/memory-current-scope.json').read_text())
    names = subprocess.check_output(['git', 'diff', '--name-only', '-z', contract['base_commit']], cwd=ROOT).decode().split('\0')
    names += subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
    changed = set(n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES)
    E.require(changed <= set(contract['allowed_paths']), 'unrelated current source edits: ' + ', '.join(sorted(changed - set(contract['allowed_paths']))))
    for name, expected in contract['immutable_reviewed_anchors'].items():
        E.require(E.sha(ROOT / name) == expected, 'immutable reviewed anchor changed: ' + name)
    return {'schema': 1, 'status': 'PASS', 'family': family, 'superseded_scope': MAPPINGS[family],
            'scope_contract_sha256': E.sha(ROOT / 'scripts/memory-current-scope.json'),
            'base_commit': contract['base_commit'], 'changed_paths': sorted(changed),
            'protected': contract['immutable_reviewed_anchors']}


if __name__ == '__main__':
    print(json.dumps(scope(sys.argv[1]), sort_keys=True))
