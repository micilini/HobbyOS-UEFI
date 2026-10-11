"""Bind this VM's work, recorder and acknowledged-service terminal cutoffs."""
import importlib.util,json,re
import memory_evidence as E
import irq_latency_evidence as V
import irq_latency_stack_reports as R
import irq_latency_warning_reports as Q
import request_local_cycles as CYCLES
import irq_latency_pairs as P
import tlb_proof_evidence as PROOF
import irq_latency_workload as WORKLOAD
CONTRACT='service-tail-after-recorder-v4'
CLOSURE='snapshot-before-post-calibration-v1'


def calibration_closure(raw,cpus):
    n=V.number
    E.require(len(raw['BEGIN'])==len(raw['SNAPSHOT_READY'])==1 and
        raw['BEGIN'][0].get('closure')==raw['SNAPSHOT_READY'][0].get('closure')==CLOSURE,
        'current calibration closure declaration')
    ready=raw['SNAPSHOT_READY'][0]
    E.require(n(ready,'cpus')==n(ready,'frozen')==cpus and n(ready,'epoch')==1,
        'calibration before complete terminal snapshot')
    states=V.index(raw['CPU'],('cpu',));bounds=[]
    E.require(set(states)=={(c,) for c in range(cpus)},'calibration cutoff CPU inventory')
    for cpu in range(cpus):
        local=[[p for p in raw['REFERENCE'] if n(p,'role')==role and n(p,'cpu')==cpu and
            P.eligible(p,cpus)==0] for role in (0,1)]
        E.require(all(local),'calibration cutoff local coverage')
        before=max(n(p,'tsc_after') for p in local[0])
        after=min(n(p,'tsc_before') for p in local[1]);cutoff=n(states[(cpu,)],'cutoff')
        E.require(before<cutoff<after,'calibration does not enclose the terminal cutoff')
        for w in raw['WITNESS']:
            if n(w,'cpu')==cpu and n(w,'first')==0 and w['kind']=='maximum' and n(w,'index')<11:
                cells=[int(v) for v in w['cells'].split(',')]
                E.require(len(cells)>=2,'calibration witness timestamps missing')
                if cells[0] or cells[1]:
                    E.require(before<=cells[0]<cells[1]<=cutoff,'witness outside measured calibration epoch')
        bounds.append(dict(cpu=cpu,pre_end=before,cutoff=cutoff,post_begin=after))
    return dict(status='PASS',method=CLOSURE,cpus=bounds)


