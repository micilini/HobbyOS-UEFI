#!/usr/bin/env python3
"""Adversarial transport-only controls; synthetic records never certify work."""
import argparse, copy, json, zlib, hashlib
from pathlib import Path
import memory_evidence as E
import tlb_proof_evidence as P


def run(output):
    output.mkdir(parents=True,exist_ok=False)
    cases=[]
    for mode in ('deferred','immediate'):
        plan=dict(role='DIAGNOSTIC_NO_ACCEPTANCE',candidate_id='synthetic',vm_ids=['hosted'],profile={'smp':8},
            proof_output=dict(method=P.CONTRACT,policy=mode,session='0000000000000042',invocations=1))
        plan['commands']=[f'irqlatency proof {mode} 0000000000000042 1','irqlatency begin','mem',
            'irqlatency stress','irqlatency finish','irqlatency audit','irqlatency export']
        body=['[TLB][BEGIN] schema=7 invocation=1 cpus=8 x2apic=1 sequential=256 concurrent_each=64 threshold=64 deadline_ns=1000000000 window_pages=65 deferred=1 observers=3',
              '[TLB][END] invocation=1 status=PASS replacements=385 retained=0']
        raw=''.join(s+f' crc={zlib.crc32(s.encode()):08x}\n' for s in body).encode()
        size=len(raw);digest=P.digest(raw);rights,count,capacity=P.bound(8,1,17)
        receipt=f'[TLB_TRACE][CAPTURED] invocation=1 channel=debugcon port=0x402 records=2 bytes={size} fnv1a=0x{digest:x} status=PASS'
        lines=[];commands=[]
        for payload in plan['commands']:
            lines.append('command begin');begin=len(lines)
            if payload.startswith('irqlatency proof '):
                lines.append(f'[TLB_PROOF][PREPARED] method={P.CONTRACT} session=0x42 policy={mode} cpus=8 invocations=1 capacity={capacity} record_bound={count} line_bound=766 baseline_runs=17 rights_bound={rights} owned_until=VM_END')
            if payload=='irqlatency stress':lines.append(receipt)
            if payload=='irqlatency audit':lines.append('[IRQ_LATENCY][AUDIT_END] epoch=1 admitted=9 completed=9 emitted=9 status=COMPLETE crc=12345678')
            if payload=='irqlatency export':
                lines.append(f'[TLB_PROOF][SEALED] method={P.CONTRACT} session=0x42 policy={mode} invocations=1 bytes={size} records=2 fnv1a=0x{digest:x} service=9 capture_port_bytes={0 if mode=="deferred" else size}')
                lines.append(f'[TLB_PROOF][EXPORTED] method={P.CONTRACT} session=0x42 bytes={size} records=2 fnv1a=0x{digest:x} service=9 export_port_bytes={size if mode=="deferred" else 0} status=COMPLETE')
            lines.append('command end')
            commands.append(dict(payload=payload,sequence=len(commands)+1,begin_line=begin,end_line=len(lines),handler_status=0,candidate_id='synthetic',vm_id='hosted'))
        serial='\n'.join(lines)
        prefix=raw if mode=='immediate' else b''
        commands[-1]['proof_prefix']=dict(bytes=len(prefix),sha256=hashlib.sha256(prefix).hexdigest(),candidate_id='synthetic',vm_id='hosted',sequence=len(commands),monotonic_ns=100)
        def transport(s,b):
            sidebands=[x for x in s.splitlines() if x.startswith(('[TLB_PROOF][SEALED]','[TLB_PROOF][EXPORTED]'))]
            clean='\n'.join('wire receipt' if x in sidebands else x for x in s.splitlines())
            if len(sidebands)!=2:return clean,b
            seal,end=(x.encode()+b'\n' for x in sidebands)
            return clean,(b+seal+end if mode=='immediate' else seal+b+end)
        clean,wire=transport(serial,raw)
        assert P.verify(clean,wire,plan,commands)['bytes']==size
        cases.append(dict(name=mode+'-exact-transport',status='PASS'))
        try:P.verify(serial,wire,plan,commands)
        except ValueError as e:cases.append(dict(name=mode+'-masked-serial-receipts',status='REJECTED',reason=str(e)))
        else:raise AssertionError('serial export receipts accepted')
        def reject(name,s=serial,b=raw,p=plan,c=commands):
            try:
                clean,wire=transport(s,b)
                P.verify(clean,wire,p,c)
            except (ValueError,KeyError):cases.append(dict(name=mode+'-'+name,status='REJECTED'));return
            raise AssertionError(name)
        for name,s in [('missing-seal','\n'.join(x for x in lines if '[SEALED]' not in x)),
            ('missing-export','\n'.join(x for x in lines if '[EXPORTED]' not in x)),
            ('wrong-session',serial.replace('session=0x42','session=0x43')),
            ('wrong-generation',serial.replace('service=9','service=8')),
            ('early-seal',serial.replace('[TLB_PROOF][PREPARED]','[TLB_PROOF][SEALED]')),
            ('legacy-relabel',serial.replace('[CAPTURED]','[COMPLETE]')),
            ('unknown-schema',serial.replace(P.CONTRACT,P.CONTRACT+'x')),
            ('changed-capacity',serial.replace(f'capacity={capacity}',f'capacity={capacity-1}')),
            ('hidden-output',serial.replace(f'capture_port_bytes={0 if mode=="deferred" else size}',f'capture_port_bytes={1 if mode=="deferred" else 0}'))]:reject(name,s=s)
        for name,b in [('prefix',raw[1:]),('middle',raw[:33]+raw[34:]),('tail',raw[:-1]),('duplicate',raw+raw),('reordered',b''.join(reversed(raw.splitlines(keepends=True))))]:reject(name,b=b)
        for key in ('candidate_id','vm_id'):
            altered=copy.deepcopy(commands);altered[-1][key]='different';reject('wrong-'+key,c=altered)
        altered=copy.deepcopy(commands);altered[-1]['handler_status']=1;reject('failed-export',c=altered)
        altered=copy.deepcopy(commands);del altered[-1]['proof_prefix'];reject('missing-prefix',c=altered)
        for key,value in [('bytes',1),('sha256','0'*64),('candidate_id','other'),('vm_id','other'),('sequence',999)]:
            altered=copy.deepcopy(commands);altered[-1]['proof_prefix'][key]=value;reject('wrong-prefix-'+key,c=altered)
        reject('old-current-method',s=serial.replace(P.CONTRACT,'immutable-tlb-proof-v1'))
        altered=copy.deepcopy(plan);altered['commands'].append('mem');reject('late-work',p=altered)
        altered=copy.deepcopy(plan);del altered['proof_output'];reject('historical-selector',p=altered)
        if mode=='immediate':
            altered=copy.deepcopy(plan);altered['role']='CURRENT_TIMING_QUALIFICATION';reject('contrast-promoted',p=altered)
        # Per-record checksum still rejects a changed record even with a freshly
        # forged outer byte/digest receipt. It is not the source of the oracle.
        changed=raw.replace(b'replacements=385',b'replacements=384')
        reject('crc-under-resealed-stream',b=changed,s=serial.replace(f'{digest:x}',f'{P.digest(changed):x}'))
    result=dict(status='PASS',authority='HOSTED_TRANSPORT_ONLY',cases=cases,implementation_acceptance=False)
    E.write(output/'result.json',result);return result


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    print(json.dumps(run(a.output)))
