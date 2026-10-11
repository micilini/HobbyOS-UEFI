"""Explicit observer successor scope; historical stack validation stays isolated."""
import json
import os
import subprocess
from pathlib import Path
import memory_evidence as E

BASE = 'd68a3b4a2c97f55dfdcc445bfe42317e09e1c55e'
TREE = '3ce6be4fcae7c23fd41adb439e081af72ea4717c'
ROLE = 'irq-off-latency'


def enabled():
    selected = os.environ.get('HOBBYOS_MEMORY_OBSERVER_ROLE')
    E.require(selected in (None, ROLE), 'unknown memory observer role')
    if selected:
        E.require(os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'guarded-kernel-stacks',
                  'observer requires explicit inherited guarded-stack role')
    return selected == ROLE


def contract(root=E.ROOT):
    import stack_current as C
    import tlb_current as T
    value = json.loads((root/'scripts/irq-latency-scope.json').read_text())
    E.require((value['base_commit'], value['base_tree'], value['name']) == (BASE, TREE, ROLE),
              'observer scope approved pre-edit identity')
    for name, digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name) == digest, 'observer protected file drift: '+name)
    for name, row in value['promoted_sources'].items():
        E.require(row['purpose'] and row['invariant'] and E.sha(root/name) == row['candidate_sha256'],
                  'observer promoted source differs: '+name)
    for name, functions in value['protected_functions'].items():
        actual = C.functions((root/name).read_text())
        for symbol, digest in functions.items():
            E.require(symbol in actual and E.identity(actual[symbol]) == digest,
                      'observer protected function drift: '+name+':'+symbol)
    T.dispatch_semantics((root/'kernel/src/core/interrupts.c').read_text())
    T.caller_semantics(root)
    return value


def scope(family):
    value = contract()
    if E.ROOT.name == 'source' and (E.ROOT.parent/'source-manifest.json').is_file():
        import device_current
        changed = device_current.portable_changes(E.ROOT.parent, base=BASE, tree_id=TREE)
    else:
        names = subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=E.ROOT).decode().split('\0')
        names += subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=E.ROOT).decode().split('\0')
        changed = {n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed <= set(value['allowed_paths']), 'unrelated observer edit: '+
              ', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':ROLE,'base_commit':BASE,
            'base_tree':TREE,'changed_paths':sorted(changed),
            'scope_contract_sha256':E.sha(E.ROOT/'scripts/irq-latency-scope.json'),
            'protected':value['immutable_reviewed_anchors']}


def audit_sites(root=E.ROOT):
    import tlb_current as T
    actual = T.instruction_sites(root)
    E.require(actual == contract(root)['instruction_sites'], 'unauthorized architectural instruction site: observer')
    return {'sites':len(actual),'sha256':E.identity(actual)}


def source_guard(root=E.ROOT):
    import device_current
    return device_current.source_guard(root, inventory='irq-latency-conversion-sites.json', base=BASE)


def build_flags(role):
    E.require(role in ('selftest','framebuffer-reference','debug','production'), 'unknown observer build role')
    return ['IRQ_LATENCY='+str(int(role in ('selftest','framebuffer-reference')))] if enabled() else []
