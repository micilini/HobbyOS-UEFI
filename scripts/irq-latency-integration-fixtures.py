#!/usr/bin/env python3
"""Actual hosted plan/profile/terminal/transport tests; no VM is launched."""
import argparse, copy, importlib.util, json, shutil
from pathlib import Path
from unittest.mock import patch
import memory_evidence as E
import irq_latency_authority as A


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def plans(output):
    X=module('irq-latency-pairing-experiment')
    info=dict(source_sha256=E.identity(E.manifest()),
        artifacts={'kernel.elf':{'sha256':'1'*64},'hobbyos.img':{'sha256':'2'*64}})
    original=X.module
    def load(name):
        value=original(name)
        if name=='memory-observation-gate':value.verify_local_candidate=lambda path:None
        return value
    cases=[]
    with patch.object(X,'module',side_effect=load), patch.object(X.subprocess,'check_output',return_value=b''):
        for role,boots,cpus,order in [('DIAGNOSTIC_NO_ACCEPTANCE',1,8,0),('CURRENT_TIMING_QUALIFICATION',3,8,0),
                               ('CURRENT_FUNCTIONAL_STRESS',1,24,0),('DIAGNOSTIC_NO_ACCEPTANCE',1,8,1)]:
            candidate=output/('candidate-'+role+'-'+str(order));candidate.mkdir()
            command=dict(argv=['make','kernel-check','image','SELFTEST=1','IRQ_LATENCY=1',
                'IRQ_LOCAL_TSC_ORDER='+str(order)],exit_status=0,build_input_sha256=info['source_sha256'])
            receipt=dict(commands=[dict(receipt='build.json',receipt_sha256=E.identity(command))],
                status='PASS',artifacts=info['artifacts'],inputs_before=info['source_sha256'],build_input_sha256=info['source_sha256'])
            current=dict(info,build_receipt_sha256=E.identity(receipt))
            E.write(candidate/'candidate.json',current)
            E.write(candidate/'build-receipt.json',receipt)
            E.write(candidate/'build.json',command)
            directory=output/(role+'-order-'+str(order))
            plan=X.prepare(directory,candidate,role,boots)
            assert plan['commands'][-3:]==['irqlatency finish','irqlatency audit','irqlatency export'],'service tail absent from actual plan'
            assert plan['commands'].index('irqlatency stress')<plan['commands'].index('irqlatency finish')
            assert plan['profile']['smp']==cpus and plan['terminal_contract']=='service-tail-after-recorder-v4'
            assert plan['transport_contract']=='strict-keyboard-release-v1'
            assert plan['stack_coverage_contract']==X.W.STACK_COVERAGE
            assert plan['observer_update_contract']==X.OBSERVER_UPDATES
            assert plan['mmio_identity_contract']==X.MMIO_IDENTITY
            assert plan['timestamp_primitive']==X.PRIMITIVES[order] and X.bound_plan(directory)==plan
            cases.append(dict(name='actual-plan-'+role+'-order-'+str(order),status='PASS',vm_launched=False))
            for label, contract in [('missing',None),('unknown','unreviewed-placement')]:
                invalid=output/(directory.name+'-coverage-'+label);shutil.copytree(directory,invalid)
                altered=copy.deepcopy(plan);altered['stack_coverage_contract']=contract
                (invalid/'plan.json').write_bytes(E.canonical(altered))
                (invalid/'plan.sha256').write_text(E.sha(invalid/'plan.json')+'\n')
                try:X.bound_plan(invalid)
                except ValueError as error:assert 'stack coverage' in str(error)
                else:raise AssertionError('current plan evaded deterministic coverage')
                cases.append(dict(name='coverage-'+label+'-'+role+'-'+str(order),status='REJECTED',vm_launched=False))
            altered=copy.deepcopy(plan);altered['timestamp_primitive']=X.PRIMITIVES[1-order]
            invalid=output/(directory.name+'-relabel');shutil.copytree(directory,invalid)
            (invalid/'plan.json').write_bytes(E.canonical(altered))
            (invalid/'plan.sha256').write_text(E.sha(invalid/'plan.json')+'\n')
            try:X.bound_plan(invalid)
            except ValueError:pass
            else:raise AssertionError('plan selected another compiled timestamp primitive')
            cases.append(dict(name='compiled-primitive-relabel-'+role+'-'+str(order),status='REJECTED',vm_launched=False))
            invalid=output/(directory.name+'-forged-build');shutil.copytree(directory,invalid)
            build=json.loads((invalid/'timestamp-build.json').read_text())
            build['commands'][0]['argv'][-1]='IRQ_LOCAL_TSC_ORDER='+str(1-order)
            (invalid/'timestamp-build.json').write_bytes(E.canonical(build))
            altered['timestamp_build_sha256']=E.sha(invalid/'timestamp-build.json')
            (invalid/'plan.json').write_bytes(E.canonical(altered))
            (invalid/'plan.sha256').write_text(E.sha(invalid/'plan.json')+'\n')
            try:X.bound_plan(invalid)
            except ValueError as error:assert 'compiled command binding' in str(error)
            else:raise AssertionError('forged build flag accepted after outer plan reseal')
            cases.append(dict(name='forged-compiled-flag-'+role+'-'+str(order),status='REJECTED',vm_launched=False))
    return cases


def run(output):
    output.mkdir(parents=True,exist_ok=False)
    cases=proof_export_prefixes(output)+plans(output)+roles()+hosts()+transport(output)+terminals()+stack_reports()+warning_reports()+identity_methods()+nested_updates()+detail_accounting()+analysis_failures(output)
    result=dict(status='PASS',authority='HOSTED_ORCHESTRATION_ONLY',cases=cases,vm_launched=False)
    E.write(output/'result.json',result);return result


