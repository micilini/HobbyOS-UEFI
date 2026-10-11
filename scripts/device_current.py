"""Owned-device successor contract rooted in the approved translation source.

The fixed baseline anchors and named promotions live in the versioned manifest.
Historical contracts continue to verify their own original source and captures.
"""
import importlib.util
import hashlib
import json
import re
import subprocess
import direct_map_evidence as RAM
from pathlib import Path
import memory_evidence as E
import tlb_current as T

ROOT=Path(__file__).resolve().parent.parent
BASE='ff6eaf609611b3d4a401e739f1f45dd7451346f3'
TREE='df9eba7d8c01ab2514474c3fd6e9f91eb3284906'

def contract(root=ROOT):
    if RAM.enabled():
        import direct_current
        return direct_current.contract(root)
    value=json.loads((root/'scripts/device-mapping-scope.json').read_text())
    E.require((value['base_commit'],value['base_tree'])==(BASE,TREE),'device scope baseline')
    for group in ('historical_contracts','immutable_reviewed_anchors'):
        for name,digest in value[group].items():
            E.require(E.sha(root/name)==digest,'device protected anchor drift: '+name)
    for name,expected in value['protected_functions'].items():
        actual=T.functions((root/name).read_text())
        for symbol,digest in expected.items():
            E.require(symbol in actual and E.identity(actual[symbol])==digest,
                      'device protected function drift: '+name+':'+symbol)
    for name,row in value['promoted_sources'].items():
        E.require(row['purpose'] and row['invariant'] and E.sha(root/name)==row['candidate_sha256'],
                  'device candidate anchor drift: '+name)
    T.dispatch_semantics((root/'kernel/src/core/interrupts.c').read_text())
    T.caller_semantics(root)
    return value

def portable_changes(package,base=BASE,tree_id=TREE):
    """Compare complete reconstructed trees against the pinned approved tree."""
    package=Path(package)
    spec=importlib.util.spec_from_file_location('device_scope_tree',ROOT/'scripts/package-memory-review.py')
    tree=importlib.util.module_from_spec(spec);spec.loader.exec_module(tree)
    def checked(directory, manifest):
        rows=manifest['files'];names=set();actual=[]
        for row in rows:
            name=row['path'];E.safe(name);path=directory/name
            E.require(name not in names and row['mode'] in ('100644','100755') and
                not any(p.is_symlink() for p in (path,*path.parents)) and path.is_file(),
                'portable scope path/mode')
            names.add(name);data=path.read_bytes()
            blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
            E.require(len(data)==row['size'] and E.sha(path)==row['sha256'] and
                blob==row['current_blob'] and path.stat().st_mode&0o777==int(row['mode'][3:],8),
                'portable scope source bytes/mode')
            actual.append({'path':name,'mode':row['mode'],'current_blob':blob})
        paths=list(directory.rglob('*'))
        E.require(not any(p.is_symlink() or not (p.is_file() or p.is_dir()) for p in paths) and
            names=={p.relative_to(directory).as_posix() for p in paths if p.is_file()},
            'portable scope source inventory')
        E.require(tree.source_tree(actual)==manifest['export_tree'],'portable scope source tree')
        return {r['path']:(r['mode'],r['current_blob']) for r in actual}
    baseline=json.loads((package/'preservation/source-manifest.json').read_text())
    E.require(baseline['schema']==2 and baseline['head']==base and
        baseline['head_tree']==baseline['export_tree']==tree_id,'portable scope approved baseline')
    before=checked(package/'baseline-source',baseline)
    current=json.loads((package/'source-manifest.json').read_text())
    E.require(current['schema']==2,'portable scope current schema')
    after=checked(package/'source',current)
    E.require(set(before)<=set(after),'portable scope removed approved source')
    return {name for name,row in after.items() if before.get(name)!=row}

def scope(family):
    if RAM.enabled():
        import direct_current
        return direct_current.scope(family)
    value=contract()
    if ROOT.name=='source' and (ROOT.parent/'source-manifest.json').is_file():
        changed=portable_changes(ROOT.parent)
    else:
        names=subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=ROOT).decode().split('\0')
        names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
        changed={n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed<=set(value['allowed_paths']),'unrelated device mapping edits: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':value['name'],
        'base_commit':BASE,'changed_paths':sorted(changed),
        'scope_contract_sha256':E.sha(ROOT/'scripts/device-mapping-scope.json'),
        'protected':value['immutable_reviewed_anchors'],
        'superseded_scope':'Owned mappings, discovery, USB lifecycle, exact probe and global request accounting; native runtime requirements retained'}

def audit_sites(root=ROOT):
    if RAM.enabled():
        import direct_current
        return direct_current.audit_sites(root)
    value=contract(root);actual=T.instruction_sites(root)
    E.require(actual==value['instruction_sites'],'unauthorized architectural instruction site')
    return {'sites':len(actual),'sha256':E.identity(actual)}

