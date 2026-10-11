"""Additive entry checkpoints; original envelope remains the qualification rule."""
import irq_latency_evidence as V

CONTRACT='entry-return-checkpoints-v2'

def verify(raw,cpus):
    probes=V.index(raw['OVERHEAD'],('role','index','probe'))
    points=V.index(raw['ENTRY_CHECKPOINT'],('role','index','probe'))
    V.require(set(points)==set(probes)=={(r,i,p) for r in (0,1) for i in range(64*cpus) for p in range(4)},
              'complete additive entry ledger')
    rows=[]
    for key in sorted(probes):
        p=probes[key];q=points[key]
        V.require(q['method']=='entry-return-checkpoints' and V.number(q,'version')==2 and V.number(q,'epoch')==1,
                  'unknown entry checkpoint method')
        V.require(q['cpu']==p['cpu'] and q['sequence']==p['sequence_after'],'entry checkpoint identity')
        if not V.number(p,'valid'):continue
        a,b,c,d=(V.number(p,k) for k in ('outer_begin','inner_begin','inner_end','outer_end'))
        t=V.number(q,'tsc');u=V.number(q,'exit_tsc')
        V.require(a<=t<=b<c<=u<=d,'entry checkpoint timestamp outside original enclosure')
        rows.append(dict(role=key[0],index=key[1],probe=key[2],cpu=V.number(p,'cpu'),
                         residual=t-a,saved_registers_to_inner=b-t,leading=b-a,interior=c-b,
                         trailing=d-c,tail_publication=u-c,return_residual=d-u,envelope=b-a+d-c,checkpoint=t,exit_checkpoint=u))
    maxima=[]
    for role in (0,1):
        for cpu in range(cpus):
            group=[r for r in rows if r['role']==role and r['cpu']==cpu]
            V.require(group,'entry checkpoint CPU coverage')
            maxima.append(dict(role=role,cpu=cpu,eligible=len(group),worst=max(group,key=lambda r:r['envelope']),
                worst_residual=max(group,key=lambda r:r['residual']),
                worst_saved_registers_to_inner=max(group,key=lambda r:r['saved_registers_to_inner']),
                worst_tail_publication=max(group,key=lambda r:r['tail_publication']),
                worst_return_residual=max(group,key=lambda r:r['return_residual'])))
    return dict(status='PASS',contract=CONTRACT,authority='ADDITIVE_DIAGNOSTIC_ONLY',maxima=maxima,
                original_envelope_changed=False,unique_cause='NOT_PROVEN',timing_acceptance=False)
