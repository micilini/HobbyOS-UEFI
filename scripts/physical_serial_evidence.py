"""Strict physical UART diagnostic reconstruction; never campaign acceptance."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import zlib
import irq_latency_evidence as V
import tlb_proof_evidence as P

ROLE='PHYSICAL_SERIAL_DIAGNOSTIC'

def auxiliary_frames(data,build_id):
    clean=[];retained=bytearray();count=0;before_end=None
    for line in data.splitlines(keepends=True):
        if line.startswith(b'[PHYSICAL_LATENCY][END] '):before_end=(count,len(retained))
        if not line.startswith(b'[SERIAL_AUX]'):
            clean.append(line);continue
        row=fields(line.decode('ascii').rstrip('\n'),'[SERIAL_AUX]')
        V.require(set(row)==set('build seq bytes data fnv1a'.split()) and
            int(row['build'],0)==int(build_id,16) and int(row['seq'])==count+1,
            'physical auxiliary identity/sequence')
        V.require(re.fullmatch('[0-9a-f]+',row['data']) and len(row['data'])%2==0,
                  'physical auxiliary encoding')
        payload=bytes.fromhex(row['data'])
        V.require(0<len(payload)<=128 and len(payload)==int(row['bytes']) and
            P.digest(payload)==int(row['fnv1a'],0),'physical auxiliary length/digest')
        retained.extend(payload);count+=1
    return b''.join(clean),dict(records=count,bytes=len(retained),sha256=hashlib.sha256(retained).hexdigest(),
        before_end=before_end),bytes(retained)

def fields(line,prefix,crc=False):
    V.require(line.startswith(prefix+' '),'physical serial marker')
    if crc:
        body,sep,checksum=line.rpartition(' crc=')
        V.require(sep and re.fullmatch('[0-9a-f]{8}',checksum) and
                  zlib.crc32(body.encode('ascii'))==int(checksum,16),'physical serial CRC')
        line=body
    pairs=[part.split('=',1) for part in line[len(prefix)+1:].split()]
    V.require(all(len(p)==2 for p in pairs),'physical serial field syntax')
    row=dict(pairs);V.require(len(row)==len(pairs),'physical serial repeated field');return row

def transport(data,build_id,version=1):
    V.require(version in (1,2,3,4,5),"physical expected schema")
    V.require(re.fullmatch('[0-9a-f]{16}',build_id) and int(build_id,16),'physical serial expected build')
    V.require(isinstance(data,bytes) and len(data)<=256*1024*1024,'physical serial log cap')
    auxiliary=None
    if version>=3:data,auxiliary,_=auxiliary_frames(data,build_id)
    else:V.require(b'[SERIAL_AUX]' not in data,'auxiliary frames require current schema')
    lines=data.decode('ascii',errors='strict').splitlines()
    def single(marker,crc=False):
        matches=[(i,line) for i,line in enumerate(lines) if line.startswith(marker+' ')]
        V.require(len(matches)==1,'physical serial missing/duplicate '+marker)
        i,line=matches[0];return i,fields(line,marker,crc)
    bi,b=single('[PHYSICAL_LATENCY][BEGIN]',True);ei,e=single('[PHYSICAL_LATENCY][END]',True)
    V.require(bi<ei and b['version']==e['version']==str(version) and
        int(b['build'],0)==int(e['build'],0)==int(build_id,16) and
        b['role']=='PHYSICAL_DIAGNOSTIC' and b['transport']=='uart-115200-8n1' and
        b['proof_output']=='3' and b['budget_ns']=='50000','physical serial role/build')
    if version>=3:
        V.require(b.get('boot_ready') in ('0','1') and auxiliary['before_end']==
            (int(e['aux_records']),int(e['aux_bytes'])),'physical auxiliary terminal totals')
    V.require(e['timing_acceptance']=='NOT_GRANTED' and
        all(e.get(k)=='0' for k in ('prepared','begin','audit','export')) and
        all(e.get(k) in ('0','1') for k in ('finish','memory','stress')),
        'physical serial incomplete command/export')
    workload_complete=e['memory']==e['stress']=='0'
    V.require((e['status']=='CAPTURE_COMPLETE' if workload_complete else
               version>=2 and e['status']=='INCOMPLETE'),'physical workload result changed')
    pp,p=single('[TLB_PROOF][PREPARED]');sp,s=single('[TLB_PROOF][SEALED]');ep,x=single('[TLB_PROOF][EXPORTED]')
    up,u=single('[SERIAL_PROOF][BEGIN]')
    expected_transport=dict(version='1' if version<3 else '2',transport='uart-115200-8n1',units='bytes',newline='LF',role='PHYSICAL_DIAGNOSTIC')
    if version>=3:expected_transport['auxiliary']='hex-fnv1a-v1'
    V.require(bi<pp<up<sp<ep<ei and u==expected_transport,
              'physical serial seal/export chronology')
    V.require(all(r['method']==(P.CONTRACT if version==1 else 'immutable-tlb-failure-proof-v1') and int(r['session'],0)==int(build_id,16) for r in (p,s,x)) and
        p['policy']==s['policy']=='deferred' and p['invocations']==s['invocations']=='1','physical serial capture identity')
    cpus=int(b['cpus']);V.require(1<=cpus<=32 and int(p['cpus'])==cpus,'physical serial topology')
    summary=None
    if version>=4:
        V.require(re.fullmatch(r'0x[0-9a-f]+',e['uart_failed_ports']) and
            int(e['uart_failed_ports'],16)<2**32,'physical UART failure mask')
        names='summary_available cells missing invalid unqualified over_budget maximum_ns maximum_cpu maximum_path'.split()
        summary={k:V.number(e,k) for k in names}
        summary['measurement']=e['measurement']
        V.require(summary['summary_available'] in (0,1) and summary['cells']<=cpus*11 and
            all(summary[k]<=cpus*11 for k in ('missing','unqualified','over_budget')) and
            summary['invalid']<=cpus+1 and summary['maximum_cpu']<cpus and summary['maximum_path']<11,
            'physical summary ranges')
        qualified=(summary['summary_available']==1 and e['finish']=='0' and
            not any(summary[k] for k in ('missing','invalid','unqualified')))
        below=qualified and not summary['over_budget']
        if version==5:
            V.require(b.get('budget_role')=='REFERENCE_ONLY' and
                b.get('policy')==e.get('policy')=='linux-comparative-v1', 'physical reference policy identity')
            V.require(e.get('reference')==('UNAVAILABLE' if not qualified else 'EXCEEDED' if summary['over_budget'] else 'BELOW'),
                'physical reference disposition')
            expected='UNAVAILABLE' if not summary['summary_available'] else 'QUALIFIED' if qualified else 'UNQUALIFIED'
            summary['reference']=e['reference']
        else:
            V.require('policy' not in b and 'budget_role' not in b and 'reference' not in e and 'policy' not in e,
                'historical schema cannot select reference policy')
            expected='UNAVAILABLE' if not summary['summary_available'] else 'BELOW_BUDGET_OBSERVED' if below else 'REJECTED'
        V.require(summary['measurement']==expected,'physical summary verdict')
    rights,bound,capacity=P.bound(cpus,1,int(p['baseline_runs']))
    if version>=2:bound+=3;capacity+=3*P.LINE_BYTES
    V.require(int(p['capacity'])==capacity and int(p['record_bound'])==bound and int(p['rights_bound'])==rights and
        int(p['line_bound'])==P.LINE_BYTES and p['owned_until']=='VM_END','physical serial capacity')
    # Preserve exact LF payload bytes. Normal UART CRLF outside this span is allowed.
    sm=re.search(rb'^\[TLB_PROOF\]\[SEALED\] [^\r\n]+\n',data,re.M)
    em=re.search(rb'^\[TLB_PROOF\]\[EXPORTED\] [^\r\n]+\n',data,re.M)
    V.require(sm and em and sm.end()<em.start(),'physical serial raw boundaries')
    raw=data[sm.end():em.start()]
    V.require(raw.endswith(b'\n') and b'\r' not in raw and raw.count(b'\n')<=bound and len(raw)<=capacity,
              'physical serial raw length/format')
    for line in raw.splitlines():
        V.require(line.startswith(b'[TLB]') and len(line)+1<=P.LINE_BYTES,'physical serial interleaved/oversize proof')
        body,sep,checksum=line.rpartition(b' crc=')
        V.require(sep and re.fullmatch(b'[0-9a-f]{8}',checksum) and zlib.crc32(body)==int(checksum,16),
                  'physical serial TLB CRC')
    V.require(all(int(r['bytes'])==len(raw) and int(r['records'])==raw.count(b'\n') and
                  int(r['fnv1a'],0)==P.digest(raw) for r in (s,x)) and
        s['service']==x['service'] and s['capture_port_bytes']=='0' and
        int(x['export_port_bytes'])==len(raw) and x['status']=='COMPLETE','physical serial missing/reordered/failed proof')
    receipt=[(i,line) for i,line in enumerate(lines) if line.startswith('[TLB_TRACE]')]
    V.require(len(receipt)==1 and pp<receipt[0][0]<up,'physical serial causal capture receipt')
    r=fields(receipt[0][1],'[TLB_TRACE][CAPTURED]')
    V.require(r['status'] in ('PASS','FAIL') and (version>=2 or r['status']=='PASS'),'physical fixture result')
    V.require(r==dict(invocation='1',channel='ram',export='uart',records=s['records'],bytes=s['bytes'],fnv1a=s['fnv1a'],status=r['status']),
        'physical serial receipt units/channel/identity')
    outcome=None
    if version>=2:
        outcomes=[fields(l.decode(),'[TLB][OUTCOME]',True) for l in raw.splitlines() if l.startswith(b'[TLB][OUTCOME] ')]
        ends=[fields(l.decode(),'[TLB][END]',True) for l in raw.splitlines() if l.startswith(b'[TLB][END] ')]
        V.require(len(outcomes)==len(ends)==1,'physical fixture outcome missing/duplicate')
        outcome=outcomes[0]
        V.require(set(outcome)==set('invocation status stage writers_retired workers_created quarantined'.split()) and
            outcome['invocation']==ends[0]['invocation']=='1' and
            outcome['status']==ends[0]['status']==r['status'] and outcome['writers_retired']=='1' and
            0<=int(outcome['workers_created'])<=(2 if cpus>1 else 0),'physical failure writer lifetime')
        passed=outcome['status']=='PASS'
        V.require((outcome['stage']=='complete' and outcome['quarantined']=='0' and ends[0]['retained']=='0') if passed else
                  (e['stress']=='1' and outcome['stage']!='complete' and outcome['quarantined']=='1' and ends[0]['retained']=='1'),
                  'physical failure hidden or quarantine lost')
        failures=[fields(l.decode(),'[TLB][CYCLE_FAILURE]',True) for l in raw.splitlines() if l.startswith(b'[TLB][CYCLE_FAILURE] ')]
        V.require(len(failures)<=2 and len({r['window'] for r in failures})==len(failures) and (not failures or not passed),
                  'physical duplicated/hidden cycle failure')
        for row in failures:
            V.require(set(row)==set('invocation window round stage phase generation status targets ack missing failed'.split()) and
                row['invocation']=='1' and int(row['window']) in (0,1) and 0<=int(row['phase'])<=4 and
                0<=int(row['round'])<=320 and re.fullmatch('[a-z_]+',row['stage']), 'physical cycle failure identity')
    return dict(version=version,workload_complete=workload_complete,fixture_outcome=outcome,
        cpus=cpus,build_id=build_id,bytes=len(raw),records=raw.count(b'\n'),
        fnv1a=s['fnv1a'],sha256=hashlib.sha256(raw).hexdigest(),capacity=capacity,
        service=int(s['service']),finish_status=int(e['finish']),losses=0,auxiliary=auxiliary,
        **({'summary':summary,'uart_failed_ports':int(e['uart_failed_ports'],16)} if version>=4 else {})),raw,lines

def verify_summary(proof,raw,measured):
    if proof['version']<4:return
    s=proof['summary'];cpus=proof['cpus']
    rows=[r for r in raw['MAXIMUM'] if V.number(r,'path')<11]
    maximum=max(rows,key=lambda r:(V.number(r,'maximum_ns'),-V.number(r,'cpu'),-V.number(r,'path')))
    expected=dict(summary_available=1,cells=cpus*11,
        missing=sum(not V.number(r,'count') or V.number(r,'invalid')!=0 or not V.number(r,'maximum_cycles') or
                    not V.number(r,'ns_available') for r in rows),
        invalid=len(measured['invalid_cpu_states'])+int(bool(measured['invalid_boundaries'])),
        unqualified=sum(not V.number(r,'measurement_qualified') for r in rows),
        over_budget=sum(V.number(r,'ns_available') and V.number(r,'maximum_ns')>=50000 for r in rows),
        maximum_ns=V.number(maximum,'maximum_ns'),maximum_cpu=V.number(maximum,'cpu'),maximum_path=V.number(maximum,'path'))
    V.require(all(s[k]==v for k,v in expected.items()),'physical summary differs from full raw maxima')

def analyze(data,build_id,version=1):
    proof,payload,lines=transport(data,build_id,version)
    serial='\n'.join(lines)+'\n';raw=V.records(serial);cpus=proof['cpus']
    V.require(len(raw['BEGIN'])==1 and raw['BEGIN'][0]['proof_output']=='3' and
              int(raw['BEGIN'][0]['cpus'])==cpus,'physical serial current observer role')
    measured=V.inspect(serial,primitive='intel-local-tsc-irq-envelope-v1')
    verify_summary(proof,raw,measured)
    import irq_latency_workload as W
    import irq_latency_terminal as T
    import irq_latency_stack_reports as R
    import irq_latency_warning_reports as Q
    import request_local_cycles as C
    import clocksource_evidence as K
    closure=T.calibration_closure(raw,cpus)
    topology={};roots=[]
    for line in lines:
        m=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if m:
            slot=int(m[1]);V.require(slot not in topology,'physical repeated CPU');topology[slot]=(int(m[2]),m[3])
        m=re.match(r'\[LOW_ROOT\] ACTIVE root=([0-9a-f]+) ',line)
        if m:roots.append(int(m[1],16))
    V.require(set(topology)==set(range(cpus)) and len(roots)==1,'physical boot topology/root')
    root=roots[0];decoder=T.tlb()
    V.require(os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks',
              'physical source role must be selected in a dedicated process')
    cutoff=['WORK_CUTOFF','SNAPSHOT_CUTOFF','AUDIT_BEGIN','AUDIT_END'];positions=[];values=[]
    for kind in cutoff:
        V.require(len(raw[kind])==1,'physical cutoff count');values.append(raw[kind][0])
        positions.append(next(i for i,l in enumerate(lines) if l.startswith('[IRQ_LATENCY]['+kind+'] ')))
    V.require(positions==sorted(positions) and len(set(positions))==4,'physical cutoff order')
    work,snapshot,begin,end=values;last=proof['service']
    V.require(int(work['admitted'])<=int(snapshot['admitted'])<=last and
        all(int(r[k])==last for r in (begin,end) for k in ('admitted','completed')) and
        int(end['emitted'])==last and end['status']=='COMPLETE','physical service cutoff')
    service=decoder.verify_service_sequence(raw['SERVICE'],topology,root,1,last)
    V.require(all(positions[2]<i<positions[3] for i,l in enumerate(lines) if l.startswith(('[IRQ_LATENCY][SERVICE]','[IRQ_LATENCY][REQUEST_CYCLES]'))),
        'physical service after terminal audit')
    V.require(positions[3]<next(i for i,l in enumerate(lines) if l.startswith('[SERIAL_PROOF][BEGIN]')),
        'physical proof export before audit')
    modes=[l for l in lines if re.fullmatch(r'\[LAPIC\] System using X2?APIC',l)]
    V.require(len(modes)==1,'physical APIC mode')
    fixture_lines=[l for l in payload.decode().splitlines() if version==1 or not l.startswith(('[TLB][OUTCOME] ','[TLB][CYCLE_FAILURE] '))]
    if not proof['workload_complete']:
        components={}
        for name,check in {
            'workload':lambda:W.verify(raw,cpus,stack_contract=W.STACK_COVERAGE),
            'fixture':lambda:decoder.verify_tlb(fixture_lines,topology,root,'X2APIC' in modes[0],timing_contract=C.CONTRACT),
        }.items():
            try:components[name]=dict(status='RECONSTRUCTED',result=check())
            except (ValueError,KeyError,IndexError) as error:components[name]=dict(status='NOT_ACCEPTED',reason=str(error))
        return dict(schema='physical-serial-diagnostic-v'+str(version),role=ROLE,proof=proof,measurement=measured,
            status='FAILED_WORKLOAD_PROOF_RECONSTRUCTED',components=components,service=service,
            request_cycles=C.verify(raw,topology,root),calibration_closure=closure,
            stack_reports=R.verify(serial),warning_reports=Q.verify(serial),
            observed_all_paths_qualified_below_budget=False,
            over_budget_cells=sum(not r['below_budget'] for r in measured['maxima']),
            implementation_acceptance=False,complete_portable_acceptance=False,
            authority='Immutable failed fixture bytes and terminal evidence; functional acceptance remains false.')
    workload=W.verify(raw,cpus,stack_contract=W.STACK_COVERAGE)
    fixture=decoder.verify_tlb(fixture_lines,topology,root,'X2APIC' in modes[0],timing_contract=C.CONTRACT)
    services={int(r['generation']):r for r in raw['SERVICE']}
    for row in decoder.rows(fixture_lines)['SERVICE']:
        V.require(row==services.get(int(row['generation'])),'physical fixture differs from terminal ledger')
    lifecycle=W.stack_coverage_lifetimes(serial,raw,cpus,W.STACK_COVERAGE,services,int(work['admitted']))
    selected=K.boot(serial,cpus);clock_terminal=K.terminal(raw,selected)
    failures=[r for r in measured['maxima'] if not r['below_budget']]
    all_below=(not failures and not measured['missing_runtime_paths'] and not measured['observer_post_failures'] and
               not measured['invalid_cpu_states'] and not measured['invalid_boundaries'])
    return dict(schema='physical-serial-diagnostic-v'+str(version),role=ROLE,proof=proof,measurement=measured,
        observed_all_paths_qualified_below_budget=all_below,over_budget_cells=len(failures),
        performance_policy='linux-comparative-v1' if version==5 else 'historical-strict-50000ns',
        comparative_acceptance='NOT_ESTABLISHED',
        workload=workload,service=service,fixture=fixture,stack_lifetimes=lifecycle,
        request_cycles=C.verify(raw,topology,root),stack_reports=R.verify(serial),warning_reports=Q.verify(serial),
        calibration_closure=closure,clocksource=selected,clocksource_terminal=clock_terminal,
        implementation_acceptance=False,complete_portable_acceptance=False,
        authority='One source-bound diagnostic capture. Physical identity supplied separately; no virtual timing-series credit.')
