#!/usr/bin/env python3
"""Contradictions in exact local HobbyOS stack captures, with restored positives.

Only labeled temporary copies receive synthetic CRC/hash rebinding. These are
host verifier tests; the original guest faults and hosted C suites stay distinct.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import zlib
import memory_evidence as E
import review_transport as R
import stack_guard_evidence as S
import kernel_image_evidence as W

def module(name):
    spec=importlib.util.spec_from_file_location('stack_fixture_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

CASES={
    'guard-present-rw':'stack independent guard page is present',
    'guard-present-ro':'stack independent guard page is present',
    'bsp-guard-present':'stack independent guard page is present',
    'guard-covered-by-large-leaf':'stack independent guard page is present',
    'payload-missing':'stack independent payload/backing/effective rights',
    'payload-readonly':'stack independent payload/backing/effective rights',
    'payload-ancestor-readonly':'stack independent payload/backing/effective rights',
    'payload-executable':'stack independent payload/backing/effective rights',
    'payload-wrong-physical':'stack independent payload/backing/effective rights',
    'unowned-slot-mapping':'stack mapped guard/padding, missing owner or incorrect backing',
    'bsp-linker-hole-widened':'stack exact BSP linker guard/payload boundary',
    'owner-missing-ist':'stack missing BSP/AP/IST owner',
    'owner-wrong-cpu':'stack CPU/APIC owner mismatch',
    'owner-task-capacity-reduced':'stack task reserve/capacity/ownership',
    'owner-immutable-image-backing':'stack owned writable view aliases image backing',
    'owner-static-backing-owned':'stack owned writable view aliases image backing',
    'owner-writer-interrupted':'stack independent inconsistent or noncanonical owner scalar',
    'count-writer-interrupted':'stack independent readiness, interrupted writer or quarantine',
    'ap-tss-unguarded':'stack independent actual TSS points to an unowned/unguarded IST',
    'actual-rsp-unguarded':'stack actual CPU RSP is outside a live guarded execution payload',
    'actual-tr-wrong':'stack actual TR/GDTR/IDTR installation differs from owner backing',
    'one-cpu-wp-clear':'stack independent local controls/root',
    'raw-cpu-relabeled':'stack raw QMP/owner CPU identity differs',
    'mixed-owner-candidate':'stack independent capture source/candidate/VM/layout binding',
    'missing-owner-bytes':'stack independent raw scalar object',
    'guard-wrong-rip':'stack exact hardware guard fault mismatch',
    'guard-wrong-root':'stack exact hardware guard fault mismatch',
    'guard-wrong-cpu':'stack exact hardware guard fault mismatch',
    'guard-wrong-apic':'stack exact hardware guard fault mismatch',
    'guard-wrong-address':'stack exact hardware guard fault mismatch',
    'guard-stale-owner':'stack exact hardware guard fault mismatch',
    'guard-protection-error':'stack exact hardware guard fault mismatch',
    'guard-write-error':'stack exact hardware guard fault mismatch',
    'guard-user-error':'stack exact hardware guard fault mismatch',
    'guard-reserved-error':'stack exact hardware guard fault mismatch',
    'guard-fetch-error':'stack exact hardware guard fault mismatch',
    'guard-not-observed':'stack exact hardware guard fault mismatch',
    'guard-stale-generation':'stack guard probe duplicate/missing generation',
    'guard-canary-changed':'stack task canary prefix changed',
    'guard-owner-missing':'stack fault owner not inventoried',
    'churn-fake-migration':'stack actual lifecycle/migration coverage',
    'churn-no-reaper-overlap':'stack creators/reaper did not overlap',
    'churn-resource-leak':'stack net resource growth',
    'retirement-one-ack-missing':'stack service binding missing, duplicate or beyond terminal audit',
    'retirement-queued-as-complete':'stack service binding missing, duplicate or beyond terminal audit',
    'retirement-tail-missing':'stack TLB worker retirement coverage',
    'retirement-audit-missing':'stack service binding missing, duplicate or beyond terminal audit',
    'retirement-wrong-owner':'stack stale lifecycle retired a replacement owner',
    'overflow-stale-cr2':'stack destructive hardware DF/guard/frame mismatch',
    'overflow-vector14':'stack destructive hardware DF/guard/frame mismatch',
    'overflow-false-task':'stack destructive owner identity mismatch',
    'overflow-no-ist':'stack overflow did not use a separate healthy IST1',
    'overflow-exact-rip-claim':'stack destructive hardware DF/guard/frame mismatch',
    'overflow-canary-substitute':'stack destructive hardware DF/guard/frame mismatch',
    'timeout-frees-backing':'stack timeout was not quarantined across late completion',
    'timeout-queued-as-complete':'stack timeout was not quarantined across late completion',
    'timeout-reuses-va':'stack timeout was not quarantined across late completion',
    'production-guard-command':'production command is present or did not return unknown',
    'production-overflow-command':'production command is present or did not return unknown',
    'probe-load-opcode':'stack assembler opcode/ABI',
    'overflow-step-opcode':'stack assembler opcode/ABI',
    'probe-recovery-offset':'stack exact load/recovery/overflow instruction boundaries',
}
POSITIVES=['physical-main-root-and-owners','whole-table-stack-domain','linker-hole',
    'functional-guard-and-churn','absolute-retirement-service','destructive-df','timeout-quarantine',
    'production-command-absence','fixed-assembly']

PEAK_PROVENANCE_CASES={
    'missing-peak':'stack runtime peak coverage',
    'swapped-peaks':'control serial path transport binding differs',
    'unindexed-origin':'unlisted original evidence path',
    'wrong-transport-target':'control serial path transport binding differs',
    'wrong-observed-log':'control serial path transport binding differs',
    'changed-vm':'completed mode raw replay differs',
    'changed-boot':'completed mode raw replay differs',
    'changed-candidate':'completed mode raw replay differs',
    'changed-guard-fact':'stack stage semantic result differs: runtime',
    'missing-run-facts':'stack stage semantic result differs: runtime',
    'extra-peak-fact':'stack stage semantic result differs: runtime',
    'changed-terminal-audit':'stack stage semantic result differs: runtime',
}

def peak_provenance_fixtures(evidence):
    """Actual comparator, captured facts and labeled synthetic path mutations."""
    import validation_stages as STAGES
    G=module('stack-guard-gate');M=module('memory-portable-replay')
    recorded=json.loads(STAGES.runtime_receipt(evidence,'stack').read_text())['result']
    original=E.identity(recorded);policy=lambda row:row['inherited']['invalidation']['policy']
    def prepared():
        observed=copy.deepcopy(recorded);transport=object.__new__(M.Transport);transport.paths={}
        for i,(old,new) in enumerate(zip(recorded['taskman_peak'],observed['taskman_peak'])):
            origin=policy(old)['observation']['logfile'];target=Path('/portable-peak-proof')/str(i)/'qemu-serial.log'
            transport.paths[origin]=target;policy(new)['observation']['logfile']=str(target)
        return observed,transport
    G.compare_runtime(recorded,copy.deepcopy(recorded))
    observed,transport=prepared();G.compare_runtime(recorded,observed,transport)
    cases=[]
    for name,boundary in PEAK_PROVENANCE_CASES.items():
        observed,transport=prepared();first=observed['taskman_peak'][0]
        origin=policy(recorded['taskman_peak'][0])['observation']['logfile']
        if name=='missing-peak':observed['taskman_peak'].pop()
        elif name=='swapped-peaks':observed['taskman_peak'].reverse()
        elif name=='unindexed-origin':del transport.paths[origin]
        elif name=='wrong-transport-target':transport.paths[origin]=Path('/unrelated/qemu-serial.log')
        elif name=='wrong-observed-log':policy(first)['observation']['logfile']='/unrelated/qemu-serial.log'
        elif name in ('changed-vm','changed-boot','changed-candidate'):
            policy(first)[{'changed-vm':'vm_id','changed-boot':'boot_id','changed-candidate':'candidate_id'}[name]]='SYNTHETIC_WRONG_IDENTITY'
        elif name=='changed-guard-fact':first['guarded_stacks']['status']='SYNTHETIC_FAIL'
        elif name=='missing-run-facts':observed['runs'].pop(next(iter(observed['runs'])))
        elif name=='extra-peak-fact':first['synthetic_extra_fact']=True
        elif name=='changed-terminal-audit':first['terminal_audit']['synthetic_missing_tail']=True
        try:G.compare_runtime(recorded,observed,transport)
        except ValueError as error:
            reason=str(error);E.require(boundary in reason,'stack peak fixture unrelated rejection: '+name+': '+reason)
        else:raise ValueError('stack peak provenance contradiction accepted: '+name)
        cases.append({'name':name,'expected_boundary':boundary,'actual_reason':reason,'synthetic':True})
    observed,transport=prepared();G.compare_runtime(recorded,observed,transport)
    E.require(E.identity(recorded)==original,'stack peak fixtures changed original facts')
    return {'status':'PASS','positive':3,'negative':len(cases),'base_sha256':original,'cases':cases,
        'authority':'ACTUAL_COMPARATOR_WITH_CAPTURED_FACTS_AND_SYNTHETIC_PATHS'}

def verify_receipt(row):
    import stack_native_audit
    stack_native_audit.verify_fixture_receipt(row['native_service'])
    peak=row['peak_provenance']
    E.require(peak['status']=='PASS' and peak['positive']==3 and peak['negative']==len(PEAK_PROVENANCE_CASES) and
        peak['base_sha256'] and peak['authority']=='ACTUAL_COMPARATOR_WITH_CAPTURED_FACTS_AND_SYNTHETIC_PATHS' and
        len(peak['cases'])==len(PEAK_PROVENANCE_CASES) and
        {c['name'] for c in peak['cases']}==set(PEAK_PROVENANCE_CASES),'stack peak provenance fixture coverage')
    for case in peak['cases']:
        E.require(case['synthetic'] is True and case['expected_boundary']==PEAK_PROVENANCE_CASES[case['name']] and
            case['expected_boundary'] in case['actual_reason'],'stack peak provenance fixture boundary')
    E.require(row['schema']==1 and row['status']=='PASS' and len(row['cases'])==len(CASES) and
        {c['name'] for c in row['cases']}==set(CASES) and row['restored_positives']==POSITIVES,
        'stack fixture coverage incomplete')
    for case in row['cases']:
        E.require(case['synthetic'] is True and case['actual_exit']==1 and
            case['expected_boundary']==CASES[case['name']] and case['expected_boundary'] in case['actual_reason'] and
            case['recipe']['base_sha256'],'stack negative reached unrelated boundary')
    E.require(row['originals_rechecked']>0 and row['authority']=='HOST_VERIFIER_MUTATIONS_OF_ACTUAL_CAPTURES',
        'stack fixture authority or original recheck absent')

def fixture_originals(bases):
    """Snapshot raw fixture inputs; full binaries have separate local receipts."""
    originals={}
    for root in bases:
        for path in root.rglob('*'):
            if not path.is_file():continue
            E.require(not path.is_symlink(),'stack fixture original is a symlink')
            size=path.stat().st_size
            with path.open('rb') as stream:prefix=stream.read(4)
            if R.local_binary_only(path.suffix,prefix,size,False):continue
            originals[path]=(E.sha(path),size,path.stat().st_mode&0o777)
    return originals

def run(evidence,output,diagnostic=None):
    F=module('kernel-wx-fixtures');T=module('verify-tlb-shootdown');G=module('stack-guard-gate')
    if diagnostic is None:diagnostic=evidence/json.loads((evidence/'direct-owner-readback.json').read_text())['path']
    output.mkdir(parents=True,exist_ok=False)
    plan,active,coverage,topology,_=F.inputs(diagnostic)
    E.require(active['nx'] and len(topology)>1,'stack fixtures require actual NX multiprocessor capture')
    audits=[T.rows(block.decode().splitlines()) for block in T.trace_blocks(diagnostic)]
    original_proof=W.physical_tables(diagnostic,active,coverage)
    S.verify_directory(diagnostic,plan,active,coverage,topology);S.maintenance(diagnostic,audits,active['root'],topology)
    experiments=json.loads((evidence/'stack-experiments.json').read_text())['runs']
    bases=[diagnostic,evidence/'candidates/selftest',evidence/'candidates/production',
        *[evidence/r['path'] for r in experiments]]
    originals=fixture_originals(bases)
    cases=[]
    def reject(name,action,base,recipe):
        try:action()
        except (ValueError,KeyError,AssertionError) as error:
            reason=str(error);E.require(CASES[name] in reason,'stack unrelated rejection '+name+': '+reason)
        else:raise ValueError('stack contradiction accepted: '+name)
        cases.append({'name':name,'synthetic':True,'actual_exit':1,'expected_boundary':CASES[name],
            'actual_reason':reason,'recipe':{'base_sha256':base,'mutation':recipe}})
    with tempfile.TemporaryDirectory(prefix='stack-semantic-negative-') as temporary:
        work=Path(temporary)/'table';work.mkdir();F.copy_table_dependencies(diagnostic,work)
        data=(work/'table-frames.bin').read_bytes();meta=json.loads((work/'table-snapshot.json').read_text())
        proof=json.loads((work/'stack-owner-readback.json').read_text())
        offsets={pa:i*4096 for i,pa in enumerate(meta['frame_addresses'])};mask=((1<<active['physical_bits'])-1)&~4095
        def frames(raw):return {pa:raw[at:at+4096] for pa,at in offsets.items()}
        def raw_readback():return S.readback(work,plan,coverage,active,topology,frames((work/'table-frames.bin').read_bytes()))
        observed=raw_readback();inventory=observed['inventory'];task=next(v for v in inventory.values() if v['kind']==1)
        def path(va):
            table=active['root'];result=[]
            for level in (4,3,2,1):
                at=offsets[table]+((va>>(12+9*(level-1)))&511)*8;entry=struct.unpack_from('<Q',data,at)[0]
                result.append((at,entry))
                if not entry&1 or level==1 or level in (2,3) and entry&128:return result
                table=entry&mask
            raise ValueError('stack fixture path depth')
        table_cases=list(CASES)[:10]
        for name in table_cases:
            changed=bytearray(data)
            def put(at,value):struct.pack_into('<Q',changed,at,value)
            target=plan['stacks']['guard'][0] if name=='bsp-guard-present' else task['guard']
            if 'guard' in name:
                chain=path(target);at,old=chain[-1]
                if name=='guard-covered-by-large-leaf':at,old=chain[-2];put(at,(task['physical']&~((1<<21)-1))|0x83|W.NX)
                else:put(at,task['physical']|1|(0 if name.endswith('-ro') else 2)|W.NX)
            elif name=='unowned-slot-mapping':
                slot=next(i for i in range(S.CPU_SLOTS,S.SLOTS) if i not in inventory)
                at,old=path(S.BASE+slot*S.PITCH+4096)[-1];E.require(old==0,'stack synthetic unused slot is present')
                put(at,task['physical']|3|W.NX)
            else:
                chain=path(task['base']);at,old=chain[-1]
                if name=='payload-missing':put(at,0)
                elif name=='payload-readonly':put(at,old&~2)
                elif name=='payload-wrong-physical':put(at,old^4096)
                elif name=='payload-ancestor-readonly':put(chain[0][0],chain[0][1]&~2)
                else:
                    for at,old in chain:put(at,old&~W.NX)
            F.synthetic_tables(work,meta,changed)
            action=(lambda:W.physical_tables(work,active,coverage)) if name=='unowned-slot-mapping' else raw_readback
            reject(name,action,meta['frames_sha256'],{'table_bytes_sha256':E.sha(work/'table-frames.bin'),
                'outer_metadata':'SYNTHETIC_HASH_REBOUND'})
            F.synthetic_tables(work,meta,data);E.require(raw_readback()==observed,'stack physical positive changed')
        symbols=(work/'symbols.txt').read_text();value=plan['symbols']['bsp_stack_guard_start']
        changed=symbols.replace(f'{value:016x} B bsp_stack_guard_start',f'{value-4096:016x} B bsp_stack_guard_start')
        E.require(changed!=symbols,'stack linker negative target absent');(work/'symbols.txt').write_text(changed)
        reject('bsp-linker-hole-widened',lambda:W.elf_plan(work),E.sha(diagnostic/'symbols.txt'),'guard start moved one page into live BSS')
        (work/'symbols.txt').write_text(symbols);E.require(W.elf_plan(work)==plan,'stack linker positive changed')
        owner_cases={
            'owner-missing-ist':lambda rows:rows.pop(next(i for i,r in enumerate(rows) if int(r['kind'])==4)),
            'owner-wrong-cpu':lambda rows:next(r for r in rows if int(r['kind'])==3).update(cpu='31'),
            'owner-task-capacity-reduced':lambda rows:next(r for r in rows if int(r['kind'])==1).update(capacity='12288',end=str(task['base']+12288)),
            'owner-immutable-image-backing':lambda rows:next(r for r in rows if int(r['kind'])==1).update(physical=str(plan['first'])),
            'owner-static-backing-owned':lambda rows:next(r for r in rows if int(r['kind'])==4 and r['owned']=='0').update(owned='1')}
        for name,change in owner_cases.items():
            rows=copy.deepcopy(observed['owner_rows']);change(rows)
            reject(name,lambda:S.owners(rows,plan,coverage,topology),E.identity(observed['owner_rows']),rows)
            S.owners(observed['owner_rows'],plan,coverage,topology)
        originals_work={p.name:p.read_bytes() for p in work.iterdir() if p.is_file()}
        def restore():
            for name,raw in originals_work.items():(work/name).write_bytes(raw)
        raw_cases=['owner-writer-interrupted','count-writer-interrupted','ap-tss-unguarded','actual-rsp-unguarded',
            'actual-tr-wrong','one-cpu-wp-clear','raw-cpu-relabeled','mixed-owner-candidate','missing-owner-bytes']
        for name in raw_cases:
            p=copy.deepcopy(proof)
            if name in ('owner-writer-interrupted','count-writer-interrupted','missing-owner-bytes'):
                obj='g_stack_count_sequence' if name=='count-writer-interrupted' else 'g_stack_owners'
                target=work/('stack-owner-'+obj+'.bin');raw=bytearray(target.read_bytes())
                if name=='missing-owner-bytes':raw=raw[:-8]
                else:struct.pack_into('<Q',raw,task['slot']*400 if obj=='g_stack_owners' else 0,1)
                target.write_bytes(raw);p['objects'][obj]['sha256']=E.sha(target)
            elif name=='ap-tss-unguarded':
                child=next(r for r in p['children'] if r['slot']==1);raw=bytearray.fromhex(child['bytes'])
                struct.pack_into('<Q',raw,36,W.BASE+task['physical']+16384);child['bytes']=raw.hex()
            elif name=='actual-rsp-unguarded':p['registers'][0]['registers']['rsp']=W.BASE+task['physical']+8192
            elif name=='one-cpu-wp-clear':p['registers'][0]['registers']['cr0']&=~0x10000
            elif name=='actual-tr-wrong':
                target=work/'stack-descriptors.log';text=target.read_text();text=text.replace('TR =0028','TR =0030',1)
                E.require(text!=target.read_text(),'stack TR negative target missing');target.write_text(text);p['descriptors_sha256']=E.sha(target)
            elif name=='raw-cpu-relabeled':
                cfg=json.loads((work/'capture-config.json').read_text());cfg['cpu_identity'][0]['apic_id']=255
                (work/'capture-config.json').write_bytes(E.canonical(cfg))
            else:p['candidate_id']='0'*64
            (work/'stack-owner-readback.json').write_bytes(E.canonical(p))
            reject(name,raw_readback,E.identity(proof),{'owner_metadata_sha256':E.identity(p),'outer_metadata':'SYNTHETIC_HASH_REBOUND'})
            restore();E.require(raw_readback()==observed,'stack raw owner positive changed')
        shutil.copyfile(diagnostic/'framed.jsonl',work/'framed.jsonl')
        serial=(work/'qemu-serial.log').read_text();serial_rows=S.serial_records(diagnostic)
        def encode(rows):
            records=[]
            for kind,row in rows:
                body=('[STACK_GUARD]['+kind+'] '+' '.join(k+'='+v for k,v in row.items())).encode()
                records.append(body.decode()+f' crc={zlib.crc32(body):08x}')
            cursor=iter(records);out=[]
            for line in serial.splitlines():
                if line.startswith('[STACK_GUARD]'):out.append(next(cursor,''))
                else:out.append(line)
            out.extend(cursor);return '\n'.join(line for line in out if line)+'\n'
        mutations={
            'guard-wrong-rip':('FAULT','rip','0'),'guard-wrong-root':('FAULT','root','1000'),
            'guard-wrong-cpu':('FAULT','actor_cpu','31'),'guard-wrong-apic':('FAULT','actor_apic','255'),
            'guard-wrong-address':('FAULT','cr2','0'),'guard-stale-owner':('FAULT','owner_generation','0'),
            'guard-protection-error':('FAULT','error','1'),'guard-write-error':('FAULT','error','2'),
            'guard-user-error':('FAULT','error','4'),'guard-reserved-error':('FAULT','error','8'),
            'guard-fetch-error':('FAULT','error','10'),'guard-not-observed':('FAULT','seen','0'),
            'guard-stale-generation':('FAULT','generation','1'),
            'churn-fake-migration':('CHURN','migrated','0'),'churn-no-reaper-overlap':('CHURN','reaped_during_creation','0'),
            'churn-resource-leak':('CHURN','owned_bytes','1')}
        for name in [*mutations,'guard-canary-changed','guard-owner-missing']:
            rows=copy.deepcopy(serial_rows)
            if name in mutations:
                kind,key,value=mutations[name];matches=[r for k,r in rows if k==kind]
                matches[1 if name=='guard-stale-generation' else 0][key]=value
            elif name=='guard-canary-changed':next(r for k,r in rows if k=='OWNER' and S.serial_number(r,'kind')==1)['canary']='4096'
            else:rows.pop(next(i for i,(k,r) in enumerate(rows) if k=='OWNER' and S.serial_number(r,'kind')==4))
            synthetic=encode(rows);(work/'qemu-serial.log').write_text(synthetic)
            reject(name,lambda:S.verify_directory(work,plan,active,coverage,topology),E.sha(diagnostic/'qemu-serial.log'),
                {'records_sha256':E.identity(rows),'outer_metadata':'SYNTHETIC_CRC_REBOUND'})
            (work/'qemu-serial.log').write_text(serial);S.verify_directory(work,plan,active,coverage,topology)
        for name in ('retirement-one-ack-missing','retirement-queued-as-complete','retirement-tail-missing',
                     'retirement-audit-missing','retirement-wrong-owner'):
            rows=copy.deepcopy(serial_rows);changed=copy.deepcopy(audits)
            target=next(r for k,r in rows if k=='RETIRE' and any(k2=='ALLOC' and
                r2['slot']==r['slot'] and r2['generation']==r['generation'] for k2,r2 in rows))
            if name=='retirement-one-ack-missing':target['acknowledged']='1'
            elif name=='retirement-queued-as-complete':target['status']='QUEUED'
            elif name=='retirement-tail-missing':changed[-1]['WORKER_REAP'].pop()
            elif name=='retirement-audit-missing':
                block=next(a for a in changed if any(W.n(r,'generation')==S.serial_number(target,'request') for r in a['SERVICE']))
                block['SERVICE']=[r for r in block['SERVICE'] if W.n(r,'generation')!=S.serial_number(target,'request')]
            else:target['generation']=str(S.serial_number(target,'generation')+1000)
            (work/'qemu-serial.log').write_text(encode(rows))
            reject(name,lambda:S.maintenance(work,changed,active['root'],topology),E.sha(diagnostic/'qemu-debugcon.log'),
                {'records_sha256':E.identity(rows),'audit_sha256':E.identity(changed),'outer_metadata':'SYNTHETIC_CRC_REBOUND'})
            (work/'qemu-serial.log').write_text(serial);S.maintenance(work,audits,active['root'],topology)
        for kind in ('stack-overflow','stack-timeout'):
            origin=evidence/next(r['path'] for r in experiments if r['experiment']==kind and 'kvm' in r['path'])
            fatal=Path(temporary)/kind;fatal.mkdir()
            for name in ('symbols.txt','qemu-serial.log','commands.jsonl'):shutil.copyfile(origin/name,fatal/name)
            raw=(fatal/'qemu-serial.log').read_text();result=json.loads((origin/'result.json').read_text())
            commands=[json.loads(line) for line in (origin/'framed.jsonl').read_text().splitlines()]
            lines=raw.splitlines();cpus={int(r['slot']):(int(r['apic']),r['role']) for r in W.LOW.records(lines,'[SMP][CPU] ONLINE ')}
            def check():return S.destructive_records(fatal,(fatal/'qemu-serial.log').read_text().splitlines(),cpus,commands,result,kind)
            expected=check();prefix='[STACK_FAULT] ' if kind=='stack-overflow' else '[STACK_GUARD][TIMEOUT] '
            original=next(line for line in lines if line.startswith(prefix))
            changes={'overflow-stale-cr2':('cr2','0000000000000000'),'overflow-vector14':('vector','14'),
                'overflow-false-task':('owner_task','9999'),'overflow-no-ist':('handler_ist1','0'),
                'overflow-exact-rip-claim':('rip_authority','PRECISE'),'overflow-canary-substitute':('classification','CANARY_ONLY')}
            if kind=='stack-timeout':changes={'timeout-frees-backing':('retained_pages','0'),
                'timeout-queued-as-complete':('result','1'),'timeout-reuses-va':('reuse_result','0')}
            for name,(key,value) in changes.items():
                import re
                line=re.sub(r'\b'+key+r'=\S+',key+'='+value,original)
                if kind=='stack-timeout':body=line.rsplit(' crc=',1)[0];line=body+f' crc={zlib.crc32(body.encode()):08x}'
                E.require(line!=original,'stack fatal mutation had no effect');(fatal/'qemu-serial.log').write_text(raw.replace(original,line,1))
                reject(name,check,E.sha(origin/'qemu-serial.log'),{'field':key,'value':value,'outer_metadata':'SYNTHETIC_CRC_REBOUND'})
                (fatal/'qemu-serial.log').write_text(raw);E.require(check()==expected,'stack destructive positive changed')
        import memory_elf_proof as ELF
        elf_proof=json.loads((evidence/'candidates/selftest/image-elf-proof.json').read_text());elf=ELF.replay(elf_proof)
        S.fixed_probe_ranges(elf)
        def patch(proof,offset,value):
            chunk=next(r for r in proof['chunks'] if r['offset']<=offset and offset+len(value)<=r['offset']+r['size'])
            raw=bytearray.fromhex(chunk['hex']);at=offset-chunk['offset'];raw[at:at+len(value)]=value
            chunk['hex']=raw.hex();chunk['sha256']=hashlib.sha256(raw).hexdigest()
        for name in ('probe-load-opcode','overflow-step-opcode','probe-recovery-offset'):
            changed=copy.deepcopy(elf_proof)
            if name=='probe-recovery-offset':
                symtab=elf.section('.symtab');symbols=elf.symbols();index=next(i for i,r in enumerate(symbols) if r['name']=='guard_probe_recovery')
                patch(changed,symtab['offset']+index*24+8,struct.pack('<Q',elf.symbol('guard_probe_recovery')['value']+1))
            else:
                symbol=elf.symbol('guard_probe_read' if name=='probe-load-opcode' else 'guard_overflow_enter');text=elf.section('.text')
                patch(changed,text['offset']+symbol['value']-text['addr']+(0 if name=='probe-load-opcode' else 4),b'\x90')
            reject(name,lambda:S.fixed_probe_ranges(ELF.replay(changed)),E.identity(elf_proof),
                {'proof_sha256':E.identity(changed),'outer_metadata':'SYNTHETIC_FRAGMENT_HASH_REBOUND'})
            S.fixed_probe_ranges(ELF.replay(elf_proof))
    production=evidence/json.loads((evidence/'production.json').read_text())['runs'][0]
    raw=(production/'qemu-serial.log').read_bytes();lines=[];ends=[];offset=0
    for line in raw.splitlines(keepends=True):offset+=len(line);lines.append(line.rstrip(b'\r\n').decode());ends.append(offset)
    result=json.loads((production/'result.json').read_text());profile={'name':result['profile'],'vm_id':result['vm_id']}
    O=module('verify-memory-observation')
    for name,command in (('production-guard-command','stacktest'),('production-overflow-command','stacktest overflow')):
        row=next(json.loads(line) for line in (production/'framed.jsonl').read_text().splitlines() if json.loads(line)['payload']==command)
        action=lambda r:O.production_absence_transaction(r,production,lines,ends,profile,result['candidate_elf'])
        action(row);reject(name,lambda:action(dict(row,handler_status=0)),E.identity(row),{'handler_status':0});action(row)
    E.require(W.physical_tables(diagnostic,active,coverage)==original_proof,'stack original table proof changed')
    E.require(all((E.sha(path),path.stat().st_size,path.stat().st_mode&0o777)==facts for path,facts in originals.items()),
        'stack negatives changed immutable originals')
    result={'schema':1,'status':'PASS','authority':'HOST_VERIFIER_MUTATIONS_OF_ACTUAL_CAPTURES',
        'cases':cases,'restored_positives':POSITIVES,'originals_rechecked':len(originals),
        'diagnostic':{'candidate_id':json.loads((diagnostic/'result.json').read_text())['candidate_id'],
            'serial_sha256':E.sha(diagnostic/'qemu-serial.log'),'table_sha256':E.sha(diagnostic/'table-frames.bin')}}
    import stack_native_audit
    result['native_service']=stack_native_audit.fixture_cases(evidence)
    result['peak_provenance']=peak_provenance_fixtures(evidence)
    verify_receipt(result);E.write(output/'results.json',result);return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--diagnostic',type=Path)
    args=parser.parse_args();print(json.dumps(run(args.evidence,args.output,args.diagnostic),sort_keys=True))
