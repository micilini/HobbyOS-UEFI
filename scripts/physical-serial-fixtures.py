#!/usr/bin/env python3
"""Synthetic physical UART transport mutations; never claimed as guest evidence."""
import json,zlib
import physical_serial_evidence as E
import tlb_proof_evidence as P

def line(text):return (text+' crc=%08x\n'%zlib.crc32(text.encode())).encode()
def capture(version=1,failed=False,retired=1,quarantine=None):
    ident='123456789abcdef0';prefix='0x'+ident;rights,records,capacity=P.bound(8,1,23)
    method=P.CONTRACT if version==1 else 'immutable-tlb-failure-proof-v1'
    outcome='FAIL' if failed else 'PASS';stress=int(failed)
    if version>=2:records+=3;capacity+=3*P.LINE_BYTES
    payload=line('[TLB][BEGIN] schema=7 invocation=1')
    if version>=2:
        payload+=line(f'[TLB][OUTCOME] invocation=1 status={outcome} stage={"final_scan" if failed else "complete"} writers_retired={retired} workers_created=2 quarantined={int(failed) if quarantine is None else quarantine}')
    payload+=line(f'[TLB][END] invocation=1 status={outcome}'+(' retained='+str(int(failed)) if version>=2 else ''))
    count=payload.count(b'\n')
    digest=hex(P.digest(payload));size=len(payload)
    return (line(f'[PHYSICAL_LATENCY][BEGIN] version={version} build={prefix} cpus=8 role=PHYSICAL_DIAGNOSTIC transport=uart-115200-8n1 proof_output=3 budget_ns=50000'+(' boot_ready=1' if version>=3 else ''))+
        f'[TLB_PROOF][PREPARED] method={method} session={prefix} policy=deferred cpus=8 invocations=1 capacity={capacity} record_bound={records} line_bound=766 baseline_runs=23 rights_bound={rights} owned_until=VM_END\r\n'.encode()+
        f'[TLB_TRACE][CAPTURED] invocation=1 channel=ram export=uart records={count} bytes={size} fnv1a={digest} status={outcome}\r\n'.encode()+
        (f'[SERIAL_PROOF][BEGIN] version={2 if version>=3 else 1} transport=uart-115200-8n1 units=bytes newline=LF role=PHYSICAL_DIAGNOSTIC'+(' auxiliary=hex-fnv1a-v1' if version>=3 else '')+'\n').encode()+
        f'[TLB_PROOF][SEALED] method={method} session={prefix} policy=deferred invocations=1 bytes={size} records={count} fnv1a={digest} service=2544 capture_port_bytes=0\n'.encode()+payload+
        f'[TLB_PROOF][EXPORTED] method={method} session={prefix} bytes={size} records={count} fnv1a={digest} service=2544 export_port_bytes={size} status=COMPLETE\n'.encode()+
        line(f'[PHYSICAL_LATENCY][END] version={version} build={prefix} prepared=0 begin=0 memory=0 stress={stress} finish=1 audit=0 export=0 status={"INCOMPLETE" if failed else "CAPTURE_COMPLETE"}'+(' aux_records=0 aux_bytes=0' if version>=3 else '')+
        (' uart_failed_ports=0x0 summary_available=1 cells=88 missing=0 invalid=0 unqualified=0 over_budget=1 maximum_ns=50000 maximum_cpu=0 maximum_path=0 measurement=REJECTED' if version==4 else '')+' timing_acceptance=NOT_GRANTED'))
