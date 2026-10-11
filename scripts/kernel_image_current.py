"""Kernel image protection successor rooted at the accepted no-low commit."""
import json
import os
import subprocess
from pathlib import Path
import memory_evidence as E
import tlb_current as T
import stack_current as STACK

ROOT=E.ROOT
BASE='0c40d612707e90acb64c23fb11a8c5c1e46cf8f2'
TREE='08405e6c8baff3fb08cfb20f2e20bcab40a02acb'
CONTRACT='kernel-image-protection'
def enabled():return os.environ.get('HOBBYOS_VALIDATION_CONTRACT')==CONTRACT or STACK.enabled()

def contract(root=ROOT):
    if STACK.enabled(): return STACK.contract(root)
    value=json.loads((root/'scripts/kernel-image-scope.json').read_text())
    E.require((value['base_commit'],value['base_tree'],value['name'])==(BASE,TREE,CONTRACT),
        'image protection scope approved baseline')
    for name,digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name)==digest,'image protection protected file drift: '+name)
    for name,rows in value['protected_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,digest in rows.items():
            E.require(symbol in actual and E.identity(actual[symbol])==digest,
                'image protection protected function drift: '+name+':'+symbol)
    for name,row in value['promoted_sources'].items():
        E.require(row['purpose'] and row['invariant'] and E.sha(root/name)==row['candidate_sha256'],
            'image protection promoted source differs: '+name)
    for name,rows in value['promoted_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,row in rows.items():
            E.require(row['purpose'] and E.identity(actual.get(symbol))==row['candidate_sha256'],
                'image protection promoted body differs: '+name+':'+symbol)
    T.dispatch_semantics((root/'kernel/src/core/interrupts.c').read_text());T.caller_semantics(root)
    return value

def scope(family):
    if STACK.enabled(): return STACK.scope(family)
    value=contract()
    if ROOT.name=='source' and (ROOT.parent/'source-manifest.json').is_file():
        import device_current
        changed=device_current.portable_changes(ROOT.parent,base=BASE,tree_id=TREE)
    else:
        names=subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=ROOT).decode().split('\0')
        names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
        changed={n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed<=set(value['allowed_paths']),'unrelated image protection edits: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':CONTRACT,'base_commit':BASE,
        'changed_paths':sorted(changed),'scope_contract_sha256':E.sha(ROOT/'scripts/kernel-image-scope.json'),
        'protected':value['immutable_reviewed_anchors']}

def audit_sites(root=ROOT):
    if STACK.enabled(): return STACK.audit_sites(root)
    expected=contract(root)['instruction_sites'];actual=T.instruction_sites(root)
    E.require(actual==expected,'unauthorized architectural instruction site: image protection')
    return {'sites':len(actual),'sha256':E.identity(actual)}

def source_guard(root=ROOT):
    if STACK.enabled(): return STACK.source_guard(root)
    import device_current
    return device_current.source_guard(root,inventory='kernel-image-conversion-sites.json',base=BASE)

def portable_changes(package):
    if STACK.enabled(): return STACK.portable_changes(package)
    import device_current
    return device_current.portable_changes(package,base=BASE,tree_id=TREE)

def boundary_semantics(root=ROOT):
    if STACK.enabled(): return STACK.boundary_semantics(root)
    import low_current
    value=contract(root);guard=source_guard(root)
    low_current.transition_semantics((root/'kernel/src/memory/paging.c').read_text(),
        (root/'kernel/src/memory/paging_build.c').read_text(),
        (root/'kernel/src/smp/smp_boot.c').read_text(),(root/'kernel/src/core/entry.S').read_text())
    builder=T.functions((root/'kernel/src/memory/paging_build.c').read_text())
    E.require('image_permissions_check_leaf(' in builder['verify_table'] and
        'inherited_write && v.writable' in builder['verify_table'] and
        'inherited_nx || v.nx' in builder['verify_table'],'image effective-rights verifier absent')
    init=T.functions((root/'kernel/src/memory/paging.c').read_text())['init_paging']
    E.require(init.index('image_permissions_from_linker(')<init.index('paging_build_create(')<
        init.index('paging_build_verify(')<init.index('paging_build_activate('),
        'image permission construction/activation order')
    E.require('image_permissions_check_leaf(' in (root/'kernel/src/memory/image_walk.c').read_text(),
        'whole-main-root expected permission predicate absent')
    return {'scope':value['name'],'guard':guard,'lower_half':'ALL_256_ROOT_ENTRIES_ZERO',
        'bootstrap':'PRESERVED_PRIVATE_SINGLE_PAGE_LIFETIME','image_permissions':'CONSTRUCTION_ONLY_ALL_ALIASES',
        'nx_absent':'WRITE_PROTECTION_ONLY','direct_map':'PRESERVED','hardware':'DEFERRED_TO_FINAL_CLOSURE'}