def proof_export_prefixes(output):
    X=module('irq-latency-pairing-experiment');runtime=output/'proof-prefix';runtime.mkdir()
    prefix=dict(bytes=0,sha256='a'*64,candidate_id='candidate',vm_id='vm',sequence=2,monotonic_ns=20)
    path=runtime/'proof-export-prefix.json';path.write_bytes(E.canonical(prefix))
    rows=[dict(payload='irqlatency audit',monotonic_ns=10),dict(payload='irqlatency export',sequence=2,
        monotonic_ns=40,proof_prefix=prefix,proof_prefix_sha256=E.sha(path))]
    event=dict(event='HMP_INPUT_BEGIN',sequence=2,monotonic_ns=30)
    driver=runtime/'driver.jsonl';driver.write_bytes(E.canonical(event));X.verify_export_prefix(runtime,rows)
    cases=[dict(name='actual-export-prefix-after-audit-before-input',status='PASS')]
    for name,stamp in [('before-audit',9),('after-input',31),('after-export',41)]:
        changed=copy.deepcopy(rows);changed[-1]['proof_prefix']['monotonic_ns']=stamp
        path.write_bytes(E.canonical(changed[-1]['proof_prefix']));changed[-1]['proof_prefix_sha256']=E.sha(path)
        try:X.verify_export_prefix(runtime,changed)
        except ValueError:cases.append(dict(name=name,status='REJECTED'))
        else:raise AssertionError(name)
    path.write_bytes(E.canonical(prefix))
    for name,contents in [('missing-input',b''),('duplicate-input',E.canonical(event)*2),('wrong-input-sequence',E.canonical(dict(event,sequence=3)))]:
        driver.write_bytes(contents)
        try:X.verify_export_prefix(runtime,rows)
        except ValueError:cases.append(dict(name=name,status='REJECTED'))
        else:raise AssertionError(name)
    driver.write_bytes(E.canonical(event));path.write_bytes(E.canonical(dict(prefix,bytes=1)))
    try:X.verify_export_prefix(runtime,rows)
    except ValueError:cases.append(dict(name='changed-prefix-file',status='REJECTED'))
    else:raise AssertionError('changed prefix')
    path.write_bytes(E.canonical(prefix));X.verify_export_prefix(runtime,rows)
    return cases


def roles():
    X=module('irq-latency-pairing-experiment');cases=[]
    commands=[dict(payload=p,handler_status=0) for p in ('mem','irqlatency stress','irqlatency finish','irqlatency audit')]
    raw={'CALIBRATION_END':[dict(status='VALID')]};post={'cpus':[dict(clock_errors=0,observer_errors=0)]}
    assert X.semantic_commands('CURRENT_TIMING_QUALIFICATION',commands,raw,post,True)['timing_commands_success']
    cases.append(dict(name='timing-semantic-success',status='PASS'))
    commands[2]['handler_status']=1;raw['CALIBRATION_END'][0]['status']='INVALID_CALIBRATION';post['cpus'][0]['observer_errors']=4
    result=X.semantic_commands('CURRENT_FUNCTIONAL_STRESS',commands,raw,post,True)
    assert result['functional_commands_success'] and not result['timing_commands_success'] and result['finish_handler_status']==1
    cases.append(dict(name='functional-explicit-observer-refusal',status='PASS'))
    assert not X.semantic_commands('CURRENT_TIMING_QUALIFICATION',commands,raw,post,True)['functional_commands_success']
    for name,index,status,functional,ending in [('ordinary-status-one',0,1,True,'INVALID_CALIBRATION'),
        ('wrong-finish-status',2,2,True,'INVALID_CALIBRATION'),('functional-corruption',2,1,False,'INVALID_CALIBRATION'),
        ('incomplete-terminal',2,1,True,'INCOMPLETE_TERMINAL')]:
        altered=copy.deepcopy(commands);altered[index]['handler_status']=status
        assert not X.semantic_commands('CURRENT_FUNCTIONAL_STRESS',altered,{'CALIBRATION_END':[dict(status=ending)]},post,functional)['functional_commands_success']
        cases.append(dict(name=name,status='REJECTED'))
    post['cpus'][0]['observer_errors']=0
    assert not X.semantic_commands('CURRENT_FUNCTIONAL_STRESS',commands,raw,post,True)['functional_commands_success']
    cases.append(dict(name='unexplained-calibration-refusal',status='REJECTED'))
    return cases


def analysis_failures(output):
    from contextlib import ExitStack
    X=module('irq-latency-pairing-experiment');cases=[]
    plan=dict(profile={'smp':8},elf_sha256='1'*64,image_sha256='2'*64,
        vm_ids=['synthetic'],candidate_id='candidate',role='DIAGNOSTIC_NO_ACCEPTANCE',entry_contract=X.J.CONTRACT,
        timestamp_primitive=X.PRIMITIVES[0],stack_coverage_contract=X.W.STACK_COVERAGE)
    launch=dict(elf={'sha256':plan['elf_sha256']},image={'sha256':plan['image_sha256']},argv=['qemu','-uuid','synthetic'],fault_trace=False)
    raw={'CALIBRATION_END':[dict(status='VALID')]}
    detail=dict(valid=True,cpus=[dict(clock_errors=0,observer_errors=0) for _ in range(8)])
    commands=[dict(payload=p,handler_status=0) for p in ('mem','irqlatency stress','irqlatency finish','irqlatency audit')]
    for name,failing in [('invalid-interior',X.I),('invalid-checkpoint',X.J)]:
        directory=output/name;runtime=directory/'boot-0/runtime';runtime.mkdir(parents=True)
        E.write(runtime/'launch.json',launch);E.write(runtime/'host-terminal.json',{})
        (runtime/'qemu-serial.log').write_text('synthetic');(runtime/'qemu-debugcon.log').write_bytes(b'')
        with ExitStack() as stack:
            for obj,method,kw in [
                (X,'bound_plan',dict(return_value=plan)),(X.V,'records',dict(return_value=raw)),
                (X.P,'method',dict(return_value=True)),(X,'observer_method',dict(return_value=True)),
                (X.P,'role_result',dict(return_value=({},detail))),
                (X.A,'verify_profile_series',dict(return_value={'status':'PASS','timing_authority_eligible':True})),
                (X.J,'verify',dict(return_value={'status':'PASS'})),(X.I,'verify',dict(return_value={'status':'PASS'})),
                (X.V,'inspect',dict(return_value={'invalid_boundaries':4})),(X.W,'verify',dict(return_value={'status':'PASS'})),
                (X,'command_proof',dict(return_value=commands)),(X.L,'verify',dict(return_value={'status':'PASS'})),
                (X,'functional_raw',dict(side_effect=ValueError('invalid recorder state')))]:
                stack.enter_context(patch.object(obj,method,**kw))
            stack.enter_context(patch.object(failing,'verify',side_effect=ValueError(name)))
            result=X.analyze(directory)
        field='interior' if failing is X.I else 'entry_checkpoints'
        assert result[field]['status']=='UNAVAILABLE' and result[field]['reason']==name
        assert result['raw']['invalid_boundaries']==4 and result['service_audit']['status']=='PASS'
        assert result['workload']['status']=='PASS' and result['functional']['status']=='FAIL'
        assert not result['qualified'] and not result['functional_complete'] and not result['implementation_acceptance']
        assert json.loads((directory/'boot-0/analysis.json').read_text())==result
        cases.append(dict(name=name+'-preserved-without-promotion',status='PASS'))
    return cases


