"""Guarded-stack successor rooted at the approved image-protection delivery."""
import os
import json
import re
import subprocess
from pathlib import Path
import memory_evidence as E

BASE='5f6b66d3cefdf3becb2a6f5323aaf97b2178d62b'
TREE='dfa58366380cb754d420d8a8a4eda599a87fcf4c'
CONTRACT='guarded-kernel-stacks'
ROOT=E.ROOT

def enabled():
    return os.environ.get('HOBBYOS_VALIDATION_CONTRACT')==CONTRACT

def hosted_stack_sources():
    return ['kernel/src/memory/stack_domain.c','kernel/src/memory/stack_map.c'] if enabled() else []

def hosted_inverse_sources():
    return ['kernel/src/memory/stack_domain.c','kernel/src/memory/kernel_stack.c'] if enabled() else []

def functions(source):
    import tlb_current as T
    result=T.functions(source)
    masked=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
        lambda m: ''.join('\n' if c=='\n' else ' ' for c in m[0]),source,flags=re.S)
    for m in re.finditer(r'(?m)^__attribute__\(\(interrupt\)\) void (\w+)\([^;{}]*\)\s*\{',masked):
        depth=1;end=m.end()
        while depth and end<len(masked):
            depth+=(masked[end]=='{')-(masked[end]=='}');end+=1
        E.require(not depth,'stack scope unbalanced exception body')
        result[m[1]]=source[m.start():end]
    return result

def contract(root=ROOT):
    import irq_latency_current as observer
    if observer.enabled():return observer.contract(root)
    import tlb_current as T
    value=json.loads((root/'scripts/stack-guard-scope.json').read_text())
    E.require((value['base_commit'],value['base_tree'],value['name'])==(BASE,TREE,CONTRACT),
        'stack scope approved pre-edit identity')
    for name,digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name)==digest,'stack protected file drift: '+name)
    for name,rows in value['protected_functions'].items():
        actual=functions((root/name).read_text())
        for symbol,digest in rows.items():
            E.require(symbol in actual and E.identity(actual[symbol])==digest,'stack protected function drift: '+name+':'+symbol)
    for name,row in value['promoted_sources'].items():
        E.require(row['purpose'] and row['invariant'] and E.sha(root/name)==row['candidate_sha256'],
            'stack promoted source differs: '+name)
    for name,rows in value['promoted_functions'].items():
        actual=functions((root/name).read_text())
        for symbol,row in rows.items():
            E.require(row['purpose'] and row['invariant'] and
                (E.identity(actual[symbol]) if symbol in actual else None)==row['candidate_sha256'],
                'stack promoted function differs: '+name+':'+symbol)
    T.dispatch_semantics((root/'kernel/src/core/interrupts.c').read_text());T.caller_semantics(root)
    return value

def scope(family):
    import irq_latency_current as observer
    if observer.enabled():return observer.scope(family)
    value=contract()
    if ROOT.name=='source' and (ROOT.parent/'source-manifest.json').is_file():
        changed=portable_changes(ROOT.parent)
    else:
        names=subprocess.check_output(['git','diff','--name-only','-z',BASE],cwd=ROOT).decode().split('\0')
        names+=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
        changed={n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed<=set(value['allowed_paths']),'unrelated guarded-stack edit: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':CONTRACT,'base_commit':BASE,
        'changed_paths':sorted(changed),'scope_contract_sha256':E.sha(ROOT/'scripts/stack-guard-scope.json'),
        'protected':value['immutable_reviewed_anchors']}

def audit_sites(root=ROOT):
    import irq_latency_current as observer
    if observer.enabled():return observer.audit_sites(root)
    import tlb_current as T
    expected=contract(root)['instruction_sites'];actual=T.instruction_sites(root)
    E.require(actual==expected,'unauthorized architectural instruction site: guarded stacks')
    return {'sites':len(actual),'sha256':E.identity(actual)}

def source_guard(root=ROOT):
    import irq_latency_current as observer
    if observer.enabled():return observer.source_guard(root)
    import device_current
    return device_current.source_guard(root,inventory='stack-guard-conversion-sites.json',base=BASE)

def portable_changes(package):
    import device_current
    return device_current.portable_changes(package,base=BASE,tree_id=TREE)

def boundary_semantics(root=ROOT):
    import low_current
    value=contract(root);guard=source_guard(root)
    read=lambda name:(root/('kernel/src/'+name)).read_text()
    low_current.transition_semantics(read('memory/paging.c'),read('memory/paging_build.c'),
        read('smp/smp_boot.c'),read('core/entry.S'))
    scheduler=functions(read('core/scheduler.c'));create=scheduler['thread_create_ex_handle'];reap=scheduler['scheduler_reap_zombies']
    E.require('kmalloc_aligned(STACK_SIZE' not in create and 'kernel_stack_acquire(' in create and
        'kernel_stack_bind_task(' in create and 'kernel_stack_publish(' in create and
        'kernel_stack_discard(' in create,'stack scheduler acquisition/failure/publication adapter')
    E.require(reap.index('spin_unlock_irqrestore(')<reap.index('kernel_stack_retire(')<reap.index('kfree(t)'),
        'stack reaper storage order')
    task=read('core/task.h')
    E.require(re.search(r'#define\s+HOBBYOS_KERNEL_STACK_SIZE\s+\(16u \* 1024u\)',task) and
        re.search(r'#define\s+HOBBYOS_KERNEL_STACK_GUARD_BYTES\s+512',task),'stack task capacity/canary changed')
    allocator=functions(read('memory/kernel_stack.c'));release=allocator['release']
    E.require(release.index('paging_stack_remove(')<release.index('kernel_mapping_invalidate(')<
        release.index('free_backing(')<release.index('stack_owner_finish_retire(record(h),h,result.generation,true)'),
        'stack reuse precedes acknowledged invalidation')
    E.require('cpu_request_mapping_context_allowed()' in release and 'result.requested==result.acknowledged' in release and
        'STACK_RUNTIME' in release and 'quarantined' in release,'stack retirement context/completion/quarantine absent')
    fault=functions(read('core/interrupts.c'))
    E.require('guard_probe_fixup(' not in fault['exc_isr8'] and
        fault['exc_isr14'].index('protection_probe_fixup(')<fault['exc_isr14'].index('guard_probe_fixup(')<
        fault['exc_isr14'].index('kpanic_exception_ex('),'stack exact fault handler integration')
    E.require('tss_create_per_cpu' not in read('cpu/tss.c') and 'xchgq' in read('cpu/tss.c'),
        'stack unguarded alternate TSS path or non-atomic IST handoff')
    return {'scope':value['name'],'guard':guard,'lower_half':'ALL_256_ROOT_ENTRIES_ZERO',
        'image_permissions':'PRESERVED_ALL_PHYSICAL_ALIASES','nx_absent':'GUARDS_AND_RO_WRITES_WITHOUT_EXECUTE_PROTECTION',
        'region':[0xffffe00000000000,0xffffe00006800000],'task_capacity':16384,'canary_prefix':512,
        'ist_capacity':32768,'guard_bytes':4096,'retirement':'ALL_TARGET_ACK_BEFORE_BACKING_OR_VA_REUSE',
        'early_workers':'EIGHT_PREMAPPED_PREPARED_SLOTS','double_fault_rip':'ABORT_UNDEFINED',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE'}