def lapic_header_contract(source):
    addition='bool lapic_discovered_extent(uint64_t source, uint64_t *physical, uint64_t *bytes);\n'
    E.require(source.count(addition)==1,'LAPIC discovery declaration missing/altered')
    T.lapic_header_contract(source.replace(addition,''))

def source_guard(root=ROOT,*,inventory=None,base=BASE):
    """All current C/header casts are anchored; private mapper names cannot leak.

    An added spelling/extern wrapper still changes the exact cast/call inventory
    or a protected/promoted source anchor. No driver directory is exempted.
    """
    if inventory is None and RAM.enabled():
        import direct_current
        return direct_current.source_guard(root)
    inventory=inventory or 'device-conversion-sites.json'
    spec=importlib.util.spec_from_file_location('device_source_scan',root/'scripts/memory-source-inventory.py')
    scan=importlib.util.module_from_spec(spec);spec.loader.exec_module(scan);scan.ROOT=root
    found=scan.scan();expected=json.loads((root/'scripts'/inventory).read_text())
    E.require(expected['base_commit']==base,'device conversion baseline')
    ordinal={};casts=[]
    for row in found['pointer_cast_candidates']:
        key=(row['path'],row['function'],row['anchor']);ordinal[key]=ordinal.get(key,0)+1
        casts.append({k:row[k] for k in ('path','function','anchor')}|{'occurrence':ordinal[key]})
    E.require(casts==[{k:r[k] for k in ('path','function','anchor','occurrence')} for r in expected['sites']],
              'unknown or altered device conversion site')
    E.require(all(r['domain'] in {'CPU_POINTER','NUMERIC_PHYSICAL','BOOTSTRAP_EXCEPTION',
        'TRANSLATION_EXCEPTION','LINKED_IMAGE_PLACEMENT','SYNTHETIC_ARGUMENT'} and r['reason']
        for r in expected['sites']),'unclassified device conversion site')
    E.require(not found['external_calls'],'raw mapping/query consumer outside memory')
    observer=scan.code_only((root/'kernel/src/shell/commands/cmd_memory_observation.c').read_text())
    E.require(not re.search(r'\b(?:acpi_find_table|acpi_get_dsdt|firmware_mapping_acquire|mmio_mapping_acquire)\s*\(',observer),
              'observer acquires mappings instead of retained read-only metadata')
    private=re.compile(r'\b(?:PageTable|PageEntry|paging_map|paging_get_physical_address|paging_query_range|'
        r'paging_owned_\w+|PAGE_(?:PRESENT|RW|USER|PWT|PCD|PAT|NX|ADDR_MASK))\b')
    for path in sorted((root/'kernel').rglob('*')):
        if path.suffix not in ('.c','.h'):continue
        name=path.relative_to(root).as_posix();text=scan.code_only(path.read_text())
        E.require(not re.search(r'\b(?:HHDM_OFFSET|VIRT_TO_PHYS_OFFSET|KERNEL_VIRT_OFFSET)\b',text),
                  'generic physical alias remains: '+name)
        if not name.startswith('kernel/src/memory/'):
            E.require(not private.search(text) and 'paging_private.h' not in path.read_text(),
                      'private mapper/PTE boundary bypass: '+name)
            if '/drivers/usb/' in name:
                E.require(not re.search(r'\b(?:physical_require_address|physical_address_range|pmm_alloc_\w+|pmm_free_frame)\s*\(',text),
                          'unowned USB device address conversion: '+name)
    return {'status':'PASS','cast_sites':len(casts),'external_raw_mappers':0,
            'inventory_sha256':E.sha(root/'scripts'/inventory)}

def boundary_semantics(root=ROOT):
    if RAM.enabled():
        import direct_current
        return direct_current.boundary_semantics(root)
    value=contract(root);source_guard(root)
    pmm=T.functions((root/'kernel/src/memory/pmem.c').read_text())
    for name in ('pmm_alloc_frame','pmm_alloc_contiguous_frames'):
        E.require('bitmap_find_zero_run_below' in pmm[name] and 'g_alloc_limit_frames' in pmm[name] and
                  'physical_allocation_ceiling()' in pmm[name],'RAM reachability enforcement absent')
    E.require('bitmap_find_aligned_run_below' in pmm['pmm_alloc_constrained_frames'] and
        'g_alloc_limit_frames' in pmm['pmm_alloc_constrained_frames'],'DMA allocation ceiling absent')
    boot=T.functions((root/'kernel/kernel.c').read_text())['kernel_main_high']
    E.require(boot.index('physical_boot_adapt(')<boot.index('boot_info->'),'boot adaptation ordering')
    return {'scope':value['name'],'full_span_integrity':'UNCHANGED_BASE_FUNCTION',
        'entry_assembly':'UNCHANGED_BASE_BYTES','mapping_design':'IDENTITY_RAM_IMAGE_AND_OWNED_DEVICE_WINDOW',
        'ceiling_max':4294967296,'translation_epoch':'IMMUTABLE_WITH_EXPLICIT_NONRAM_HOLES'}