def hosts():
    cases=[]
    for count,role in ((8,'CURRENT_TIMING_QUALIFICATION'),(24,'CURRENT_FUNCTIONAL_STRESS')):
        group=dict(proc_cgroup=['0::/'],ancestors=[dict(path='/sys/fs/cgroup',fields={
            'cgroup.controllers':'cpu cpuset memory','cgroup.subtree_control':'cpu cpuset memory',
            'cpu.max':'max 100000','cpuset.cpus.effective':'0-11','memory.max':'max','memory.current':'1024',
            'memory.high':'max','memory.events':'oom 0\noom_kill 0\n'})])
        argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-smp',f'{count},sockets=1,cores={count},threads=1','-uuid','synthetic','-m','2G']
        cpus=[{'cpu-index':i,'thread-id':100+i} for i in range(count)]
        row=dict(schema=1,guest_cpus=count,placement_changed=False,affinity=list(range(12)),online=list(range(12)),cgroup=group,
            usable_cpus=list(range(12)),capacity_numerator=12,capacity_denominator=1,capacity_sufficient=count<=12,
            kvm_open='PASS',vm_pid=99,observed_pid=99,qemu_processes=[dict(pid=99,argv=argv)],
            threads=[dict(tid=100+i,affinity=list(range(12)),cgroup=copy.deepcopy(group),schedstat='1 2 3') for i in range(count)],
            qmp_identity=dict(uuid='synthetic',cpus=cpus,transcript=[{'receive':{'id':'irq-host-query-uuid','return':{'UUID':'synthetic'}}},
                {'receive':{'id':'irq-host-query-cpus-fast','return':cpus}}]),
            topology=[dict(cpu=i,thread_siblings_list=str(i),physical_package_id='0',core_id=str(i)) for i in range(12)],host_boot_id='synthetic')
        rows=[dict(copy.deepcopy(row),monotonic_ns=i) for i in (1,2,3)];launch=dict(argv=argv)
        result=A.verify_profile_series(rows,launch,role,count)
        assert result['guest_cpus']==count and result['timing_authority_eligible']==(count==8)
        assert result['capacity_sufficient']==(count==8)
        cases.append(dict(name=f'actual-host-profile-{count}',status='PASS'))
        for name,selected,cpucount in [('wrong-topology',role,24 if count==8 else 8),('wrong-role','CURRENT_TIMING_QUALIFICATION',24)]:
            try:A.verify_profile_series(rows,launch,selected,cpucount)
            except ValueError:pass
            else:raise AssertionError(name)
            cases.append(dict(name=f'{name}-{count}',status='REJECTED'))
        broken=copy.deepcopy(rows);broken[-1]['threads'].pop()
        try:A.verify_profile_series(broken,launch,role,count)
        except ValueError:pass
        else:raise AssertionError('missing actual vCPU thread accepted')
        cases.append(dict(name=f'missing-thread-{count}',status='REJECTED'))
    return cases