def tlb():
    spec=importlib.util.spec_from_file_location('terminal_tlb',E.ROOT/'scripts/verify-tlb-shootdown.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m


def verify(serial,debugcon,plan,commands,index=0):
    E.require(plan['terminal_contract']==CONTRACT,'terminal contract unknown')
    current_proof=plan.get('proof_output') is not None
    E.require(plan['commands'][-(4 if current_proof else 3):]==['irqlatency stress','irqlatency finish','irqlatency audit']+(['irqlatency export'] if current_proof else []), 'terminal plan unmeasured trailing workload')
    E.require([r['payload'] for r in commands]==plan['commands'] and
        all(r['vm_id']==plan['vm_ids'][index] and r['candidate_id']==plan['candidate_id'] for r in commands),
        'terminal command VM/candidate binding')
    output_proof=PROOF.verify(serial,debugcon,plan,commands,index) if current_proof else None
    if current_proof:
        commands=commands[1:-1]
        debugcon=PROOF.wire(debugcon)[0]
    raw=V.records(serial);cpus=plan['profile']['smp'];T=tlb();n=T.n
    E.require(current_proof == any(r.get('proof_output')=='2' for r in raw['BEGIN']), 'proof current/historical selector mismatch')
    reports=R.verify(serial);warnings=Q.verify(serial)
    topology={};roots=[];lines=serial.splitlines()
    for line in lines:
        match=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if match:
            slot=int(match[1]);E.require(slot not in topology,'terminal repeated CPU identity');topology[slot]=(int(match[2]),match[3])
        match=re.match(r'\[LOW_ROOT\] ACTIVE root=([0-9a-f]+) ',line)
        if match:roots.append(int(match[1],16))
    E.require(set(topology)==set(range(cpus)) and len(roots)==1,'terminal independent boot topology/root')
    root=roots[0]
    timing_contract=plan.get('request_timing_contract')
    E.require(timing_contract in (None,CYCLES.CONTRACT) and
        (len(raw['BEGIN'])==1 and raw['BEGIN'][0].get('request_timing')==timing_contract if timing_contract else
         not any('request_timing' in r for r in raw['BEGIN'])), 'terminal request timing source/role binding')
    kinds=('WORK_CUTOFF','SNAPSHOT_CUTOFF','AUDIT_BEGIN','AUDIT_END')
    E.require(all(len(raw[k])==1 for k in kinds),'terminal cutoff cardinality')
    work,snapshot,begin,end=(raw[k][0] for k in kinds)
    for row,joined in ((work,0),(snapshot,1)):
        E.require(n(row,'epoch')==1 and n(row,'cpus')==cpus and n(row,'joined')==joined and
            n(row,'completed')<=n(row,'admitted'),'terminal ownership/work cutoff')
    last=n(end,'completed')
    E.require(n(work,'admitted')<=n(snapshot,'admitted')<=n(begin,'admitted')==n(begin,'completed')==
        n(end,'admitted')==last==n(end,'emitted') and n(begin,'root')==root and n(begin,'cpus')==cpus and
        n(begin,'epoch')==n(end,'epoch')==1 and end['status']=='COMPLETE','terminal service outstanding/late request')
    proof=T.verify_service_sequence(raw['SERVICE'],topology,root,1,last)
    positions={k:[i for i,line in enumerate(lines) if line.startswith('[IRQ_LATENCY]['+k+']')] for k in kinds}
    E.require(positions['WORK_CUTOFF'][0]<positions['SNAPSHOT_CUTOFF'][0]<positions['AUDIT_BEGIN'][0]<positions['AUDIT_END'][0], 'terminal cutoff chronology')
    cpupos=[i for i,line in enumerate(lines) if line.startswith('[IRQ_LATENCY][CPU] ')]
    E.require(len(cpupos)==cpus and all(positions['WORK_CUTOFF'][0]<i<positions['SNAPSHOT_CUTOFF'][0] for i in cpupos),'terminal recorder cutoff coverage')
    closure=calibration_closure(raw,cpus)
    ready=[i for i,line in enumerate(lines) if line.startswith('[IRQ_LATENCY][SNAPSHOT_READY] ')]
    post=[i for i,line in enumerate(lines) if line.startswith('[IRQ_LATENCY][REFERENCE] role=1 ')]
    E.require(len(ready)==1 and post and positions['WORK_CUTOFF'][0]<ready[0]<min(post) and
        max(post)<min(cpupos),'post calibration/reporting chronology')
    E.require(commands[-3]['handler_status']==0 and commands[-3]['end_line']<positions['WORK_CUTOFF'][0]+1 and
        commands[-2]['begin_line']<positions['WORK_CUTOFF'][0]+1<positions['SNAPSHOT_CUTOFF'][0]+1<commands[-2]['end_line'] and
        commands[-1]['begin_line']<positions['AUDIT_BEGIN'][0]+1<positions['AUDIT_END'][0]+1<commands[-1]['end_line'],
        'terminal audit or freeze outside actual command')
    services={n(r,'generation'):r for r in raw['SERVICE']}
    stack_coverage=WORKLOAD.stack_coverage_lifetimes(serial,raw,cpus,
        plan.get('stack_coverage_contract'),services,n(work,'admitted'))
    E.require(all(n(work,'admitted')<g<=n(snapshot,'admitted') and g in services and
        n(services[g],'operation')==1 and n(services[g],'status')==0
        for g in reports['qualification_requests']),'post calibration stack service outside final audit')
    tlb_commands=[r for r in commands if r['payload'] in (('tlbtest','tlbtest run','irqlatency stress') if current_proof else ('tlbtest','tlbtest run'))]
    inherited=[];previous=0
    if tlb_commands:
        from device_mapping_evidence import split_traces
        blocks=split_traces(debugcon,maximum_bytes=output_proof['capacity']+32*1024*1024 if output_proof else 32*1024*1024)['TLB']
        E.require(len(blocks)==len(tlb_commands),'terminal inherited TLB command count')
        modes=[line for line in lines if re.fullmatch(r'\[LAPIC\] System using X2?APIC',line)]
        E.require(len(modes)==1,'terminal inherited APIC mode')
        for command,block in zip(tlb_commands,blocks):
            segment=lines[command['begin_line']-1:command['end_line']]
            trace=T.bind_trace(segment,block,proof_policy=PROOF.CONTRACT if output_proof else None);result=T.verify_tlb(trace,topology,root,'X2APIC' in modes[0],previous,timing_contract=timing_contract)
            previous=result['generation_last'];inherited.append(result)
            for row in T.rows(trace)['SERVICE']:
                E.require(row==services.get(n(row,'generation')),'terminal inherited audit differs from final ledger')
    request_telemetry=None
    if timing_contract:
        request_telemetry=CYCLES.verify(raw,topology,root)
        timing_positions=[i for i,line in enumerate(lines) if line.startswith('[IRQ_LATENCY][REQUEST_CYCLES] ')]
        E.require(all(positions['AUDIT_BEGIN'][0]<i<positions['AUDIT_END'][0] for i in timing_positions),'terminal timing records beyond cutoff')
    else:E.require(not raw.get('REQUEST_CYCLES'),'terminal current telemetry relabeled historical')
    # Snapshot requests and reaper operations can be later than an earlier fixture
    # checkpoint. Every generation through the final admitted boundary is required.
    return dict(status='PASS',contract=CONTRACT,cpus=cpus,root=root,work_admitted=n(work,'admitted'),
        snapshot_admitted=n(snapshot,'admitted'),final_admitted=last,service=proof,proof_output=output_proof,request_telemetry=request_telemetry,inherited_workloads=inherited,stack_reports=reports,warning_reports=warnings,calibration_closure=closure,
        **({'stack_coverage':stack_coverage} if stack_coverage else {}))
