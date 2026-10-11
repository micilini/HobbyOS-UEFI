"""Reconstruct current immutable TLB capture/export, independently of handler PASS."""
import re
import hashlib
import memory_evidence as E

CONTRACT = 'immutable-tlb-proof-v2'
LINE_BYTES = 766
OWNER_COUNT = 4*32+1536+1


def bound(cpus, invocations, baseline):
    E.require(1 <= cpus <= 32 and 1 <= invocations <= 3 and 0 < baseline < 8192*512,
              'proof capacity topology/admission')
    rights = baseline + OWNER_COUNT + 2*512 + 2*65
    cycles = 256+1+(128 if cpus > 1 else 0)
    windows = 2 if cpus > 1 else 1
    records = invocations*(cycles*(5+8*cpus)+windows*(2+2*cpus)+
              (3 if cpus > 1 else 2)*(rights+OWNER_COUNT+1)+12)+8192
    return rights, records, records*LINE_BYTES


def contract(plan):
    proof = plan.get('proof_output')
    E.require(isinstance(proof, dict) and set(proof)=={'method','policy','session','invocations'} and
              proof['method']==CONTRACT and proof['policy'] in ('immediate','deferred') and
              re.fullmatch('[0-9a-f]{16}', proof['session']) and int(proof['session'],16)>0,
              'current proof output method/policy/session')
    count = 1+sum(p in ('tlbtest','tlbtest run') for p in plan['commands'])
    E.require(proof['invocations']==count and 1<=count<=3 and
              plan['commands'][:2]==[f"irqlatency proof {proof['policy']} {proof['session']} {count}", 'irqlatency begin'] and
              plan['commands'][-4:]==['irqlatency stress','irqlatency finish','irqlatency audit','irqlatency export'],
              'proof plan ordinary work/setup/export chronology')
    E.require(proof['policy']=='deferred' or plan['role']=='DIAGNOSTIC_NO_ACCEPTANCE',
              'immediate transport contrast cannot provide current acceptance')
    return proof


def digest(data):
    value=14695981039346656037
    for byte in data:value=((value^byte)*1099511628211)&0xffffffffffffffff
    return value


def wire(debugcon):
    E.require(isinstance(debugcon,bytes) and debugcon.endswith(b'\n'),'proof torn wire')
    metadata=[];payload=[];offset=0
    for line in debugcon.splitlines(keepends=True):
        if line.startswith(b'[TLB_PROOF]'):
            E.require(len(line)<384,'proof sideband length')
            try:text=line.decode('ascii').rstrip('\n')
            except UnicodeDecodeError:raise ValueError('proof sideband encoding')
            metadata.append((offset,text))
        else:payload.append(line)
        offset+=len(line)
    E.require(len(metadata)==2 and metadata[0][1].startswith('[TLB_PROOF][SEALED] ') and
              metadata[1][1].startswith('[TLB_PROOF][EXPORTED] ') and
              metadata[1][0]+len(metadata[1][1])+1==len(debugcon),'proof sideband order/tail')
    return b''.join(payload),metadata