def transport(output):
    import hashlib
    import irq_latency_transport as K
    F=K.module('foundation-qemu');runtime=output/'transport';runtime.mkdir()
    layout=dict(schema=1,controller_size=1024,slot_count=64,key_count=6)
    offset=0
    for name,size in [('prev_keys',384),('prev_mods',64),('repeat_key',64),('repeat_mods',64),('repeat_active',64)]:
        layout[name+'_offset']=offset;layout[name+'_size']=size;offset+=size
    E.write(runtime/'input-layout.json',layout)
    virtual=dict(g_buffer=4096,g_len=4352,g_pos=4356,g_shell_active=4360,xhci_driver=8192)
    symbols=''.join(f'{v:016x} B {k}\n' for k,v in virtual.items());(runtime/'input-symbols.txt').write_text(symbols)
    readelf='  LOAD 0x000000 0x000000 0x000000 0x1000 0x10000 RW 0x1000\n'
    segments=[dict(offset=0,virtual_address=0,physical_address=0,file_size=4096,memory_size=65536)]
    E.write(runtime/'input-address-map.json',dict(schema=1,kind='elf-input-physical-address-map',elf_sha256='1'*64,
        readelf_argv=['readelf','-lW','kernel.elf'],readelf_stdout=readelf,load_segments=segments,
        virtual_addresses={k:f'0x{v:016x}' for k,v in virtual.items()},physical_addresses={k:f'0x{v:016x}' for k,v in virtual.items()}))
    E.write(runtime/'input-identity.json',dict(elf_sha256='1'*64,address_sha256=E.sha(runtime/'input-address-map.json'),
        layout_sha256=E.sha(runtime/'input-layout.json'),symbols_sha256=E.sha(runtime/'input-symbols.txt')))
    plan=dict(candidate_id='candidate',elf_sha256='1'*64,vm_ids=['synthetic'],transport_contract=K.CONTRACT)
    payload='irqlatency finish';crc,frame=F.build_command_frame(1,payload);clock=[1.0]
    def now():clock[0]+=.001;return clock[0]
    class Qmp:
        def __init__(self,path,log,deadline):
            self.log=log;self.log.write_text('');self.i=0;self.text='';self.enter=False;self.record('receive',{'QMP':{}});self.command('qmp_capabilities',{},deadline)
        def record(self,direction,message):
            with self.log.open('a') as stream:stream.write(json.dumps(dict(direction=direction,host_monotonic=now(),message=message))+'\n')
        def command(self,name,args,deadline):
            self.i+=1;identity='input-'+str(self.i);request=dict(execute=name,id=identity)
            if args:request['arguments']=args
            self.record('send',request)
            if name=='query-uuid':value={'UUID':'synthetic'}
            elif name=='qmp_capabilities':value={}
            elif args['command-line'].startswith('sendkey '):
                key=args['command-line'].split()[1]
                if key=='ret':
                    self.enter=True;(runtime/'qemu-serial.log').write_text(f'[HARNESS][FRAME] ACCEPT seq=1 crc={crc} len={len(payload)}\n[HARNESS][BEGIN] seq=1\n[HARNESS][END] seq=1 status=0\n')
                else:self.text=frame[:len(self.text)+1]
                value=''
            else:
                command=args['command-line'].split();size=int(command[1][1:-2]);address=int(command[2],0);data=bytearray(size)
                if address==4096:
                    text='' if self.enter else self.text;data[:len(text)]=text.encode();data[256:260]=len(text).to_bytes(4,'little');data[260:264]=len(text).to_bytes(4,'little');data[264]=int(not self.enter)
                value=''.join(f'{address+i:016x}:'+''.join(f' 0x{b:02x}' for b in data[i:i+8])+'\n' for i in range(0,size,8))
            self.record('receive',dict(id=identity,**{'return':value}));return identity,{'return':value}
        def close(self):pass
    original=K.module
    def load(name):
        if name=='foundation-qemu':return F
        return original(name)
    original_read=Path.read_bytes;serial_reads=[0]
    def split_serial(path):
        data=original_read(path)
        if path==runtime/'qemu-serial.log':
            serial_reads[0]+=1
            if serial_reads[0]==1:return data[:data.index(b' len=')+6]
            if serial_reads[0]==2:return data[:data.index(b'\n')]
        return data
    with patch.object(K,'module',side_effect=load),patch.object(F,'QmpInputClient',Qmp),patch.object(K.time,'monotonic',side_effect=now),patch.object(K.time,'sleep',side_effect=lambda n:clock.__setitem__(0,clock[0]+n)),patch.object(Path,'read_bytes',split_serial):
        K.capture(runtime,plan,0,1,payload)
    assert serial_reads[0]>=3
    path=runtime/'input-1-capture.json';original_bytes=path.read_bytes();cases=[dict(name='actual-keyboard-capture-and-raw-replay',status='PASS')]
    cases.append(dict(name='actual-capture-waits-for-complete-serial-line',status='PASS'))
    for name,mutate in [('missing-release',lambda r:r.update(snapshots=[x for x in r['snapshots'] if x['kind']!='release'])),
                        ('raised-release-deadline',lambda r:r.update(release_deadline_seconds=6)),
                        ('wrong-vm',lambda r:r.update(vm_id='other')),
                        ('late-release',lambda r:r['snapshots'][-1]['sample'].update(observed_monotonic=99999))]:
        row=json.loads(original_bytes);mutate(row);path.write_text(json.dumps(row))
        try:K.verify_input(runtime,plan,0,1,payload)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'));path.write_bytes(original_bytes)
    X=module('irq-latency-pairing-experiment');plan['commands']=[payload]
    frame_row=X.framed_command_result(plan,runtime,0,1,payload)
    (runtime/'framed.jsonl').write_text(json.dumps(frame_row)+'\n');assert X.command_proof(plan,runtime,0)==[frame_row]
    serial=(runtime/'qemu-serial.log').read_bytes()
    for name,tail in [('replay',b'[HARNESS][REPLAY] seq=1 status=0\n'),('reject',b'[HARNESS][FRAME] REJECT seq=1 reason=synthetic\n'),('duplicate',serial)]:
        (runtime/'qemu-serial.log').write_bytes(serial+tail)
        try:X.command_proof(plan,runtime,0)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name='actual-command-'+name,status='REJECTED'))
    (runtime/'qemu-serial.log').write_bytes(serial)
    return cases


