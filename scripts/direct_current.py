"""Pinned direct-map successor, rooted at the accepted owned-device tree."""
import importlib.util
import json
import subprocess
from pathlib import Path
import memory_evidence as E
import tlb_current as T
import low_current as L

ROOT=Path(__file__).resolve().parent.parent
BASE='3210e0f0436c6bcfdf6ee6bfa6129c669696efa1'
TREE='c2672e19df93b945fb4439498231cd7e4b442233'

def contract(root=ROOT):
    if L.enabled(): return L.contract(root)
    value=json.loads((root/'scripts/direct-map-scope.json').read_text())
    E.require((value['base_commit'],value['base_tree'])==(BASE,TREE),'RAM scope approved baseline')
    for name,digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name)==digest,'RAM protected file drift: '+name)
    for name,expected in value['protected_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,digest in expected.items():
            E.require(symbol in actual and E.identity(actual[symbol])==digest,'RAM protected function drift: '+name+':'+symbol)
    for name,row in value['promoted_sources'].items():
        E.require(row['purpose'] and row['invariant'] and E.sha(root/name)==row['candidate_sha256'],
            'RAM candidate anchor drift: '+name)
    for name,rows in value['promoted_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,row in rows.items():
            E.require(row['purpose'] and E.identity(actual[symbol])==row['candidate_sha256'],
                'RAM promoted function differs: '+name+':'+symbol)
    T.dispatch_semantics((root/'kernel/src/core/interrupts.c').read_text());T.caller_semantics(root)
    return value

def scope(family):
    if L.enabled(): return L.scope(family)
    value=contract()
    if ROOT.name=='source' and (ROOT.parent/'source-manifest.json').is_file():
        import device_current
        changed=device_current.portable_changes(ROOT.parent,base=BASE,tree_id=TREE)
    else:
        names=subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=ROOT).decode().split('\0')
        names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
        changed={n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed<=set(value['allowed_paths']),'unrelated RAM mapping edits: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':value['name'],'base_commit':BASE,
        'changed_paths':sorted(changed),'scope_contract_sha256':E.sha(ROOT/'scripts/direct-map-scope.json'),
        'protected':value['immutable_reviewed_anchors'],'superseded_scope':'Verified RAM construction and pointer transition; original device/request/native semantics retained.'}

def audit_sites(root=ROOT):
    if L.enabled(): return L.audit_sites(root)
    expected=contract(root)['instruction_sites'];actual=T.instruction_sites(root)
    E.require(actual==expected,'unauthorized RAM architectural instruction site')
    return {'sites':len(actual),'sha256':E.identity(actual)}

def source_guard(root=ROOT):
    if L.enabled(): return L.source_guard(root)
    import device_current
    return device_current.source_guard(root,inventory='direct-map-conversion-sites.json',base=BASE)

def boundary_semantics(root=ROOT):
    if L.enabled(): return L.boundary_semantics(root)
    value=contract(root);guard=source_guard(root)
    pmm=T.functions((root/'kernel/src/memory/pmem.c').read_text())
    for name in ('pmm_alloc_frame','pmm_alloc_contiguous_frames'):
        E.require('bitmap_find_zero_run_below' in pmm[name] and 'physical_allocation_ceiling()' in pmm[name],
            'RAM original allocation reachability absent')
    E.require('bitmap_find_aligned_run_below' in pmm['allocate_range'] and 'g_alloc_limit_frames' in pmm['allocate_range'] and
        'allocate_range(count, alignment_frames, 0, limit, physical)' in pmm['pmm_alloc_constrained_frames'],
        'RAM constrained production search bypass')
    paging=T.functions((root/'kernel/src/memory/paging.c').read_text())
    init=paging['init_paging']
    E.require(init.index('paging_build_verify(')<init.index('paging_build_activate(')<init.index('pmm_direct_map_activate('),
        'RAM preactivation/coverage ordering')
    E.require('pmm_alloc_constrained_frames(1, 1, PHYSICAL_ENTRY_LIMIT' in paging['paging_create_bootstrap_root'],
        'RAM low bootstrap root constraint absent')
    ap=T.functions((root/'kernel/src/smp/smp_boot.c').read_text())['smp_boot_aps']
    E.require('cpu->stack_top' in ap and 'stack_physical +' not in ap,'RAM virtual AP stack handoff')
    boot=T.functions((root/'kernel/kernel.c').read_text())['kernel_main_high']
    E.require(boot.index('physical_boot_adapt(')<boot.index('boot_info->'),'RAM early handoff adaptation')
    return {'scope':value['name'],'guard':guard,'full_span_integrity':'UNCHANGED_BASE_FUNCTION',
        'entry_assembly':'UNCHANGED_BASE_BYTES','mapping_design':'RETAINED_IDENTITY_VERIFIED_RAM_AND_OWNED_DEVICE_WINDOW',
        'translation_epoch':'BOOT_ONLY_VERIFIED_TRANSITION','ram_base':0xffff800000000000,'ram_span':0x400000000000}

def portable_changes(package):
    import device_current
    return device_current.portable_changes(package,base=BASE,tree_id=TREE)
