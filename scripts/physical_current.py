"""Checked address-boundary scope rooted in the approved invalidation source."""
import json
import os
import re
import subprocess
from pathlib import Path
import memory_evidence as E
import tlb_current as T

ROOT=Path(__file__).resolve().parent.parent
BASE='0f6629ce6ee9c9e04fdde236ddac36f5b5531d49'
TREE='a5c8b055ed5eacbb0205b984a672518c76548b75'

def contract(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.contract(root)
    value=json.loads((root/'scripts/physical-translation-scope.json').read_text())
    E.require((value['base_commit'],value['base_tree'])==(BASE,TREE),'physical scope baseline')
    for name,digest in value['historical_contracts'].items():
        E.require(E.sha(root/name)==digest,'historical scope modified: '+name)
    for name,digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name)==digest,'physical protected anchor drift: '+name)
    for name,expected in value['protected_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,digest in expected.items():
            E.require(symbol in actual and E.identity(actual[symbol])==digest,
                      'physical protected function drift: '+name+':'+symbol)
    # Promoted whole-source anchors bind only the explicitly reviewed boundary
    # edits. Original protected function hashes above are never derived from them.
    for name,row in value['promoted_sources'].items():
        E.require(E.sha(root/name)==row['candidate_sha256'],'physical candidate source drift: '+name)
    return value

def audit_sites(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.audit_sites(root)
    value=contract(root);actual=T.instruction_sites(root)
    E.require(actual==value['instruction_sites'],'unauthorized architectural instruction site')
    return {'sites':len(actual),'sha256':E.identity(actual)}

def scope(family):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.scope(family)
    value=contract()
    names=subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=ROOT).decode().split('\0')
    names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
    changed={n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed<=set(value['allowed_paths']),'unrelated physical source edits: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':value['name'],
        'base_commit':BASE,'changed_paths':sorted(changed),'scope_contract_sha256':E.sha(ROOT/'scripts/physical-translation-scope.json'),
        'protected':value['immutable_reviewed_anchors'],'superseded_scope':'Explicit address conversions and bounded allocation; existing native runtime invariants retained'}

def boundary_semantics(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.boundary_semantics(root)
    value=contract(root)
    code={p.relative_to(root).as_posix():p.read_text() for p in (root/'kernel').rglob('*') if p.suffix in ('.c','.h')}
    for name,text in code.items():
        stripped=re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)
        E.require(not re.search(r'\b(?:HHDM_OFFSET|VIRT_TO_PHYS_OFFSET|KERNEL_VIRT_OFFSET)\b',stripped),
                  'generic physical alias remains: '+name)
        if name not in ('kernel/src/memory/physical_layout.h','kernel/src/memory/physical_contract.c',
                        'kernel/src/memory/physical_translation.c'):
            E.require('KERNEL_IMAGE_OFFSET' not in stripped,'image offset escaped boundary: '+name)
    pmm=T.functions(code['kernel/src/memory/pmem.c'])
    for name in ('pmm_alloc_frame','pmm_alloc_contiguous_frames'):
        E.require('bitmap_find_zero_run_below' in pmm[name] and 'g_alloc_limit_frames' in pmm[name] and
                  'physical_allocation_ceiling()' in pmm[name],'allocation reachability enforcement absent')
    paging=code['kernel/src/memory/paging.c']
    E.require('paging_kernel_root_physical' in paging and 'physical_require_address' in paging,
              'physical hardware root boundary absent')
    entry=code['kernel/kernel.c'];body=T.functions(entry)['kernel_main_high']
    E.require(body.index('physical_boot_adapt(')<body.index('boot_info->'),'boot handoff dereferenced before adaptation')
    T.dispatch_semantics(code['kernel/src/core/interrupts.c']);T.caller_semantics(root)
    return {'scope':value['name'],'full_span_integrity':'UNCHANGED_BASE_FUNCTION',
            'entry_assembly':'UNCHANGED_BASE_BYTES','mapping_design':'IDENTITY_AND_LINKED_IMAGE',
            'ceiling_max':4294967296,'translation_epoch':'IMMUTABLE_FOR_CURRENT_BOOT'}