def terminals():
    import zlib
    import irq_latency_terminal as L
    cases=[]
    def fixture(count):
        plan=dict(terminal_contract=L.CONTRACT,commands=['irqlatency stress','irqlatency finish','irqlatency audit'],
            profile=dict(smp=count),vm_ids=['synthetic'],candidate_id='candidate')
        lines=[f'[SMP][CPU] ONLINE slot={i} apic={i} role='+('BSP' if i==0 else 'AP') for i in range(count)]
        lines.append('[LOW_ROOT] ACTIVE root=0000000000120000 empty=1');commands=[]
        def raw(kind,fields):
            body=f'[IRQ_LATENCY][{kind}] '+fields;lines.append(body+f' crc={zlib.crc32(body.encode()):08x}')
        def start(payload):
            lines.append('[HARNESS][BEGIN] seq='+str(len(commands)+1))
            commands.append(dict(payload=payload,handler_status=0,begin_line=len(lines),vm_id='synthetic',candidate_id='candidate'))
        def end():
            lines.append('[HARNESS][END] seq='+str(len(commands))+' status=0');commands[-1]['end_line']=len(lines)
        start(plan['commands'][0]);end();start(plan['commands'][1])
        raw('STACK_REPORT_CUTOFF','role=work admitted=0 consumed=0 complete=1')
        raw('WARNING_REPORT_CUTOFF','role=work admitted=0 consumed=0 complete=1')
        raw('BEGIN',f'epoch=1 cpus={count} closure={L.CLOSURE}')
        def reference(role,cpu,tsc):
            raw('REFERENCE',f'role={role} cpu={cpu} end_cpu={cpu} tsc_before={tsc} tsc_after={tsc+1} acquired=1 sample_valid=1 initialized=1 if_enabled=1 if_after=1 identity_valid=1 switch_before=0 switch_after=0 mode=1 bits=64 retries=0 valid=1')
        for cpu in range(count):reference(0,cpu,100)
        raw('WORK_CUTOFF',f'epoch=1 admitted=1 completed=1 cpus={count} joined=0')
        raw('SNAPSHOT_READY',f'epoch=1 cpus={count} frozen={count} closure={L.CLOSURE}')
        for cpu in range(count):reference(1,cpu,300)
        for cpu in range(count):raw('CALIBRATION_WORKER',f'role=1 index={cpu} task={cpu+1} lifecycle=1 done=1 joined=1')
        raw('STACK_REPORT_BEGIN',f'first=0 limit={2*count}')
        for cpu in range(count):
            for kind in ('ALLOC','RETIRE'):
                request=2+2*cpu+int(kind=='RETIRE')
                body=f'[STACK_GUARD][{kind}] slot={cpu} generation=1 task={cpu+1} lifecycle=1 base=ffffe00000001000 physical=1000 bytes=16384 request={request} requested={2**count-1:x} acknowledged={2**count-1:x} status=COMPLETE'
                lines.append(body+f' crc={zlib.crc32(body.encode()):08x}')
        raw('STACK_REPORT_END',f'first=0 after={2*count} admitted={2*count} status=COMPLETE')
        for cpu in range(count):raw('CPU',f'cpu={cpu} cutoff=200')
        raw('SNAPSHOT_CUTOFF',f'epoch=1 admitted={2*count+1} completed={2*count+1} cpus={count} joined=1');end();start(plan['commands'][2])
        raw('AUDIT_BEGIN',f'epoch=1 admitted={2*count+2} completed={2*count+2} cpus={count} root=0x120000')
        for generation in range(1,2*count+3):
            raw('SERVICE',f'generation={generation} operation=1 mapping=1 root=0x120000 first=0x1000 last=0x1000 pages=1 strategy=1 targets={2**count-1} ack={2**count-1} missing=0 failed=0 sent=0 send_failed=0 notifications=0 status=0 elapsed_ns=100 identities='+','.join(f'{i}:{i}' for i in range(count)))
        raw('STACK_REPORT_CUTOFF',f'role=final admitted={2*count} consumed={2*count} complete=1')
        raw('WARNING_REPORT_CUTOFF','role=final admitted=0 consumed=0 complete=1')
        raw('AUDIT_END',f'epoch=1 admitted={2*count+2} completed={2*count+2} emitted={2*count+2} status=COMPLETE');end()
        return lines,plan,commands
    for count in (8,24):
        lines,plan,commands=fixture(count);assert L.verify('\n'.join(lines),b'',plan,commands)['service']['last']==2*count+2
        cases.append(dict(name=f'complete-terminal-{count}',status='PASS'))
    lines,plan,commands=fixture(8)
    for name,change in [('missing-final-service',lambda ls,p,c:ls.__setitem__(next(i for i,l in enumerate(ls) if '[SERVICE] generation=3 ' in l),'REMOVED')),
        ('borrowed-vm',lambda ls,p,c:c[-1].update(vm_id='other')),
        ('borrowed-candidate',lambda ls,p,c:c[-1].update(candidate_id='other')),
        ('freeze-before-stress-end',lambda ls,p,c:c[-3].update(end_line=c[-2]['end_line'])),
        ('ordinary-after-freeze',lambda ls,p,c:p['commands'].append('mem')),
        ('snapshot-request-after-final-audit',lambda ls,p,c:ls.append(next(l for l in ls if '[SERVICE] generation=3 ' in l))),
        ('missing-snapshot-cpu',lambda ls,p,c:ls.__setitem__(next(i for i,l in enumerate(ls) if '[CPU] cpu=7 ' in l),'REMOVED'))]:
        a,b,c=copy.deepcopy((lines,plan,commands));change(a,b,c)
        try:L.verify('\n'.join(a),b'',b,c)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    import irq_latency_evidence as V
    raw=V.records('\n'.join(lines));assert L.calibration_closure(raw,8)['status']=='PASS'
    for name,change in [
        ('post-reference-before-cutoff',lambda r:r['REFERENCE'][-1].update(tsc_before='199',tsc_after='200')),
        ('cutoff-before-pre-reference',lambda r:r['CPU'][0].update(cutoff='100')),
        ('missing-post-reference',lambda r:r.__setitem__('REFERENCE',[p for p in r['REFERENCE'] if p['role']=='0' or p['cpu']!='7'])),
        ('missing-snapshot-ready',lambda r:r.__setitem__('SNAPSHOT_READY',[])),
        ('incomplete-snapshot',lambda r:r['SNAPSHOT_READY'][0].update(frozen='7')),
        ('old-closure-relabel',lambda r:r['BEGIN'][0].update(closure='legacy')),
        ('maximum-after-cutoff',lambda r:r['WITNESS'].append(dict(cpu='0',kind='maximum',index='0',first='0',cells='150,201'))),
        ('maximum-before-pre-reference',lambda r:r['WITNESS'].append(dict(cpu='0',kind='maximum',index='0',first='0',cells='90,150'))),
    ]:
        changed=copy.deepcopy(raw);change(changed)
        try:L.calibration_closure(changed,8)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for name,old,new in [
        ('ordinary-task-after-cutoff','task=1 lifecycle=1','task=100 lifecycle=1'),
        ('zero-post-worker-owner','task=1 lifecycle=1','task=0 lifecycle=1'),
        ('post-worker-not-retired','done=1 joined=1','done=1 joined=0'),
        ('wrong-post-stack-generation','slot=0 generation=1 task=1 lifecycle=1 base=','slot=0 generation=2 task=1 lifecycle=1 base='),
    ]:
        changed=[];done=False
        for line in lines:
            if old in line and not done and ('[RETIRE]' in line or '[CALIBRATION_WORKER]' in line):
                body=line.rpartition(' crc=')[0].replace(old,new);line=body+f' crc={zlib.crc32(body.encode()):08x}';done=True
            changed.append(line)
        assert done
        try:L.verify('\n'.join(changed),b'',plan,commands)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return cases

