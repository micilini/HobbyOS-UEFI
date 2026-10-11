#!/usr/bin/env python3
"""Recreate targeted ownership contradictions from immutable current evidence."""
import argparse
import copy
import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import zlib
import memory_evidence as E
import device_mapping_evidence as D
import device_current as C
import direct_map_evidence as RAM

ROOT=Path(__file__).resolve().parent.parent
def module(name):
    spec=importlib.util.spec_from_file_location('owned_fixture_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value
V=module('verify-device-mapping');T=module('verify-tlb-shootdown');O=module('verify-memory-observation')

COMPILED=[
 ('root-duplicate-admission','usb-ownership','kernel/src/drivers/usb/xhci/usb_ownership.c',
  'o->phase != USB_ROOT_DETACHED','false','!usb_root_claim'),
 ('child-overwrites-root','hub-ownership','kernel/src/drivers/usb/xhci/usb_hub.c',
  'if (ok) hub->child_slot[ctx->port_on_parent] = slot;','if (ok) hub->slot_id = slot;','parent->slot_id==1'),
 ('duplicate-poll-callback','usb-hotplug','kernel/src/drivers/usb/xhci/usb_hotplug.c',
  'g_poll_ticket == (uintptr_t)ctx','ctx != NULL','scheduled==before'),
 ('wrong-trb-completes-lane','usb-ownership','kernel/src/drivers/usb/xhci/usb_ownership.c',
  'if (trb == o->trb[i])','if (i + 1 == o->count)','!usb_ep0_event'),
 ('completion-before-event-retirement','usb-ownership','kernel/src/drivers/usb/xhci/usb_ownership.c',
  'cc == 1 ? USB_EP0_OBSERVED','cc == 1 ? USB_EP0_COMPLETE','!usb_ep0_retire'),
 ('timeout-lane-reused','usb-ownership','kernel/src/drivers/usb/xhci/usb_ownership.c',
  'o->phase = USB_EP0_QUARANTINED;','o->phase = USB_EP0_IDLE;','lane.phase==USB_EP0_QUARANTINED'),
 ('full-speed-fictional-translator','usb-ownership','kernel/src/drivers/usb/xhci/usb_ownership.c',
  'parent_speed == 3','parent_speed != 4','slot==(tt?2:0)'),
 ('undiscovered-mmio-accepted','origin','kernel/src/memory/owned_mapping.c',
  'if (!mmio_origin_valid(origin)) return MAPPING_INVALID;','(void)origin;','mmio_mapping_acquire(&bad,CACHE_WRITE_COMBINING,&out)==MAPPING_INVALID'),
 ('stale-origin-accepted','origin','kernel/src/memory/device_origin.c',
  'discovered.generation != origin->generation','false','!mmio_origin_valid(&bad)'),
 ('cache-maintenance-omitted','origin','kernel/src/memory/owned_mapping.c',
  '(origin && !writeback())','(origin && false && !writeback())','sequence_count==5'),
 ('dma-zeroing-omitted','dma','kernel/src/memory/dma_buffer.c',
  'if (ok) memset(cpu, 0, buffer.backing_bytes);','if (false) memset(cpu, 0, buffer.backing_bytes);','((uint8_t *)buffer.cpu)[i]==0'),
 ('ac64-limit-ignored','dma','kernel/src/drivers/usb/xhci/xhci_dma.c',
  '(hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX','(hccparams1 & 1u) ? UINT64_MAX : UINT64_MAX','xhci_dma_capability_limit(0)=='),
 ('probe-wrong-address-recovered','probe','kernel/src/memory/mapping_probe_match.c',
  's->address == address','(address || true)','!mapping_probe_match(&s,s.site,s.address+1'),
 ('probe-protection-fault-recovered','probe','kernel/src/memory/mapping_probe_match.c',
  'error == 0','(error || true)','!mapping_probe_match(&s,s.site,s.address,error'),
]

SEMANTIC={
 'framebuffer-wb-alias':'incompatible device identity alias remains',
 'framebuffer-wc-downgraded':'framebuffer selected cache policy',
 'register-wb-accepted':'device registers have cacheable selection',
 'firmware-writable':'firmware must be WB read-only',
 'partial-map-success':'mapping complete page envelope missing',
 'wrong-physical-leaf':'actual managed leaf differs from ownership',
 'dma-complete-extent-limit':'DMA extent/domain/lifetime/constraint',
 'dma-high-word-truncated':'DMA extent/domain/lifetime/constraint',
 'dma-misaligned':'DMA extent/domain/lifetime/constraint',
 'dma-not-zeroed':'owned unmap/probe/zeroed constrained allocation',
 'unmap-fault-not-observed':'owned unmap/probe/zeroed constrained allocation',
 'unmap-frame-reused':'mapping repeated-cycle retention/accounting',
 'unmap-buffer-still-live':'mapping repeated-cycle retention/accounting',
 'unmap-view-still-live':'mapping repeated-cycle retention/accounting',
 'unmap-handle-reused':'fixture stale/duplicate resource generation',
 'actual-ac64-contradicted':'actual xHCI capability/readiness',
 'duplicate-view':'owned view extent/state',
 'missing-framebuffer-pages':'mapping leaf coverage gap/duplicate',
}
if RAM.LOW.enabled():
    SEMANTIC['framebuffer-wb-alias']='retired identity alias unexpectedly restored'
AUDIT={
 'arbitrary-start-generation':'boot audit missing/duplicate/stale generation',
 'missing-maintenance-prelude':'boot audit missing/duplicate/stale generation',
 'missing-middle-service':'boot audit missing/duplicate/stale generation',
 'duplicate-service':'boot audit missing/duplicate/stale generation',
 'missing-terminal-service':'boot audit missing/duplicate/stale generation',
 'wrong-service-root':'service root/target/completion failure',
 'wrong-service-target':'service root/target/completion failure',
 'wrong-service-identity':'service target identity',
 'hidden-service-failure':'service root/target/completion failure',
 'sample-used-as-maintenance':'unexplained service generation',
 'checkpoint-from-first-record':'audit checkpoint binding',
 'workload-service-contradiction':'fixture/service records disagree',
}
EXTRA={'production-service-omitted','production-probe-present','production-reference-present',
       'raw-mapper-bypass','raw-pte-bypass','unowned-usb-dma','observer-lazy-map',
       'reference-relabel','direct-keyboard-relabeled','hub-child-overwrites-root','hub-wrong-translator',
       'command-stdout-offset','command-stdout-hash','duplicate-command-stdout','mapping-after-final-audit'}
REQUIRED=set(SEMANTIC)|set(AUDIT)|EXTRA|{r[0] for r in COMPILED}

def verify_receipt(result):
    names=[r['name'] for r in result['cases']]
    E.require(result['status']=='PASS' and len(names)==len(set(names)) and set(names)==REQUIRED,
              'device negative inventory incomplete/duplicate')
    E.require(result['positive_controls']==['current-mapping-raw','complete-service-audit','actual-USB-C',
        'actual-MMIO-admission-C','production-symbols','sole-hub-keyboard','bounded-transport',
        'concurrent-global-free-change','final-maintenance-before-audit'],
        'device negative positive controls incomplete')
    for row in result['cases']:
        E.require(row['synthetic'] and row['actual_exit']!=0 and row['expected_boundary'] in row['actual_reason'],
                  'device fixture rejected at unrelated boundary: '+row['name'])
    module('review-transport-fixtures').verify(result['transport'])

def run(evidence,output,bases=None):
    output.mkdir(parents=True,exist_ok=False);cases=[]
    def reject(name,boundary,action,recipe):
        try:action()
        except (ValueError,KeyError,OSError,AssertionError) as error:
            actual=str(error);E.require(boundary in actual,'wrong device fixture boundary: '+name+': '+actual)
        else:raise ValueError('device contradiction accepted: '+name)
        cases.append({'name':name,'synthetic':True,'expected_boundary':boundary,'actual_reason':actual,
                      'actual_exit':1,'recipe':recipe})
    if bases is None:
        series=json.loads((evidence/'series.json').read_text())
        bases={'current':next(n for n in series['runs'] if n.startswith('profiles/q35-kvm-smp4-')),
            'reference':'framebuffer-reference/'+json.loads((evidence/'framebuffer-reference/series.json').read_text())['runs'][0],
            'hub':'hub-keyboard/'+json.loads((evidence/'hub-keyboard/series.json').read_text())['runs'][0]}
    E.require(set(bases)=={'current','reference','hub'},'device fixture base roles')
    for name in bases.values():E.safe(name)
    base=evidence/bases['current']
    V.verify_run(base)
    V.complete_command_order(V.COMMANDS)
    reject('mapping-after-final-audit','final mapping maintenance follows the last service audit',
        lambda:V.complete_command_order(V.WORKLOAD_CAPTURE_COMMANDS),
        {'operation':'Place the second mapping workload after the final audited workload',
         'commands':V.WORKLOAD_CAPTURE_COMMANDS})
    serial=(base/'qemu-serial.log').read_text().splitlines();topology=V.topology(serial)
    transactions=[json.loads(l) for l in (base/'framed.jsonl').read_text().splitlines()]
    root=module('verify-cpu-control-state').verify_run(base)['root']
    ram=RAM.activation(serial) if RAM.enabled() else None
    raw=D.split_traces((base/'qemu-debugcon.log').read_bytes())['MAPPING'][0]
    segment=O.FOUNDATION.transaction_segment(serial,next(r for r in transactions if r['payload']=='mappingtest'))
    D.verify_mapping(raw,segment,root,topology,ram=ram)
    concurrent=list(segment)
    at=next(i for i,l in enumerate(concurrent) if l.startswith('[MAPPING][END] '))
    counts=D.fields(concurrent[at],'[MAPPING][END] ')
    counts['free_after']=str(D.number(counts,'free_before')+4096)
    concurrent[at]='[MAPPING][END] '+' '.join(k+'='+v for k,v in counts.items())
    D.verify_mapping(raw,concurrent,root,topology,ram=ram)
    decoded=[]
    for line in raw.decode().splitlines():
        body=line.rsplit(' crc=',1)[0];prefix=body[:body.index(' ')+1]
        decoded.append((prefix,D.fields(body,prefix)))
    fbslot=next(r['slot'] for p,r in decoded if p=='[MAPPING][VIEW] ' and r['origin']=='1')
    def synthetic(name):
        rows=copy.deepcopy(decoded);lines=list(segment)
        def edit(prefix,predicate,change):
            row=next(r for p,r in rows if p==prefix and predicate(r));change(row)
        if name=='framebuffer-wb-alias':
            def alias(r):
                pairs=r['entries'].split(',');leaf=int(pairs[0].split(':')[0],16)
                pairs[0]=f'{leaf:x}:{(leaf&D.MASK)|3:x}';r['entries']=','.join(pairs)
            edit('[MAPPING][LEAVES] ',lambda r:r['slot']==fbslot,alias)
        elif name=='framebuffer-wc-downgraded':edit('[MAPPING][VIEW] ',lambda r:r['slot']==fbslot,
            lambda r:r.update(flags=hex((D.number(r,'flags')&~0x98)|0x80)))
        elif name=='register-wb-accepted':edit('[MAPPING][VIEW] ',lambda r:int(r['origin'])>1,
            lambda r:r.update(flags=hex(D.number(r,'flags')&~0x98)))
        elif name=='firmware-writable':edit('[MAPPING][VIEW] ',lambda r:r['origin']=='0',
            lambda r:r.update(flags=hex(D.number(r,'flags')|2)))
        elif name in ('partial-map-success','missing-framebuffer-pages'):
            at=[i for i,(p,r) in enumerate(rows) if p=='[MAPPING][LEAVES] ' and r['slot']==fbslot]
            del rows[at[-1 if name=='partial-map-success' else 0]]
        elif name=='wrong-physical-leaf':
            def corrupt(r):
                pairs=r['entries'].split(',');leaf,old=pairs[0].split(':');pairs[0]=f'{int(leaf,16)^4096:x}:{old}';r['entries']=','.join(pairs)
            edit('[MAPPING][LEAVES] ',lambda r:True,corrupt)
        elif name in ('dma-complete-extent-limit','dma-high-word-truncated','dma-misaligned'):
            def dma(r):
                if name=='dma-complete-extent-limit':r['maximum']=hex(D.number(r,'device')+D.number(r,'backing')-2)
                elif name=='dma-high-word-truncated':r['device']=hex(D.number(r,'device')+(1<<32))
                else:r['alignment']=str(3)
            edit('[MAPPING][DMA] ',lambda r:True,dma)
        elif name=='actual-ac64-contradicted':edit('[MAPPING][USB] ',lambda r:True,lambda r:r.update(maximum='0'))
        elif name=='duplicate-view':rows.insert(1,copy.deepcopy(next(v for v in rows if v[0]=='[MAPPING][VIEW] ')))
        elif name=='unmap-handle-reused':
            positions=[i for i,l in enumerate(lines) if l.startswith('[MAPPING][UNMAP] ')]
            first=D.fields(lines[positions[0]],'[MAPPING][UNMAP] ')
            second=D.fields(lines[positions[1]],'[MAPPING][UNMAP] ')
            second.update(dma_slot=first['dma_slot'],dma_generation=first['dma_generation'])
            lines[positions[1]]='[MAPPING][UNMAP] '+' '.join(k+'='+v for k,v in second.items())
        else:
            prefix='[MAPPING][END] ' if name in ('unmap-frame-reused','unmap-buffer-still-live','unmap-view-still-live') else '[MAPPING][UNMAP] '
            at=next(i for i,l in enumerate(lines) if l.startswith(prefix));r=D.fields(lines[at],prefix)
            key={'dma-not-zeroed':'zeroed','unmap-fault-not-observed':'recovered','unmap-frame-reused':'released_bytes',
                 'unmap-buffer-still-live':'live_buffers','unmap-view-still-live':'live_views'}[name]
            r[key]='1' if name in ('unmap-buffer-still-live','unmap-view-still-live') else '0'
            lines[at]=prefix+' '.join(k+'='+v for k,v in r.items())
        raw=b''
        for prefix,row in rows:
            body=(prefix+' '.join(k+'='+v for k,v in row.items())).encode()
            raw+=body+f' crc={zlib.crc32(body):08x}\n'.encode()
        digest=14695981039346656037
        for b in raw:digest=((digest^b)*1099511628211)&((1<<64)-1)
        at=next(i for i,l in enumerate(lines) if l.startswith('[MAPPING_TRACE][COMPLETE] '))
        r=D.fields(lines[at],'[MAPPING_TRACE][COMPLETE] ');r.update(bytes=str(len(raw)),records=str(raw.count(b'\n')),fnv64=hex(digest))
        lines[at]='[MAPPING_TRACE][COMPLETE] '+' '.join(k+'='+v for k,v in r.items())
        return raw,lines
    for name,boundary in SEMANTIC.items():
        changed,lines=synthetic(name)
        reject(name,boundary,lambda b=changed,l=lines:D.verify_mapping(b,l,root,topology,ram=ram),
            {'base':base.relative_to(evidence).as_posix(),'trace_sha256':E.sha(base/'qemu-debugcon.log'),
             'operation':name,'result_sha256':E.identity([changed.hex(),lines]),'outer_metadata':'synthetic CRC/FNV recomputed'})
    tlb=T.expand_trace(T.trace_blocks(base)[0].decode().splitlines());T.verify_tlb(T.trace_blocks(base)[0].decode().splitlines(),topology,root,True)
    fixture_generations={D.number(D.fields(line,'[TLB][REQUEST] '),'generation') for line in tlb
                         if line.startswith('[TLB][REQUEST] ')}
    for name,boundary in AUDIT.items():
        changed=list(tlb);positions=[i for i,l in enumerate(changed) if l.startswith('[TLB][SERVICE] ')]
        if name in ('missing-maintenance-prelude','missing-middle-service','missing-terminal-service'):
            del changed[positions[{'missing-maintenance-prelude':0,'missing-middle-service':len(positions)//2,'missing-terminal-service':-1}[name]]]
        elif name=='duplicate-service':changed.insert(positions[0],changed[positions[0]])
        else:
            at=positions[0];prefix='[TLB][SERVICE] '
            if name=='checkpoint-from-first-record':at=next(i for i,l in enumerate(changed) if l.startswith('[TLB][AUDIT] '));prefix='[TLB][AUDIT] '
            if name=='workload-service-contradiction':
                at=next(i for i in positions if D.number(D.fields(changed[i],prefix),'generation') in fixture_generations)
            r=D.fields(changed[at],prefix)
            if name=='arbitrary-start-generation':r['generation']='1000'
            elif name=='wrong-service-root':r['root']='0x2000'
            elif name=='wrong-service-target':r['targets']='0x7'
            elif name=='wrong-service-identity':r['identities']='0:0,1:2,2:1,3:3'
            elif name=='hidden-service-failure':r['status']='7'
            elif name=='sample-used-as-maintenance':r['operation']='2'
            elif name=='checkpoint-from-first-record':r['emitted_before']=r['before']
            else:r['elapsed_ns']=str(D.number(r,'elapsed_ns')+1)
            changed[at]=prefix+' '.join(k+'='+v for k,v in r.items())
        sealed=[l+' crc='+f'{zlib.crc32(l.encode()):08x}' for l in changed]
        reject(name,'TLB '+boundary,lambda s=sealed:T.verify_tlb(s,topology,root,True),
            {'base':base.relative_to(evidence).as_posix(),'operation':name,'result_sha256':E.identity(sealed),
             'outer_metadata':'synthetic record checksums recomputed; checkpoint never inferred'})
    symbols=(evidence/'candidates/production/symbols.txt').read_text();strings=(evidence/'candidates/production/compiled-strings.txt').read_text()
    V.production(symbols,strings)
    for name,change,boundary in [
        ('production-service-omitted',lambda s:re.sub(r'^.*\bmmio_mapping_acquire$','',s,flags=re.M),'production device service missing'),
        ('production-probe-present',lambda s:s+'\n00000001 T mapping_probe_fault\n','production mapping test payload'),
        ('production-reference-present',lambda s:s+'\n00000001 T mmio_framebuffer_reference\n','production mapping test payload')]:
        reject(name,boundary,lambda c=change:V.production(c(symbols),strings),{'operation':name,'base':E.sha(evidence/'candidates/production/symbols.txt')})
    with tempfile.TemporaryDirectory(prefix='owned-source-negative-') as temp:
        scratch=Path(temp)
        for prefix in ('kernel','bootloader','shared'):shutil.copytree(ROOT/prefix,scratch/prefix,ignore=shutil.ignore_patterns('*.o','*.d','*.su'))
        (scratch/'scripts').mkdir()
        inventories=['memory-source-inventory.py','device-conversion-sites.json','direct-map-conversion-sites.json']
        if RAM.LOW.enabled():inventories.append('low-address-conversion-sites.json')
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='kernel-image-protection':
            inventories.append('kernel-image-conversion-sites.json')
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            inventories.append('stack-guard-conversion-sites.json')
        for name in inventories:shutil.copyfile(ROOT/'scripts'/name,scratch/'scripts'/name)
        C.source_guard(scratch)
        path=scratch/'kernel/src/drivers/usb/xhci/xhci.c';original=path.read_text()
        for name,code,boundary in [
            ('raw-mapper-bypass','extern void paging_map(unsigned long, unsigned long, unsigned long);','private mapper/PTE boundary bypass'),
            ('raw-pte-bypass','void invalid_store(PageTable *p) { p->entries[0] = 3; }','private mapper/PTE boundary bypass'),
            ('unowned-usb-dma','void invalid_dma(void *p) { physical_require_address(p, 1); }','unowned USB device address conversion')]:
            path.write_text(original+'\n'+code+'\n');reject(name,boundary,lambda:C.source_guard(scratch),{'path':path.relative_to(scratch).as_posix(),'append':code});path.write_text(original)
        path=scratch/'kernel/src/shell/commands/cmd_memory_observation.c';original=path.read_text()
        path.write_text(original+'\nvoid invalid_observer(void) { acpi_get_dsdt(); }\n')
        reject('observer-lazy-map','observer acquires mappings',lambda:C.source_guard(scratch),{'operation':'append lazy ACPI acquisition'})
    reference=evidence/bases['reference'];V.verify_run(reference,True)
    reject('reference-relabel','framebuffer reference role mislabeled',lambda:V.verify_run(reference),{'operation':'present real WB reference to WC acceptance'})
    hubdir=evidence/bases['hub'];positive=V.verify_run(hubdir)
    # This copy contains data only; a caller may place it in bounded external
    # replay scratch. Compiled hosted executables keep their normal location.
    with tempfile.TemporaryDirectory(prefix='owned-hub-label-',dir=os.environ.get('HOBBYOS_REVIEW_SCRATCH')) as temp:
        dest=Path(temp)/'capture';shutil.copytree(base,dest)
        original_commands=(dest/'commands.jsonl').read_bytes()
        for name in ('command-stdout-offset','command-stdout-hash','duplicate-command-stdout'):
            commands=[json.loads(l) for l in original_commands.splitlines()]
            command=next(r for r in commands if r['name']=='command-1')
            E.require('stdout_evidence' in command,'fixture requires a real canonical stdout positive')
            if name=='command-stdout-offset':command['stdout_evidence']['offset']+=1
            elif name=='command-stdout-hash':command['stdout_evidence']['sha256']='0'*64
            else:commands.append(copy.deepcopy(command))
            (dest/'commands.jsonl').write_bytes(b''.join(E.canonical(r) for r in commands))
            boundary='command stdout coverage' if name=='duplicate-command-stdout' else 'canonical command stdout reference'
            reject(name,boundary,lambda:V.verify_run(dest),{'operation':name,'base':base.relative_to(evidence).as_posix()})
        (dest/'commands.jsonl').write_bytes(original_commands)
        value=json.loads((dest/'result.json').read_text());value['device_profile']='hub-keyboard';(dest/'result.json').write_bytes(E.canonical(value))
        reject('direct-keyboard-relabeled','hub',lambda:V.verify_run(dest),{'operation':'direct topology presented as sole hub keyboard','base':base.relative_to(evidence).as_posix()})
    hserial=(hubdir/'qemu-serial.log').read_text().splitlines();hrows=[json.loads(l) for l in (hubdir/'framed.jsonl').read_text().splitlines()]
    hub_root=module('verify-cpu-control-state').verify_run(hubdir)['root']
    hsegment=O.FOUNDATION.transaction_segment(hserial,next(r for r in hrows if r['payload']=='mappingtest'))
    hraw=D.split_traces((hubdir/'qemu-debugcon.log').read_bytes())['MAPPING'][0]
    for name,key,value,boundary in [('hub-child-overwrites-root','slot','1','child topology/ownership'),
            ('hub-wrong-translator','tt_slot','1','translator ancestry mismatch')]:
        original=raw;original_segment=segment;original_decoded=decoded
        rows=[]
        for line in hraw.decode().splitlines():
            body=line.rsplit(' crc=',1)[0];prefix=body[:body.index(' ')+1];r=D.fields(body,prefix)
            if prefix=='[MAPPING][CHILD] ':r[key]=value
            rows.append((prefix,r))
        changed=b''.join((b:=p+' '.join(k+'='+v for k,v in r.items())).encode()+f' crc={zlib.crc32(b.encode()):08x}\n'.encode() for p,r in rows)
        hs=list(hsegment);digest=14695981039346656037
        for b in changed:digest=((digest^b)*1099511628211)&((1<<64)-1)
        at=next(i for i,l in enumerate(hs) if l.startswith('[MAPPING_TRACE][COMPLETE] '));r=D.fields(hs[at],'[MAPPING_TRACE][COMPLETE] ')
        r.update(bytes=str(len(changed)),records=str(changed.count(b'\n')),fnv64=hex(digest));hs[at]='[MAPPING_TRACE][COMPLETE] '+' '.join(k+'='+v for k,v in r.items())
        reject(name,boundary,lambda b=changed,l=hs:D.verify_mapping(b,l,hub_root,V.topology(hserial),ram=RAM.activation(hserial) if RAM.enabled() else None),{'operation':name,'base':hubdir.relative_to(evidence).as_posix(),'result_sha256':E.identity(changed.hex())})
    compiled(output,cases)
    transport=module('review-transport-fixtures').run(output/'transport')
    result={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),'harness_sha256':E.identity(E.manifest(harness=True)),
        'positive_controls':['current-mapping-raw','complete-service-audit','actual-USB-C','actual-MMIO-admission-C',
                            'production-symbols','sole-hub-keyboard','bounded-transport','concurrent-global-free-change',
                            'final-maintenance-before-audit'],
        'cases':cases,'transport':transport}
    verify_receipt(result);E.write(output/'results.json',result);return result

def compiled(output,cases):
    source_sets={
        'usb-ownership':['scripts/usb-ownership-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c'],
        'usb-hotplug':['scripts/usb-hotplug-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c'],
        'hub-ownership':['scripts/usb-hub-ownership-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c'],
        'origin':['scripts/device-origin-host.c','kernel/src/memory/physical_contract.c','kernel/src/memory/mapping_core.c',
                  'kernel/src/utils/bitmap.c','kernel/src/cpu/control_policy.c','kernel/src/memory/cache_attributes.c'],
        'dma':['scripts/device-dma-host.c','kernel/src/utils/bitmap.c','kernel/src/memory/mapping_core.c',
               'kernel/src/memory/dma_buffer.c','kernel/src/drivers/usb/xhci/xhci_dma.c'],
        'probe':['scripts/mapping-probe-host.c','kernel/src/memory/mapping_probe_match.c']}
    for name,subject,file,old,new,boundary in COMPILED:
        dest=output/name;dest.mkdir()
        with tempfile.TemporaryDirectory(prefix='owned-compiled-negative-') as temp:
            scratch=Path(temp)
            for prefix in ('kernel','shared'):shutil.copytree(ROOT/prefix,scratch/prefix,ignore=shutil.ignore_patterns('*.o','*.d','*.su'))
            (scratch/'scripts').mkdir()
            for path in source_sets[subject]:
                if path.startswith('scripts/'):shutil.copyfile(ROOT/path,scratch/path)
            changed=scratch/file;original=changed.read_text();E.require(old in original,'owned compiled mutation anchor missing: '+name)
            changed.write_text(original.replace(old,new,1))
            argv=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-builtin','-fno-pie','-no-pie',
                  '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=0',
                  '-DHOBBYOS_CONTROL_POLICY_HOST_TEST',*source_sets[subject],'-o','fixture']
            build=subprocess.run(argv,cwd=scratch,capture_output=True,text=True)
            (dest/'compile.log').write_text(build.stdout+build.stderr);E.require(build.returncode==0,'owned mutation failed to compile: '+name)
            result=subprocess.run(['./fixture'],cwd=scratch,capture_output=True,text=True,
                preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)),timeout=20)
            (dest/'run.log').write_text(result.stdout+result.stderr)
            E.require(result.returncode!=0 and boundary in result.stderr,'owned mutation wrong C boundary: '+name+': '+result.stderr)
            cases.append({'name':name,'synthetic':True,'actual_exit':result.returncode,'expected_boundary':boundary,
                'actual_reason':result.stderr.strip(),'compile_argv':argv,'compile_exit':build.returncode,
                'recipe':{'path':file,'old':old,'new':new,'count':1,'base_sha256':E.sha(ROOT/file),'result_sha256':E.sha(changed)}})

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--bases',type=Path,help='Explicit validated current/hub/reference captures for fixture development; never a series receipt.')
    a=p.parse_args();print(json.dumps(run(a.evidence,a.output,json.loads(a.bases.read_text()) if a.bases else None),sort_keys=True))