def main():
    data=capture();ident='123456789abcdef0';p,_,_=E.transport(data,ident);assert p['finish_status']==1 and p['losses']==0
    mutations={
      'truncated':data[:-10],
      'duplicate':data+data,
      'wrong_identity':data.replace(b'123456789abcdef0',b'223456789abcdef0'),
      'wrong_channel':data.replace(b'channel=ram export=uart',b'channel=debugcon port=0x402'),
      'truncated_payload':data.replace(line('[TLB][END] invocation=1 status=PASS'),b''),
      'reordered':data.replace(line('[TLB][BEGIN] schema=7 invocation=1')+line('[TLB][END] invocation=1 status=PASS'),line('[TLB][END] invocation=1 status=PASS')+line('[TLB][BEGIN] schema=7 invocation=1')),
      'injected_writer':data.replace(b'[TLB][END]',b'interrupt\n[TLB][END]'),
      'io_in_capture':data.replace(b'capture_port_bytes=0',b'capture_port_bytes=1'),
      'wrong_export_count':data.replace(b'export_port_bytes=',b'export_port_bytes=1'),
      'wrong_capacity':data.replace(b'line_bound=766',b'line_bound=765'),
      'carriage_payload':data.replace(line('[TLB][END] invocation=1 status=PASS'),line('[TLB][END] invocation=1 status=PASS').replace(b'\n',b'\r\n')),
      'failed_end':data.replace(b'export=0 status=CAPTURE_COMPLETE',b'export=1 status=INCOMPLETE'),
      'no_footer':data[:data.index(b'[TLB_PROOF][EXPORTED]')],
      'crc_flip':data.replace(b'schema=7 invocation=1',b'schema=6 invocation=1'),
    }
    for name,raw in mutations.items():
        try:E.transport(raw,ident)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name+' accepted')
    # Passing transport alone does not provide functional/timing evidence.
    try:E.analyze(data,ident)
    except ValueError:pass
    else:raise AssertionError('summary-only capture accepted')
    for failed in (False,True):
        result,_,_=E.transport(capture(2,failed),ident,2)
        assert result['workload_complete']==(not failed) and result['fixture_outcome']['status']==('FAIL' if failed else 'PASS')
    more={
        'unretired_failure':(capture(2,True,retired=0),2),
        'failure_resources_reused':(capture(2,True,quarantine=0),2),
        'new_as_old':(capture(2),1),
        'old_as_new':(capture(),2),
        'quarantined_pass':(capture(2,quarantine=1),2),
    }
    for name,(raw,version) in more.items():
        try:E.transport(raw,ident,version)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name+' accepted')
    for failed in (False,True):E.transport(capture(3,failed),ident,3)
    current=capture(3);payload=b'partial ordinary message\r\n[USB] preserved\x00'
    frame=f'[SERIAL_AUX] build=0x{ident} seq=1 bytes={len(payload)} data={payload.hex()} fnv1a={hex(P.digest(payload))}\n'.encode()
    framed=current.replace(b'[TLB][END]',frame+b'[TLB][END]')
    footer=next(x for x in framed.splitlines(keepends=True) if x.startswith(b'[PHYSICAL_LATENCY][END]'))
    body=footer.rsplit(b' crc=',1)[0].decode().replace('aux_records=0 aux_bytes=0',f'aux_records=1 aux_bytes={len(payload)}')
    framed=framed.replace(footer,line(body));result,proof,_=E.transport(framed,ident,3)
    assert result['auxiliary']['bytes']==len(payload) and E.auxiliary_frames(framed,ident)[2]==payload
    assert proof==E.transport(current,ident,3)[1]
    extra={
      'deleted_auxiliary':framed.replace(frame,b''),
      'duplicated_auxiliary':framed.replace(frame,frame+frame),
      'wrong_auxiliary_sequence':framed.replace(b' seq=1 ',b' seq=2 '),
      'wrong_auxiliary_bytes':framed.replace(f'bytes={len(payload)} data='.encode(),b'bytes=1 data='),
      'corrupt_auxiliary':framed.replace(payload.hex().encode(),('00'+payload.hex()[2:]).encode()),
      'foreign_auxiliary':framed.replace(frame,frame.replace(ident.encode(),b'223456789abcdef0')),
      'partial_auxiliary':framed.replace(frame,frame[:-5]),
      'unframed_noise':current.replace(b'[TLB][END]',b'noise\n[TLB][END]'),
    }
    for name,raw in extra.items():
        try:E.transport(raw,ident,3)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name+' accepted')
    for raw,version in [(current,2),(capture(2),3),(framed,1)]:
        try:E.transport(raw,ident,version)
        except (ValueError,KeyError):pass
        else:raise AssertionError('cross-schema auxiliary accepted')
    latest=capture(4);assert E.transport(latest,ident,4)[0]['summary']['measurement']=='REJECTED'
    def change_footer(data,old,new):
        footer=next(l for l in data.splitlines(keepends=True) if l.startswith(b'[PHYSICAL_LATENCY][END]'))
        body=footer.rsplit(b' crc=',1)[0].decode();assert old in body
        return data.replace(footer,line(body.replace(old,new)))
    latest_bad=[change_footer(latest,'measurement=REJECTED','measurement=BELOW_BUDGET_OBSERVED'),
        change_footer(latest,'summary_available=1','summary_available=0'),
        change_footer(latest,'cells=88','cells=89'),
        change_footer(latest,'over_budget=1','over_budget=89'),
        change_footer(latest,' maximum_ns=50000',''),
        change_footer(latest,'maximum_cpu=0','maximum_cpu=8')]
    for bad in latest_bad:
        try:E.transport(bad,ident,4)
        except (ValueError,KeyError):pass
        else:raise AssertionError('invalid summary accepted')
    for data,version in [(latest,3),(current,4)]:
        try:E.transport(data,ident,version)
        except (ValueError,KeyError):pass
        else:raise AssertionError('summary schema bypass')
    measured=dict(invalid_cpu_states=[],invalid_boundaries=0)
    maxima=[dict(cpu=str(cpu),path=str(path),count='1',invalid='0',maximum_cycles='49999',
                 ns_available='1',maximum_ns='49999',measurement_qualified='1') for cpu in range(8) for path in range(11)]
    maxima[0].update(maximum_ns='50000',maximum_cycles='50000')
    proof=E.transport(latest,ident,4)[0]
    E.verify_summary(proof,dict(MAXIMUM=list(reversed(maxima))),measured)
    for field in ('cells','missing','invalid','unqualified','over_budget','maximum_ns','maximum_cpu','maximum_path'):
        original=proof['summary'][field];proof['summary'][field]=original+1
        try:E.verify_summary(proof,dict(MAXIMUM=maxima),measured)
        except ValueError:pass
        else:raise AssertionError('summary differs from raw '+field)
        proof['summary'][field]=original
    # Explicit successor: the reference can be exceeded with valid measurement;
    # failures of coverage, calibration or transport remain failures.
    reference=capture(4).replace(b'version=4',b'version=5')
    def reframe(data):
        return b''.join(line(l.rsplit(b' crc=',1)[0].decode()) if l.startswith(b'[PHYSICAL_LATENCY]') else l+b'\n'
                        for l in data.splitlines())
    reference=reference.replace(b'boot_ready=1',b'boot_ready=1 budget_role=REFERENCE_ONLY policy=linux-comparative-v1')
    reference=reference.replace(b'finish=1',b'finish=0').replace(b'measurement=REJECTED',
        b'measurement=QUALIFIED policy=linux-comparative-v1 reference=EXCEEDED')
    reference=reframe(reference)
    assert E.transport(reference,ident,5)[0]['summary']['reference']=='EXCEEDED'
    unqualified=change_footer(reference,'measurement=QUALIFIED','measurement=UNQUALIFIED')
    unqualified=change_footer(unqualified,'unqualified=0','unqualified=11')
    unqualified=change_footer(unqualified,'reference=EXCEEDED','reference=UNAVAILABLE')
    assert E.transport(unqualified,ident,5)[0]['summary']['measurement']=='UNQUALIFIED'
    new_bad=[(reference,4),(latest,5),
        (change_footer(reference,'reference=EXCEEDED','reference=BELOW'),5),
        (change_footer(reference,'policy=linux-comparative-v1','policy=unknown'),5),
        (change_footer(reference,'unqualified=0','unqualified=11'),5),
        (change_footer(reference,'missing=0','missing=1'),5),
        (change_footer(reference,'invalid=0','invalid=1'),5),
        (change_footer(reference,'finish=0','finish=1'),5),
        (change_footer(reference,'summary_available=1','summary_available=0'),5),
        (change_footer(unqualified,'reference=UNAVAILABLE','reference=BELOW'),5)]
    for data,version in new_bad:
        try:E.transport(data,ident,version)
        except (ValueError,KeyError):pass
        else:raise AssertionError('reference policy bypass')
    print(json.dumps(dict(status='PASS',synthetic_controls=10,expected_rejections=len(mutations)+len(more)+len(extra)+20+len(new_bad))))
if __name__=='__main__':main()