def stack_reports():
    import zlib
    import irq_latency_stack_reports as R
    def line(body):return body+f' crc={zlib.crc32(body.encode()):08x}'
    bodies=['[IRQ_LATENCY][BEGIN] epoch=1',
        '[IRQ_LATENCY][STACK_REPORT_BEGIN] first=0 limit=2',
        '[STACK_GUARD][ALLOC] slot=1 generation=2',
        '[STACK_GUARD][RETIRE] slot=1 generation=2',
        '[IRQ_LATENCY][STACK_REPORT_END] first=0 after=2 admitted=2 status=COMPLETE',
        '[IRQ_LATENCY][STACK_REPORT_CUTOFF] role=work admitted=2 consumed=2 complete=1',
        '[IRQ_LATENCY][WORK_CUTOFF] epoch=1',
        '[IRQ_LATENCY][STACK_REPORT_CUTOFF] role=final admitted=2 consumed=2 complete=1',
        '[IRQ_LATENCY][AUDIT_END] epoch=1']
    def check(values):return R.verify('\n'.join(line(v) for v in values))
    assert check(bodies)['records']==2
    cases=[dict(name='complete-stack-report-drain',status='PASS')]
    for name,change in [
        ('missing-stack-report',lambda r:r.pop(2)),
        ('duplicate-stack-report',lambda r:r.insert(2,r[2])),
        ('missing-stack-drain-end',lambda r:r.pop(4)),
        ('missing-stack-final-cutoff',lambda r:r.pop(7)),
        ('wrong-stack-cutoff-role',lambda r:r.__setitem__(7,r[7].replace('final','work'))),
        ('stack-report-sequence-gap',lambda r:r.__setitem__(1,r[1].replace('first=0','first=1'))),
        ('stack-report-after-work-cutoff',lambda r:r.insert(7,r[2])),
        ('stack-report-after-cutoff-admitted',lambda r:r.__setitem__(7,r[7].replace('admitted=2','admitted=3'))),
        ('stack-report-incomplete',lambda r:r.__setitem__(4,r[4].replace('COMPLETE','INCOMPLETE'))),
    ]:
        rows=bodies[:];change(rows)
        try:check(rows)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return cases

