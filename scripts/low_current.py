"""Explicit no-low successor rooted at the accepted final RAM-map commit."""
import json
import os
import subprocess
from pathlib import Path
import memory_evidence as E
import tlb_current as T

ROOT=E.ROOT
BASE='8be743834324cceb41025b0fd617e7ba52757448'
TREE='04820799f871331c84d13d7e72c95cb34b8239d3'
CONTRACT='low-address-isolation'
def enabled():return os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in (CONTRACT,'kernel-image-protection','guarded-kernel-stacks')

def contract(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        import kernel_image_current
        return kernel_image_current.contract(root)
    value=json.loads((root/'scripts/low-address-scope.json').read_text())
    E.require((value['base_commit'],value['base_tree'],value['name'])==(BASE,TREE,CONTRACT),
        'low-domain scope approved baseline')
    for name,digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name)==digest,'low-domain protected file drift: '+name)
    for name,rows in value['protected_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,digest in rows.items():
            E.require(symbol in actual and E.identity(actual[symbol])==digest,
                'low-domain protected function drift: '+name+':'+symbol)
    for name,row in value['promoted_sources'].items():
        E.require(row['purpose'] and row['invariant'] and E.sha(root/name)==row['candidate_sha256'],
            'low-domain promoted source differs: '+name)
    for name,rows in value['promoted_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,row in rows.items():
            E.require(row['purpose'] and E.identity(actual.get(symbol))==row['candidate_sha256'],
                'low-domain promoted body differs: '+name+':'+symbol)
    T.dispatch_semantics((root/'kernel/src/core/interrupts.c').read_text());T.caller_semantics(root)
    return value

def scope(family):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        import kernel_image_current
        return kernel_image_current.scope(family)
    value=contract()
    if ROOT.name=='source' and (ROOT.parent/'source-manifest.json').is_file():
        import device_current
        changed=device_current.portable_changes(ROOT.parent,base=BASE,tree_id=TREE)
    else:
        names=subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=ROOT).decode().split('\0')
        names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
        changed={n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed<=set(value['allowed_paths']),'unrelated low-domain edits: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':CONTRACT,'base_commit':BASE,
        'changed_paths':sorted(changed),'scope_contract_sha256':E.sha(ROOT/'scripts/low-address-scope.json'),
        'protected':value['immutable_reviewed_anchors']}

def audit_sites(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        import kernel_image_current
        return kernel_image_current.audit_sites(root)
    expected=contract(root)['instruction_sites'];actual=T.instruction_sites(root)
    E.require(actual==expected,'unauthorized architectural instruction site: low-domain')
    return {'sites':len(actual),'sha256':E.identity(actual)}

def source_guard(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        import kernel_image_current
        return kernel_image_current.source_guard(root)
    import device_current
    return device_current.source_guard(root,inventory='low-address-conversion-sites.json',base=BASE)

def portable_changes(package):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        import kernel_image_current
        return kernel_image_current.portable_changes(package)
    import device_current
    return device_current.portable_changes(package,base=BASE,tree_id=TREE)

def boundary_semantics(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        import kernel_image_current
        return kernel_image_current.boundary_semantics(root)
    value=contract(root);guard=source_guard(root)
    transition_semantics((root/'kernel/src/memory/paging.c').read_text(),
        (root/'kernel/src/memory/paging_build.c').read_text(),
        (root/'kernel/src/smp/smp_boot.c').read_text(),(root/'kernel/src/core/entry.S').read_text())
    return {'scope':value['name'],'guard':guard,'lower_half':'ALL_256_ROOT_ENTRIES_ZERO',
        'bootstrap':'PRIVATE_SINGLE_LOW_PAGE_WITH_VERIFIED_DEPARTURE','reservation':'ADOPT_EXISTING_RESERVED_CONVENTIONAL_RAM',
        'image_permissions':'UNCHANGED','direct_map':'PRESERVED','hardware':'DEFERRED_TO_FINAL_CLOSURE'}


def transition_semantics(paging_source,builder_source,ap_source,entry_source):
    paging=T.functions(paging_source);init=paging['init_paging']
    E.require(init.index('paging_build_verify(')<init.index('paging_build_activate(')<
        init.index('pmm_direct_map_activate(')<init.index('physical_translation_retire_identity('),
        'low-domain validation/activation/epoch order')
    tail=init[init.index('paging_build_activate('):]
    E.require('table->cpu->entries' not in tail and 'paging_table_hash(' in tail,
        'post-CR3 readback uses construction CPU pointers or omits backing verification')
    builder=T.functions(builder_source)
    E.require('map_interval(b, 0, 0,' not in builder['paging_build_create'] and
        'level == 4 && i < 256 && raw' in builder['verify_table'],'steady root low branch or incomplete zero check')
    ap=T.functions(ap_source);entry=ap['ap_kernel_entry']
    order=['gdt_flush(','ltr %%ax','idt_load();','paging_bootstrap_handoff_ready(','paging_load_kernel_root();',
        'cpu_policy_confirm_local(','paging_bootstrap_depart(','smp_mark_cpu_online(']
    E.require(all(s in entry for s in order) and [entry.index(s) for s in order]==sorted(entry.index(s) for s in order),
        'AP main root precedes high descriptor readiness or local departure')
    start=ap['smp_boot_aps']
    E.require(start.index('pmm_claim_bootstrap(')<start.index('paging_bootstrap_create(') and
        'pmm_mark_frame_used(' not in start and 'smp_find_trampoline_page' not in start and
        'paging_bootstrap_retire(ptr_cr3)' in start and '*ptr_stack = cpu->stack_top' in start,
        'trampoline reservation/root/stack ownership')
    root=entry_source
    E.require('movabs rsp, offset bsp_stack_top' in root and root.index('mov cr3, rax')<root.index('movabs rsp, offset bsp_stack_top')<
        root.index('sub rsp, 8')<root.index('jmp rax') and '.skip 16384\nbsp_stack_top:' in root,
        'BSP high stack missing before first C frame')
    return {'status':'PASS','activation':'VERIFIED_BEFORE_SWITCH','AP':'DESCRIPTORS_BEFORE_MAIN_ROOT'}
