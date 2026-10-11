#!/usr/bin/env python3
"""Actual diagnostic plan/controller/parser tests; synthetic host events only."""
import copy
import importlib.util
import json
import zlib
from unittest.mock import patch
import memory_evidence as E
import irq_latency_qemu_trace as T
import irq_latency_authority as A


def module(name):
    spec=importlib.util.spec_from_file_location(name,E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def fixture():
    lines=[];clocks=[];stamp=1000000
    def emit(cpu,event,fields):
        nonlocal stamp
        lines.append(f'{100+cpu}@0.{stamp//1000:06d}:{event} {fields}\n');stamp+=5000
    for cpu in range(2):
        for site in range(3):
            ticks=(1<<32)+1000+cpu*10+site
            clocks.append(dict(cpu=cpu,site=site,ticks=ticks,mode=1,retries=0,
                authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',minimum_ns=60000))
            for address,value in ((0xfed000f4,1),(0xfed000f0,ticks&0xffffffff),(0xfed000f4,1)):
                emit(cpu,'memory_region_ops_read',f"cpu {cpu} mr 0x1234 addr 0x{address:x} value 0x{value:x} size 4 name 'hpet'")
    return lines,clocks,{0:100,1:101},0,10**9


def run(output):
    output.mkdir(parents=True,exist_ok=False);cases=[]
    data=fixture();result=T.correlate(*data)
    assert result['status']=='MATCHED' and len(result['rows'])==6 and not result['timing_authority']
    assert all(r['read_gaps_ns']==[5000]*2 and r['first_to_last_read_ns']==10000 for r in result['rows'])
    E.write(output/'synthetic.json',dict(lines=data[0],clocks=data[1],tids=data[2],result=result))
    cases.append(dict(name='exact-value-independent-CPU-join',status='PASS'))
    def reject(name,mutate):
        value=copy.deepcopy(data);mutate(value)
        try:T.correlate(*value)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    reject('wrong-TID',lambda d:d[0].__setitem__(0,d[0][0].replace('100@','101@')))
    reject('wrong-CPU',lambda d:d[0].__setitem__(0,d[0][0].replace('cpu 0','cpu 1')))
    reject('wall-clock-reversed',lambda d:d[0].__setitem__(1,d[0][1].replace('0.001005','0.000995')))
    reject('no-real-timestamp',lambda d:d[0].__setitem__(0,d[0][0].split(':',1)[1]))
    reject('partial-final-line',lambda d:d[0].__setitem__(-1,d[0][-1].rstrip()))
    reject('wrong-width',lambda d:d[0].__setitem__(0,d[0][0].replace('size 4','size 8')))
    reject('mixed-HPET-region',lambda d:d[0].__setitem__(1,d[0][1].replace('0x1234','0x4567')))
    reject('unsupported-retry',lambda d:d[1][0].update(retries=1))
    reject('reused-clock-ID',lambda d:d[1].append(copy.deepcopy(d[1][0])))
    reject('changed-thread-map',lambda d:d[2].update({1:100}))
    for name,edit in (('missing-middle-read',lambda d:d[0].pop(1)),
                      ('wrong-value',lambda d:d[0].__setitem__(1,d[0][1].replace('0x3e8','0x3e7'))),
                      ('missing-read-tail',lambda d:d[0].pop())):
        value=copy.deepcopy(data);edit(value);partial=T.correlate(*value)
        assert partial['status']=='PARTIAL' and partial['missing'] and not partial['timing_authority']
        cases.append(dict(name=name,status='PARTIAL_NOT_COMPLETE'))
    reject('undeclared-ioctl-trace',lambda d:d[0].insert(0,'100@0.000900:kvm_vcpu_ioctl cpu_index 0, type 0xae80, arg (nil)\n'))
    value=copy.deepcopy(data)
    value[0][1]=value[0][1].replace('value 0x3e8','value 0x2000003e8')
    assert T.correlate(*value)['rows'][0]['ticks']==data[1][0]['ticks']
    cases.append(dict(name='actual-MMIO-width-masks-callback-return',status='PASS'))
    run_lines=[]
    for line in data[0]:
        parsed=T.LINE.fullmatch(line.rstrip());tid,seconds,micros,_,fields=parsed.groups()
        cpu=int(T.READ.fullmatch(fields)[1]);stamp=int(seconds)*1000000+int(micros)
        run_lines.extend([f'{tid}@0.{stamp-2:06d}:kvm_vcpu_ioctl cpu_index {cpu}, type 0xae80, arg (nil)\n',
                          f'{tid}@0.{stamp-1:06d}:kvm_run_exit cpu_index {cpu}, reason 6\n',line])
    run_result=T.correlate(run_lines,*data[1:],run_events=True)
    assert run_result['status']=='MATCHED' and all(
        p==dict(status='MATCHED',after_read_to_run_ns=3000,run_and_postrun_ns=1000,exit_to_read_ns=1000)
        for row in run_result['rows'] for p in row['callback_gap_regions'])
    cases.append(dict(name='actual-parser-separates-userspace-from-run-and-postrun',status='PASS'))
    for name,edit in [('missing-exit',lambda rows:rows.pop(4)),
                     ('wrong-exit-reason',lambda rows:rows.__setitem__(4,rows[4].replace('reason 6','reason 2'))),
                     ('missing-run-entry',lambda rows:rows.pop(3))]:
        altered=list(run_lines);edit(altered)
        parsed=T.correlate(altered,*data[1:],run_events=True)
        assert parsed['rows'][0]['callback_gap_regions'][0]['status']=='UNAVAILABLE'
        assert parsed['rows'][0]['callback_gap_regions'][0]['exit_to_read_ns'] is None
        cases.append(dict(name=name,status='UNAVAILABLE_NOT_ZERO'))
    X=module('irq-latency-pairing-experiment');candidate=output/'candidate';candidate.mkdir()
    info=dict(profile='irq-entry-diagnostic',source_sha256=E.identity(E.manifest()),
        artifacts={'kernel.elf':{'sha256':'1'*64},'hobbyos.img':{'sha256':'2'*64}})
    command=dict(argv=['make','kernel-check','image','SELFTEST=1','IRQ_LATENCY=1','IRQ_ENTRY_DIAGNOSTIC=1','IRQ_LATENCY_DETAILS=0'],
        exit_status=0,build_input_sha256=info['source_sha256'])
    receipt=dict(commands=[dict(receipt='build.json',receipt_sha256=E.identity(command))],status='PASS',
        artifacts=info['artifacts'],inputs_before=info['source_sha256'],build_input_sha256=info['source_sha256'])
    E.write(candidate/'candidate.json',dict(info,build_receipt_sha256=E.identity(receipt)))
    E.write(candidate/'build-receipt.json',receipt)
    E.write(candidate/'build.json',command)
    original=X.module
    def load(name):
        value=original(name)
        if name=='memory-observation-gate':value.verify_local_candidate=lambda p:None
        return value
    with patch.object(X,'module',side_effect=load),patch.object(X.subprocess,'check_output',return_value=b''):
        plan=X.prepare(output/'plan',candidate,entry_diagnostic=True)
        current=X.prepare(output/'counter-plan',candidate,entry_diagnostic=True,qemu_counter_trace=True)
        try:X.prepare(output/'retired-trace-plan',candidate,entry_diagnostic=True,qemu_exit_trace=True)
        except ValueError as error:assert 'retired HPET trace join' in str(error)
        else:raise AssertionError('current cycle candidate selected retired trace')
    assert T.contract(plan) is None and X.PROOF.contract(plan)
    assert plan['commands'][1:]==['irqlatency begin','mem','irqlatency stress','irqlatency finish','irqlatency audit','irqlatency export']
    cases.append(dict(name='actual-current-plan-preserves-workload-without-retired-trace',status='PASS'))
    cases.append(dict(name='actual-current-plan-rejects-retired-HPET-trace',status='REJECTED'))
    assert T.contract(current)==T.declaration(T.COUNTER_CONTRACT)
    assert X.PROOF.contract(current) and current['commands'][1:]==plan['commands'][1:]
    assert current['proof_output']['policy']==plan['proof_output']['policy']=='deferred'
    assert current['tick_body_contract']=='lapic-handler-body-v4'
    cases.append(dict(name='actual-current-counter-plan-no-workload-change',status='PASS'))
    for name,edit in [('old-method-on-current',dict(qemu_exit_trace=T.declaration())),
                      ('old-clock-on-current',dict(writeback_clock_contract='continuous-mask-writeback-clock-v1')),
                      ('missing-counter-schema',dict(tick_body_contract=None)),
                      ('wrong-counter-schema',dict(tick_body_contract='lapic-handler-body-v2')),
                      ('counter-timing-promotion',dict(role='CURRENT_TIMING_QUALIFICATION')),
                      ('counter-backend-change',dict(ram_backend={'prealloc':'on'}))]:
        altered=copy.deepcopy(current);altered.update(edit)
        try:T.contract(altered)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    tick=dict(status='PASS',contract='lapic-handler-body-v4',rows=[])
    for cpu in range(8):
        tick['rows'].append(dict(cpu=cpu,count=1,raw_cycles=125000,authority='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY',epoch_qualified=True,
            complete=True,invalid=0,frequency_upper=2500000000,
            witness=dict(outer_sequence=cpu+1,sample_begin=100,counter_end=125100,
                sample=dict(cpu=str(cpu),valid='1',outer_sequence=str(cpu+1),ticks=str(1000+cpu),mode='1',retries='0',source='1',source_epoch='1'))))
    clocks=T.counter_clocks(tick)
    assert len(clocks)==8 and all(r['minimum_ns']==50000 and r['raw_cycles']==125000 for r in clocks)
    cases.append(dict(name='actual-counter-adapter-preserves-minimum-units-and-identity',status='PASS'))
    bsp=copy.deepcopy(tick)
    for row in bsp['rows'][1:]:row.update(count=0,raw_cycles=0,witness=None,authority='UNAVAILABLE',epoch_qualified=False)
    assert len(T.counter_clocks(bsp))==1 and T.counter_clocks(bsp)[0]['cpu']==0
    assert T.correlate(run_lines,*data[1:3],data[3],data[4],run_events=True)['status']=='MATCHED'
    subset=list(copy.deepcopy(data));subset[1]=[r for r in subset[1] if r['cpu']==0]
    assert len(T.correlate(run_lines,*subset[1:],run_events=True)['rows'])==3
    cases.append(dict(name='actual-BSP-memory-work-with-complete-AP-identity',status='PASS'))
    erased=copy.deepcopy(bsp);erased['rows'][0]['count']=0
    try:T.counter_clocks(erased)
    except ValueError:cases.append(dict(name='zero-count-cannot-hide-existing-maximum',status='REJECTED'))
    else:raise AssertionError('erased maximum')
    for name,edit in [('unqualified-epoch',lambda r:r.update(epoch_qualified=False)),
                      ('missing-witness',lambda r:r.update(witness=None)),
                      ('invalid-continuity',lambda r:r.update(invalid=1)),
                      ('wrong-counter-CPU',lambda r:r['witness']['sample'].update(cpu='7')),
                      ('TSC-falsely-labeled-HPET',lambda r:r['witness']['sample'].update(source='2',source_epoch='2',mode='0')),
                      ('copied-outer-sequence',lambda r:r['witness']['sample'].update(outer_sequence='3')),
                      ('interior-upper-promotion',lambda r:r.update(authority='ACCEPTED_UPPER_BOUND'))]:
        altered=copy.deepcopy(tick);edit(altered['rows'][0])
        try:T.counter_clocks(altered)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    window=dict(trigger=None,begin={'end':{'monotonic_after_ns':10}},end={'begin':{'monotonic_before_ns':40}})
    driver=[dict(event='HMP_INPUT_BEGIN',command='irqlatency stress',sequence=3,monotonic_ns=20),
            dict(marker='[HARNESS][END] seq=3 ',monotonic_ns=30)]
    def write_driver(rows):
        (output/'driver.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
    write_driver(driver);T.verify_stress_window(output,window)
    cases.append(dict(name='actual-stress-window-complete',status='PASS'))
    for name,rows in [('missing-stress-end',driver[:1]),('duplicate-stress',driver+[driver[0]]),
                      ('late-enable',[dict(driver[0],monotonic_ns=9),driver[1]]),
                      ('early-disable',[driver[0],dict(driver[1],monotonic_ns=41)])]:
        write_driver(rows)
        try:T.verify_stress_window(output,window)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    write_driver(driver)
    # Retain the historical trace adapter's independent synthetic controls.
    plan=copy.deepcopy(plan);plan.pop('request_timing_contract')
    plan.update(qemu_exit_trace=T.declaration(),writeback_clock_contract='continuous-mask-writeback-clock-v1')
    assert T.contract(plan)==T.declaration()
    control_dir=output/'control';control_dir.mkdir()
    class FakeClient:
        states={event:False for event in T.COUNTER_EVENTS}
        def __init__(self,socket,log,deadline):
            self.log=log;self.next=0;self.socket=self;log.write_text('')
        def close(self):pass
        def command(self,name,args,deadline):
            self.next+=1;identity=str(self.next)
            if name=='query-uuid':value={'UUID':plan['vm_ids'][0]}
            elif name=='query-cpus-fast':value=[{'cpu-index':c,'thread-id':t} for c,t in data[2].items()]
            elif name=='trace-event-set-state':self.states[args['name']]=args['enable'];value={}
            elif name=='trace-event-get-state':value=[dict(name=args['name'],state='enabled' if self.states[args['name']] else 'disabled',vcpu=False)]
            else:value={}
            request=dict(id=identity,execute=name)
            if args:request['arguments']=args
            reply=dict(id=identity,**{'return':value})
            with self.log.open('a') as stream:
                stream.write(json.dumps(dict(direction='send',message=request))+'\n')
                stream.write(json.dumps(dict(direction='receive',message=reply))+'\n')
            return identity,reply
    # The real adapter sends capabilities in its constructor.
    init=FakeClient.__init__
    def initialize(self,*args):init(self,*args);self.command('qmp_capabilities',{},0)
    FakeClient.__init__=initialize
    window=dict(begin=T.control(control_dir,plan,0,'enable',True,FakeClient),
                end=T.control(control_dir,plan,0,'disable',False,FakeClient))
    T.verify_controls(control_dir,window,plan['candidate_id'],plan['vm_ids'][0],[data[2]])
    cases.append(dict(name='actual-QMP-control-raw-replay',status='PASS'))
    transcript=control_dir/'exit-trace-enable.jsonl';original=transcript.read_text()
    for name,change in [('missing-QMP-reply',lambda rows:rows.pop()),
                         ('wrong-QMP-VM',lambda rows:rows[3]['message']['return'].update(UUID='other')),
                         ('wrong-QMP-thread',lambda rows:rows[5]['message']['return'][0].update({'thread-id':999})),
                         ('duplicate-QMP-reply',lambda rows:rows.append(copy.deepcopy(rows[-1]))),
                         ('changed-event-state',lambda rows:rows[-1]['message']['return'][0].update(state='disabled'))]:
        rows=[json.loads(line) for line in original.splitlines()];change(rows)
        transcript.write_text(''.join(json.dumps(r)+'\n' for r in rows))
        try:T.verify_controls(control_dir,window,plan['candidate_id'],plan['vm_ids'][0],[data[2]])
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    transcript.write_text(original)
    plan_before=plan;plan=current;current_control=output/'counter-control';current_control.mkdir()
    current_window=dict(begin=T.control(current_control,plan,0,'enable',True,FakeClient),
                        end=T.control(current_control,plan,0,'disable',False,FakeClient))
    T.verify_controls(current_control,current_window,plan['candidate_id'],plan['vm_ids'][0],[data[2]])
    cases.append(dict(name='actual-current-three-event-QMP-control',status='PASS'))
    plan=plan_before
    prefix_dir=output/'bounded-prefix';prefix_dir.mkdir();saved_plan=plan;plan=current
    data[2].update({cpu:100+cpu for cpu in range(2,8)})
    def stamp(t):return dict(monotonic_before_ns=t,monotonic_after_ns=t+1,realtime_ns=t)
    with patch.object(T,'anchor',side_effect=[stamp(t) for t in (10,20,50000010,50000020,100000010,100000020)]):
        enabled=T.control(prefix_dir,plan,0,'enable',True,FakeClient)
        stopped=T.control(prefix_dir,plan,0,'limit-disable',False,FakeClient)
        disabled=T.control(prefix_dir,plan,0,'disable',False,FakeClient)
    point=dict(stamp(50000000),bytes=T.MAX_BYTES,free_bytes=T.RESERVE+1)
    prefix_window=dict(method=T.COUNTER_CONTRACT,candidate_id=plan['candidate_id'],vm_id=plan['vm_ids'][0],
        status='UNAVAILABLE',error=None,trigger=None,armed=stamp(0),begin=enabled,end=disabled,
        samples=[stamp(1000000),point],limit=dict(reason='DECLARED_TRACE_LIMIT',observation=point))
    E.write(prefix_dir/'exit-trace-window.json',prefix_window)
    (prefix_dir/'driver.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in (
        dict(event='HMP_INPUT_BEGIN',command='irqlatency stress',sequence=3,monotonic_ns=30),
        dict(marker='[HARNESS][END] seq=3 ',monotonic_ns=100000000))))
    (prefix_dir/'qemu-serial.log').write_text('synthetic topology supplied by the declared fixture interface\n')
    (prefix_dir/'qemu-trace.log').write_text(''.join(run_lines))
    E.write(prefix_dir/'seal.json',dict(files=[dict(path='qemu-trace.log',bytes=(prefix_dir/'qemu-trace.log').stat().st_size,
        sha256=E.sha(prefix_dir/'qemu-trace.log'))]))
    prefix_tick=copy.deepcopy(bsp);prefix_tick['rows'][0]['witness']['sample']['ticks']=str(data[1][0]['ticks'])
    cpus=[dict(**{'cpu-index':cpu,'thread-id':tid},target='x86_64',props={'core-id':cpu,'socket-id':0,'thread-id':0}) for cpu,tid in data[2].items()]
    # The QMP simulator's identity result is also the independent thread fixture.
    # Runtime QMP replies keep the simulator's exact fields; topology has a
    # separate declared raw guest reader seam for this hosted-only execution.
    hosts=[dict(qmp_identity=dict(cpus=[dict(c) for c in cpus]))]
    for label,row in [('enable',enabled),('limit-disable',stopped),('disable',disabled)]:
        row['cpus']=cpus
        (prefix_dir/('exit-trace-'+label+'.json')).write_text(json.dumps(row)+'\n')
        transcript=prefix_dir/('exit-trace-'+label+'.jsonl')
        rows=[json.loads(line) for line in transcript.read_text().splitlines()]
        for r in rows:
            if r['direction']=='receive' and r['message'].get('id')=='2':r['message']['return']={'UUID':plan['vm_ids'][0]}
            if r['direction']=='receive' and r['message'].get('id')=='3':r['message']['return']=cpus
        transcript.write_text(''.join(json.dumps(r)+'\n' for r in rows))
    (prefix_dir/'exit-trace-window.json').write_text(json.dumps(prefix_window)+'\n')
    launch=dict(argv=['qemu','-machine','q35','-m','2G','-msg','timestamp=on','-d','guest_errors','-uuid',plan['vm_ids'][0]],
        elf={'sha256':plan['elf_sha256']},image={'sha256':plan['image_sha256']},fault_trace=False)
    import irq_latency_ram_backend as RAM
    with patch.object(T.A,'verify_profile_series'),patch.object(T.S,'verify'),\
         patch.object(RAM,'guest_inventory',return_value={'cpus':[{'slot':str(c),'apic':str(c)} for c in range(8)]}):
        result=T.verify_counter_prefix(plan,launch,prefix_dir,prefix_tick,hosts)
        assert (result['status']=='BOUNDED_PREFIX_ONLY' and result['matching_status']=='MATCHED' and
            not result['complete_capture'] and not result['timing_authority'] and not result['implementation_acceptance'])
        cases.append(dict(name='actual-bounded-prefix-never-promotes-full-capture',status='PASS'))
        for name,edit in [('invented-limit',lambda w:w['limit']['observation'].update(bytes=0)),
                          ('failed-collector-hidden',lambda w:w.update(error='lost collector')),
                          ('wrong-prefix-candidate',lambda w:w.update(candidate_id='other')),
                          ('wrong-prefix-VM',lambda w:w.update(vm_id='other')),
                          ('full-capture-promotion',lambda w:w.update(status='CAPTURED')),
                          ('lost-required-event',lambda w:w['begin']['states'].pop('kvm_run_exit'))]:
            altered=copy.deepcopy(prefix_window);edit(altered)
            (prefix_dir/'exit-trace-window.json').write_text(json.dumps(altered)+'\n')
            try:T.verify_counter_prefix(plan,launch,prefix_dir,prefix_tick,hosts)
            except ValueError:pass
            else:raise AssertionError(name)
            cases.append(dict(name=name,status='REJECTED'))
        (prefix_dir/'exit-trace-window.json').write_text(json.dumps(prefix_window)+'\n')
        import lzma
        raw_trace=prefix_dir/'qemu-trace.log';encoded=prefix_dir/'qemu-trace.log.xz';original=raw_trace.read_bytes()
        encoded.write_bytes(lzma.compress(original));raw_trace.unlink()
        representation=dict(raw_bytes=len(original),raw_sha256=__import__('hashlib').sha256(original).hexdigest(),
            encoded_bytes=encoded.stat().st_size,encoded_sha256=E.sha(encoded))
        E.write(prefix_dir/'trace-representation.json',representation)
        assert T.verify_counter_prefix(plan,launch,prefix_dir,prefix_tick,hosts)==result
        cases.append(dict(name='exact-compressed-prefix-reconstruction',status='PASS'))
        representation['raw_bytes']+=1
        (prefix_dir/'trace-representation.json').write_text(json.dumps(representation)+'\n')
        try:T.verify_counter_prefix(plan,launch,prefix_dir,prefix_tick,hosts)
        except ValueError:cases.append(dict(name='changed-trace-expansion-bound',status='REJECTED'))
        else:raise AssertionError('trace bytes changed')
        representation['raw_bytes']-=1
        (prefix_dir/'trace-representation.json').write_text(json.dumps(representation)+'\n')
    plan=saved_plan
    for name,change in [('timing-promotion',dict(role='CURRENT_TIMING_QUALIFICATION')),
                        ('functional-promotion',dict(role='CURRENT_FUNCTIONAL_STRESS')),
                        ('different-backend',dict(ram_backend={'prealloc':'on'})),
                        ('retries',dict(retries=1)),('missing-clock-records',dict(writeback_clock_contract=None))]:
        invalid=copy.deepcopy(plan);invalid.update(change)
        try:T.contract(invalid)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for option in ('-trace','-msg'):
        try:A.verify_profile_series([],dict(argv=[option,'anything']),'CURRENT_TIMING_QUALIFICATION',8)
        except ValueError as error:assert 'tracing' in str(error)
        else:raise AssertionError('tracing promoted')
        cases.append(dict(name='actual-authority-rejects-'+option,status='REJECTED'))
    marker='[IRQ_LATENCY][MAPPING] completed=192 required=192 backing_released=1 status=PASS'
    def crc(body):return body+' crc='+format(zlib.crc32(body.encode()),'08x')
    assert T.mapping_trigger(crc(marker))['status']=='PASS'
    cases.append(dict(name='actual-existing-marker-trigger',status='PASS'))
    for name,body in [('incomplete-mapping',marker.replace('completed=192','completed=191')),
                      ('failed-mapping',marker.replace('status=PASS','status=FAIL')),
                      ('missing-backing-retirement',marker.replace('backing_released=1','backing_released=0'))]:
        try:T.mapping_trigger(crc(body))
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    try:T.mapping_trigger(crc(marker)+'0')
    except ValueError:cases.append(dict(name='trigger-corrupt-CRC',status='REJECTED'))
    else:raise AssertionError('corrupt trigger')
    # Exercise actual watcher limit and error paths without a VM.
    for limit in ('bytes','space','time','stat-error'):
        window=T.Window(output,plan,0,None)
        window.started={'end':{'monotonic_after_ns':1}}
        window.armed={'monotonic_after_ns':1};window.serial_offset=0
        (output/'qemu-serial.log').write_text('')
        class Once:
            def wait(self,seconds):return False
        window.done=Once()
        point=dict(monotonic_before_ns=2,monotonic_after_ns=3,realtime_ns=4)
        if limit=='time':point['monotonic_after_ns']=91*10**9
        from types import SimpleNamespace
        with patch.object(T,'anchor',return_value=point),patch.object(T,'control',return_value={}) as disable,\
             patch.object(T.shutil,'disk_usage',return_value=SimpleNamespace(free=0 if limit=='space' else T.RESERVE+T.MAX_BYTES)),\
             patch.object(type(output),'stat',side_effect=OSError('controlled missing trace') if limit=='stat-error' else None,
                return_value=SimpleNamespace(st_size=T.MAX_BYTES if limit=='bytes' else 1)):
            window.watch()
        assert disable.call_count==1 and (window.error if limit=='stat-error' else window.limit)
        cases.append(dict(name='actual-watch-'+limit,status='PASS'))
    receipt=dict(status='PASS',authority='HOSTED_ORCHESTRATION_ONLY',cases=cases,vm_launched=False)
    E.write(output/'result.json',receipt);return receipt