def identity_methods():
    X=module('irq-latency-pairing-experiment')
    plan=dict(identity_contract='stack-mask-identity-v2',entry_admission_contract='entry-admission-cookie-v1',observer_update_contract=X.OBSERVER_UPDATES,mmio_identity_contract=X.MMIO_IDENTITY,request_timing_contract=X.CYCLES.CONTRACT,detail_policy=X.DETAILS_RETAINED)
    raw=dict(BEGIN=[dict(identity_method='stack-mask-identity-v2',entry_admission='entry-admission-cookie-v1',observer_updates=X.OBSERVER_UPDATES,mmio_identity=X.MMIO_IDENTITY,writeback_method='unavailable',request_timing=X.CYCLES.CONTRACT,heap_method='unavailable',heap_operation_method='unavailable',clock_body_method='unavailable',tick_body_method='unavailable',detail_policy=X.DETAILS_RETAINED)])
    plan['proof_output']=dict(method=X.PROOF.CONTRACT,policy='deferred',session='0000000000000001',invocations=1)
    plan['commands']=['irqlatency proof deferred 0000000000000001 1','irqlatency begin','irqlatency stress','irqlatency finish','irqlatency audit','irqlatency export']
    plan['role']='DIAGNOSTIC_NO_ACCEPTANCE'
    raw['BEGIN'][0]['proof_output']='2'
    plan['timestamp_primitive']=raw['BEGIN'][0]['primitive']=X.PRIMITIVES[0]
    plan['terminal_contract']=X.L.CONTRACT;raw['BEGIN'][0]['closure']=X.L.CLOSURE
    assert X.observer_method(plan,raw)
    cases=[dict(name='current-observer-identity-bound',status='PASS')]
    local=copy.deepcopy(raw);local['BEGIN'][0]['primitive']=X.PRIMITIVES[1]
    assert X.observer_method(dict(plan,timestamp_primitive=X.PRIMITIVES[1]),local)
    cases.append(dict(name='current-local-order-bound',status='PASS'))
    for name,selected,records in [('new-read-claimed-old-primitive',plan,local),
        ('old-capture-claimed-new-primitive',dict(plan,timestamp_primitive=X.PRIMITIVES[1]),raw)]:
        try:X.observer_method(selected,records)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for name,value in [('legacy-observer-claimed-current',None),('publication-race-method-claimed-current','stack-mask-identity-v1'),('unknown-identity-method','other')]:
        changed=copy.deepcopy(raw);changed['BEGIN'][0]['identity_method']=value
        try:X.observer_method(plan,changed)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    for value in (None,'unknown'):
        changed=copy.deepcopy(raw);changed['BEGIN'][0]['entry_admission']=value
        try:X.observer_method(plan,changed)
        except ValueError:pass
        else:raise AssertionError('entry admission mismatch')
        cases.append(dict(name='entry-admission-'+str(value),status='REJECTED'))
    diagnostic={**plan,'entry_contract':X.J.CONTRACT,'writeback_contract':X.B.CONTRACT,'request_timing_contract':X.CYCLES.CONTRACT,'heap_contract':X.F.CONTRACT,'heap_operation_contract':X.F.OPERATION_CONTRACT,'clock_body_contract':X.I.CLOCK_CONTRACT,'tick_body_contract':X.I.TICK_CONTRACT}
    current=dict(BEGIN=[dict(raw['BEGIN'][0],writeback_method=X.B.CONTRACT,request_timing=X.CYCLES.CONTRACT,heap_method=X.F.CONTRACT,heap_operation_method=X.F.OPERATION_CONTRACT,clock_body_method=X.I.CLOCK_CONTRACT,tick_body_method=X.I.TICK_CONTRACT)])
    assert X.observer_method(diagnostic,current)
    for name,selected,records in (
        ('tick-method-missing',diagnostic,dict(BEGIN=[{k:v for k,v in current['BEGIN'][0].items() if k!='tick_body_method'}])),
        ('tick-method-unknown',diagnostic,dict(BEGIN=[dict(current['BEGIN'][0],tick_body_method='unknown')])),
        ('tick-ordinary-relabel',plan,dict(BEGIN=[dict(raw['BEGIN'][0],tick_body_method=X.I.TICK_CONTRACT)])),
        ('clock-method-missing',diagnostic,dict(BEGIN=[{k:v for k,v in current['BEGIN'][0].items() if k!='clock_body_method'}])),
        ('clock-method-unknown',diagnostic,dict(BEGIN=[dict(current['BEGIN'][0],clock_body_method='unknown')])),
        ('clock-ordinary-relabel',plan,dict(BEGIN=[dict(raw['BEGIN'][0],clock_body_method=X.I.CLOCK_CONTRACT,tick_body_method=X.I.TICK_CONTRACT)])),
        ('operation-method-missing',diagnostic,dict(BEGIN=[{k:v for k,v in current['BEGIN'][0].items() if k!='heap_operation_method'}])),
        ('operation-method-unknown',diagnostic,dict(BEGIN=[dict(current['BEGIN'][0],heap_operation_method='unknown')])),
        ('operation-ordinary-relabel',plan,dict(BEGIN=[dict(raw['BEGIN'][0],heap_operation_method=X.F.OPERATION_CONTRACT,clock_body_method=X.I.CLOCK_CONTRACT,tick_body_method=X.I.TICK_CONTRACT)])),
        ('operation-ordinary-hidden-row',plan,dict(raw,HEAP_OPERATION=[dict(cpu='0')]))):
        try:X.observer_method(selected,records)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))

    cases.append(dict(name='writeback-current-diagnostic-bound',status='PASS'))
    for value in (None,'unknown'):
        changed=dict(BEGIN=[dict(current['BEGIN'][0],writeback_method=value)])
        try:X.observer_method(diagnostic,changed)
        except ValueError:pass
        else:raise AssertionError('writeback method mismatch')
        cases.append(dict(name='writeback-method-'+str(value),status='REJECTED'))
    try:X.observer_method({**diagnostic,'writeback_contract':'unknown'},current)
    except ValueError:pass
    else:raise AssertionError('writeback plan mismatch')
    cases.append(dict(name='writeback-plan-mismatch',status='REJECTED'))
    for value in ('unknown',None):
        changed=copy.deepcopy(current);changed['BEGIN'][0]['request_timing']=value
        try:X.observer_method(diagnostic,changed)
        except ValueError:pass
        else:raise AssertionError('clock-call method mismatch')
        cases.append(dict(name='clock-call-method-'+str(value),status='REJECTED'))
    try:X.observer_method({**diagnostic,'request_timing_contract':'unknown'},current)
    except ValueError:pass
    else:raise AssertionError('clock-call plan mismatch')
    cases.append(dict(name='clock-call-plan-mismatch',status='REJECTED'))
    contrast={**diagnostic,'role':'DIAGNOSTIC_NO_ACCEPTANCE','detail_policy':X.DETAILS_OMITTED}
    contrast_raw=copy.deepcopy(current);contrast_raw['BEGIN'][0]['detail_policy']=X.DETAILS_OMITTED
    assert X.observer_method(contrast,contrast_raw)
    cases.append(dict(name='explicit-diagnostic-details-off',status='PASS'))
    for name,selected,records in (
        ('omitted-details-timing-role',{**contrast,'role':'CURRENT_TIMING_QUALIFICATION'},contrast_raw),
        ('undeclared-detail-change',diagnostic,contrast_raw),
        ('missing-detail-policy',diagnostic,{'BEGIN':[{k:v for k,v in current['BEGIN'][0].items() if k!='detail_policy'}]})):
        try:X.observer_method(selected,records)
        except ValueError:pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    assert not X.raw_qualified({},contrast_raw)
    cases.append(dict(name='detail-contrast-cannot-pass-timing',status='REJECTED'))

    for value in (None,'unknown'):
        changed=copy.deepcopy(current);changed['BEGIN'][0]['heap_method']=value
        try:X.observer_method(diagnostic,changed)
        except ValueError:pass
        else:raise AssertionError('heap method mismatch')
        cases.append(dict(name='heap-method-'+str(value),status='REJECTED'))
        try:X.observer_method({**diagnostic,'heap_contract':value},current)
        except ValueError:pass
        else:raise AssertionError('heap plan mismatch')
        cases.append(dict(name='heap-plan-'+str(value),status='REJECTED'))
    for value in (None,'unknown'):
        changed=copy.deepcopy(raw);changed['BEGIN'][0]['observer_updates']=value
        try:X.observer_method(plan,changed)
        except ValueError:pass
        else:raise AssertionError('nested observer method mismatch')
        cases.append(dict(name='nested-observer-method-'+str(value),status='REJECTED'))
        try:X.observer_method({**plan,'observer_update_contract':value},raw)
        except ValueError:pass
        else:raise AssertionError('nested observer plan mismatch')
        cases.append(dict(name='nested-observer-plan-'+str(value),status='REJECTED'))
        changed=copy.deepcopy(raw);changed['BEGIN'][0]['mmio_identity']=value
        try:X.observer_method(plan,changed)
        except ValueError:pass
        else:raise AssertionError('MMIO identity mismatch')
        cases.append(dict(name='mmio-identity-method-'+str(value),status='REJECTED'))
        try:X.observer_method({**plan,'mmio_identity_contract':value},raw)
        except ValueError:pass
        else:raise AssertionError('MMIO identity plan mismatch')
        cases.append(dict(name='mmio-identity-plan-'+str(value),status='REJECTED'))
    return cases

def detail_accounting():
    X=module('irq-latency-pairing-experiment');cases=[]
    raw=dict(BEGIN=[dict(detail_policy=X.DETAILS_OMITTED)],CPU=[dict(cpu='0',failures='0',failure_overflow='2',invalid='0')],
        MAXIMUM=[dict(cpu='0',path=str(p),violations=str(2 if p==0 else 1 if p==1 else 0)) for p in range(14)],WITNESS=[])
    result=X.optional_details(raw,1);assert result['status']=='PASS' and not result['complete_optional_history']
    cases.append(dict(name='detail-off-compulsory-accounting',status='PASS'))
    for name,change in (
        ('hidden-detail-omission',lambda r:r['CPU'][0].update(failure_overflow='0')),
        ('invented-detail-omission',lambda r:r['CPU'][0].update(failure_overflow='4')),
        ('detail-off-stored-counter',lambda r:r['CPU'][0].update(failures='1')),
        ('detail-off-extra-copy',lambda r:r['WITNESS'].append(dict(kind='failure'))),
        ('missing-compulsory-row',lambda r:r['MAXIMUM'].pop()),
        ('missing-compulsory-cpu',lambda r:r['CPU'].pop()),
        ('unknown-detail-policy',lambda r:r['BEGIN'][0].update(detail_policy='unknown'))):
        altered=copy.deepcopy(raw);change(altered)
        try:X.optional_details(altered,1)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return cases