def verify(serial, debugcon, plan, commands, index=0):
    policy=contract(plan);lines=serial.splitlines();payload,metadata=wire(debugcon)
    E.require(not any(line.startswith(('[TLB_PROOF][SEALED]','[TLB_PROOF][EXPORTED]')) for line in lines),'proof export used masked serial channel')
    E.require([r['payload'] for r in commands]==plan['commands'] and
              all(r['candidate_id']==plan['candidate_id'] and r['vm_id']==plan['vm_ids'][index] for r in commands),
              'proof command candidate/VM mismatch')
    def row(kind, keys):
        matches=[(i,line) for i,line in (enumerate(lines,1) if kind=='PREPARED' else metadata) if line.startswith('[TLB_PROOF]['+kind+'] ')]
        E.require(len(matches)==1,'proof missing/duplicate '+kind)
        pos,line=matches[0];fields={}
        for part in line.split()[1:]:
            k,sep,v=part.partition('=')
            E.require(sep and k not in fields,'proof field grammar');fields[k]=v
        E.require(set(fields)==set(keys.split()) and fields['method']==CONTRACT and
                  int(fields['session'],0)==int(policy['session'],16),'proof identity/schema '+kind)
        return pos,fields
    def n(r,k):
        E.require(re.fullmatch(r'(0x[0-9a-f]+|0|[1-9][0-9]*)',r[k]),'proof numeric grammar')
        value=int(r[k],0);E.require(0<=value<2**64,'proof numeric range');return value
    pp,p=row('PREPARED','method session policy cpus invocations capacity record_bound line_bound baseline_runs rights_bound owned_until')
    sp,s=row('SEALED','method session policy invocations bytes records fnv1a service capture_port_bytes')
    ep,e=row('EXPORTED','method session bytes records fnv1a service export_port_bytes status')
    rights,records,capacity=bound(plan['profile']['smp'],policy['invocations'],n(p,'baseline_runs'))
    E.require(p['policy']==s['policy']==policy['policy'] and
              n(p,'cpus')==plan['profile']['smp'] and n(p,'invocations')==n(s,'invocations')==policy['invocations'] and
              n(p,'capacity')==capacity and n(p,'record_bound')==records and n(p,'line_bound')==LINE_BYTES and
              n(p,'rights_bound')==rights and p['owned_until']=='VM_END','proof support capacity/identity')
    E.require(commands[0]['handler_status']==commands[-1]['handler_status']==0 and
              commands[0]['begin_line']<pp<commands[0]['end_line']<commands[1]['begin_line'] and
              commands[-2]['end_line']<commands[-1]['begin_line']<commands[-1]['end_line'] and sp<ep,
              'proof seal/export before terminal audit or outside command')
    prefix=commands[-1].get('proof_prefix')
    E.require(isinstance(prefix,dict) and set(prefix)=={'bytes','sha256','candidate_id','vm_id','sequence','monotonic_ns'} and
              prefix['candidate_id']==plan['candidate_id'] and prefix['vm_id']==plan['vm_ids'][index] and
              prefix['sequence']==commands[-1]['sequence'] and prefix['bytes']==sp and
              prefix['sha256']==hashlib.sha256(debugcon[:sp]).hexdigest(),'proof export prefix identity/boundary')
    middle=debugcon[sp+len(metadata[0][1])+1:ep]
    E.require((not middle if policy['policy']=='immediate' else
               all(line.startswith(b'[TLB]') for line in middle.splitlines()) and b'[TLB]' not in debugcon[:sp]),
              'proof bytes outside declared export phase')
    audits=[line for line in lines if line.startswith('[IRQ_LATENCY][AUDIT_END] ')]
    E.require(len(audits)==1,'proof terminal service audit missing')
    match=re.search(r' admitted=(\d+) completed=(\d+) emitted=(\d+) status=COMPLETE crc=',audits[0])
    E.require(match and len(set(match.groups()))==1 and n(s,'service')==n(e,'service')==int(match[1]),
              'proof service boundary incomplete/changed')
    from device_mapping_evidence import split_traces
    blocks=split_traces(payload,maximum_bytes=capacity+32*1024*1024)['TLB']
    E.require(len(blocks)==policy['invocations'],'proof missing/duplicate invocation')
    raw=b''.join(blocks)
    E.require(len(raw)<=capacity and raw.count(b'\n')<=records and
              all(0<len(line)<=LINE_BYTES for line in raw.splitlines(keepends=True)), 'proof capacity exceeded')
    E.require(n(s,'bytes')==n(e,'bytes')==len(raw)>0 and n(s,'records')==n(e,'records')==raw.count(b'\n') and
              n(s,'fnv1a')==n(e,'fnv1a')==digest(raw) and e['status']=='COMPLETE',
              'proof bytes missing/duplicated/reordered/corrupt')
    E.require(n(s,'capture_port_bytes')==(0 if policy['policy']=='deferred' else len(raw)) and
              n(e,'export_port_bytes')==(len(raw) if policy['policy']=='deferred' else 0),
              'proof output occurred in wrong phase')
    workload=[r for r in commands if r['payload'] in ('tlbtest','tlbtest run','irqlatency stress')]
    E.require(len(workload)==len(blocks),'proof workload invocation cardinality')
    import importlib.util
    spec=importlib.util.spec_from_file_location('proof_tlb',E.ROOT/'scripts/verify-tlb-shootdown.py')
    T=importlib.util.module_from_spec(spec);spec.loader.exec_module(T)
    for invocation,(command,block) in enumerate(zip(workload,blocks),1):
        data=T.bind_trace(lines[command['begin_line']-1:command['end_line']],block,proof_policy=CONTRACT)
        rows=T.rows(data)
        E.require(T.n(rows['BEGIN'][0],'invocation')==invocation,'proof invocation order')
        for stage in (0,1,2):
            E.require(sum(T.n(r,'stage')==stage for r in rows.get('RIGHTS',[]))<=rights,
                      'proof admitted scan bound exceeded')
    return dict(status='PASS',method=CONTRACT,policy=policy['policy'],session=policy['session'],
                bytes=len(raw),records=raw.count(b'\n'),fnv1a=f'{digest(raw):016x}',capacity=capacity,
                invocations=len(blocks),service=n(s,'service'),losses=0,
                capture_port_bytes=n(s,'capture_port_bytes'),export_port_bytes=n(e,'export_port_bytes'),
                wire_bytes=len(debugcon),sideband_bytes=sum(len(line)+1 for _,line in metadata),export_prefix_bytes=sp,
                authority='Exact test-proof transport; no timing or implementation acceptance.')