def nested_updates():
    X=module('irq-latency-pairing-experiment')
    raw=dict(BEGIN=[dict(cpus='2',epoch='1',observer_updates=X.OBSERVER_UPDATES,mmio_identity=X.MMIO_IDENTITY)],
        CPU=[dict(cpu=str(cpu),epoch='1') for cpu in range(2)],
        NESTED_OBSERVER=[dict(cpu=str(cpu),epoch='1',elided=str(cpu*3),mmio_reused=str(cpu*2),prechecked=str(cpu)) for cpu in range(2)])
    result=X.observer_updates(raw,2)
    assert result['status']=='PASS' and result['cpus']==[dict(cpu=0,elided=0,mmio_reused=0,prechecked=0),dict(cpu=1,elided=3,mmio_reused=2,prechecked=1)]
    cases=[dict(name='nested-observer-complete-ledger',status='PASS')]
    mutations=[('missing-nested-counter',lambda r:r['NESTED_OBSERVER'].pop()),
        ('missing-precheck-counter',lambda r:r['NESTED_OBSERVER'][1].pop('prechecked')),
        ('precheck-exceeds-total',lambda r:r['NESTED_OBSERVER'][1].update(prechecked='4')),
        ('old-postcheck-method',lambda r:r['BEGIN'][0].update(observer_updates='nested-observer-noop-elision-v1')),
        ('noncanonical-precheck-counter',lambda r:r['NESTED_OBSERVER'][1].update(prechecked='01')),
        ('duplicate-nested-counter',lambda r:r['NESTED_OBSERVER'].append(r['NESTED_OBSERVER'][0])),
        ('wrong-nested-cpu',lambda r:r['NESTED_OBSERVER'][1].update(cpu='2')),
        ('wrong-nested-epoch',lambda r:r['NESTED_OBSERVER'][1].update(epoch='2')),
        ('exhausted-nested-counter',lambda r:r['NESTED_OBSERVER'][1].update(elided=str(2**64-1))),
        ('overflow-nested-counter',lambda r:r['NESTED_OBSERVER'][1].update(elided=str(2**64))),
        ('noncanonical-nested-counter',lambda r:r['NESTED_OBSERVER'][1].update(elided='03')),
        ('missing-frozen-cpu',lambda r:r['CPU'].pop()),
        ('wrong-frozen-epoch',lambda r:r['CPU'][0].update(epoch='2')),
        ('wrong-nested-topology',lambda r:r['BEGIN'][0].update(cpus='8')),
        ('old-nested-method',lambda r:r['BEGIN'][0].pop('observer_updates')),
        ('old-mmio-identity',lambda r:r['BEGIN'][0].pop('mmio_identity')),
        ('missing-mmio-counter',lambda r:r['NESTED_OBSERVER'][1].pop('mmio_reused')),
        ('exhausted-mmio-counter',lambda r:r['NESTED_OBSERVER'][1].update(mmio_reused=str(2**64-1))),
        ('overflow-mmio-counter',lambda r:r['NESTED_OBSERVER'][1].update(mmio_reused=str(2**64))),
        ('noncanonical-mmio-counter',lambda r:r['NESTED_OBSERVER'][1].update(mmio_reused='02'))]
    for name,change in mutations:
        changed=copy.deepcopy(raw);change(changed)
        try:X.observer_updates(changed,2)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return cases

def warning_reports():
    import zlib
    import irq_latency_warning_reports as R
    warning=('[ASSERT][WARN] file_hex=61 file_bytes=1 file_valid=1 file_truncated=0 line=3 '
        'expression_hex=62 expression_bytes=1 expression_valid=1 expression_truncated=0 '
        'cpu_id=0 cpu_valid=1 cpu_slot=0 slot_valid=1 task_id=12 task_generation=27 task_valid=1 if_enabled=1')
    bodies=['[IRQ_LATENCY][WARNING_REPORT_BEGIN] first=0 limit=1',
        f'[IRQ_LATENCY][WARNING_REPORT_ITEM] sequence=0 bytes={len(warning)} payload_crc={zlib.crc32(warning.encode()):08x}',
        warning,'[IRQ_LATENCY][WARNING_REPORT_END] first=0 after=1 admitted=1 status=COMPLETE',
        '[IRQ_LATENCY][WARNING_REPORT_CUTOFF] role=work admitted=1 consumed=1 complete=1',
        '[IRQ_LATENCY][WORK_CUTOFF] epoch=1',
        '[IRQ_LATENCY][WARNING_REPORT_CUTOFF] role=final admitted=1 consumed=1 complete=1',
        '[IRQ_LATENCY][AUDIT_END] epoch=1']
    def check(values):
        return R.verify('\n'.join(v if v.startswith('[ASSERT]') else v+f' crc={zlib.crc32(v.encode()):08x}' for v in values))
    assert check(bodies)['records']==1
    cases=[dict(name='original-warning-bound-and-drained',status='PASS')]
    for name,change in [
        ('missing-warning-text',lambda r:r.pop(2)),
        ('missing-warning-item',lambda r:r.pop(1)),
        ('duplicate-warning',lambda r:r.insert(3,r[2])),
        ('altered-warning-context',lambda r:r.__setitem__(2,r[2].replace('generation=27','generation=28'))),
        ('wrong-warning-sequence',lambda r:r.__setitem__(1,r[1].replace('sequence=0','sequence=1'))),
        ('missing-warning-cutoff',lambda r:r.pop(6)),
        ('warning-after-cutoff',lambda r:r.__setitem__(6,r[6].replace('admitted=1','admitted=2'))),
        ('warning-incomplete',lambda r:r.__setitem__(3,r[3].replace('COMPLETE','INCOMPLETE'))),
        ('warning-capacity-overrun',lambda r:r.__setitem__(0,r[0].replace('limit=1','limit=65'))),
    ]:
        rows=bodies[:];change(rows)
        try:check(rows)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    return cases

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    print(json.dumps(run(a.output)))
