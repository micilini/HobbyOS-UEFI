"""Strict keyboard framing using existing QMP physical-read collector/checker.

No debugger stop: raw shell bytes and all keyboard-release cells are observed
through the established physical mappings; handler framing proves dispatch.
"""
import importlib.util,json,subprocess,time
from pathlib import Path
import memory_evidence as E
import qemu_hmp

CONTRACT='strict-keyboard-release-v1'

def module(name):
    spec=importlib.util.spec_from_file_location('latency_transport_'+name.replace('-','_'),E.ROOT/'scripts'/(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m


def prepare(runtime,elf):
    F=module('foundation-qemu');virtual=F.elf_symbol_addresses(elf,(*F.SHELL_INPUT_SYMBOLS,F.XHCI_INPUT_SYMBOL))
    physical,segments,readelf,argv=F.elf_symbol_physical_addresses(elf,virtual)
    layout_binary=runtime/'input-layout';command=['gcc','-std=gnu11','-O2','scripts/command-transport-layout.c','-o',str(layout_binary)]
    completed=subprocess.run(command,cwd=E.ROOT,capture_output=True,text=True);E.require(completed.returncode==0,'input layout compilation')
    layout=subprocess.check_output([str(layout_binary)],text=True)
    (runtime/'input-layout.json').write_text(layout)
    E.write(runtime/'input-layout-build.json',dict(argv=command,exit_status=completed.returncode,
        source_sha256=E.sha(E.ROOT/'scripts/command-transport-layout.c'),binary_sha256=E.sha(layout_binary),stdout=completed.stdout,stderr=completed.stderr))
    layout_binary.unlink()
    E.write(runtime/'input-address-map.json',dict(schema=1,kind='elf-input-physical-address-map',elf_sha256=E.sha(elf),
        readelf_argv=argv,readelf_stdout=readelf,load_segments=segments,
        virtual_addresses={k:f'0x{v:016x}' for k,v in virtual.items()},physical_addresses={k:f'0x{v:016x}' for k,v in physical.items()}))
    (runtime/'input-symbols.txt').write_text(subprocess.check_output(['nm','-n',str(elf)],text=True))
    E.write(runtime/'input-identity.json',dict(elf_sha256=E.sha(elf),address_sha256=E.sha(runtime/'input-address-map.json'),
        layout_sha256=E.sha(runtime/'input-layout.json'),symbols_sha256=E.sha(runtime/'input-symbols.txt')))


def capture(runtime,plan,index,sequence,payload):
    F=module('foundation-qemu');_,layout=F.load_input_layout(runtime/'input-layout.json')
    addresses=json.loads((runtime/'input-address-map.json').read_text());virtual={k:int(v,0) for k,v in addresses['virtual_addresses'].items()}
    physical={k:int(v,0) for k,v in addresses['physical_addresses'].items()}
    crc,frame=F.build_command_frame(sequence,payload);deadline=time.monotonic()+90
    log=runtime/f'input-{sequence}.jsonl';qmp=F.QmpInputClient(runtime/'qmp.sock',log,deadline);events=[]
    row=dict(contract=CONTRACT,sequence=sequence,payload=payload,crc32=crc,candidate_id=plan['candidate_id'],
        elf_sha256=plan['elf_sha256'],vm_id=plan['vm_ids'][index],release_deadline_seconds=5,
        input_identity_sha256=E.sha(runtime/'input-identity.json'),events=events,snapshots=[])
    def execute(name,args):
        started=time.monotonic();request,response=qmp.command(name,args,deadline)
        event=dict(request_id=request,qmp_execute=name,arguments=args,expected_return=response['return'],
            started_monotonic=started,completed_monotonic=time.monotonic());events.append(event);return event
    def snapshot(kind,until,predicate):
        while time.monotonic()<until:
            sample=F.qmp_input_snapshot(qmp,virtual,physical,layout,until)
            row['snapshots'].append(dict(kind=kind,sample=sample))
            if predicate(sample):return sample
            time.sleep(.05)
        raise TimeoutError('TRANSPORT guest '+kind+' observation deadline')
    try:
        identity=execute('query-uuid',{});E.require(identity['expected_return']=={'UUID':row['vm_id']},'input VM identity')
        snapshot('clean',min(deadline,time.monotonic()+5),lambda s:s['shell_snapshot_stable'] and s['active']==1 and s['length']==s['position']==0 and s['keyboard_release_observed'])
        for ch in frame:
            execute('human-monitor-command',{'command-line':'sendkey '+qemu_hmp.keyname(ch)+' 30'});time.sleep(.1)
        snapshot('frame',min(deadline,time.monotonic()+5),lambda s:s['shell_snapshot_stable'] and s['active']==1 and s['length']==s['position']==len(frame) and s['observed_text']==frame and s['terminated'] and s['keyboard_release_observed'])
        enter=execute('human-monitor-command',{'command-line':'sendkey ret 30'});row['enter_request_id']=enter['request_id']
        until=enter['completed_monotonic']+5
        while time.monotonic()<until:
            serial=(runtime/'qemu-serial.log').read_bytes()
            serial=serial[:serial.rfind(b'\n')+1]
            accepted=F.matching(serial.decode(errors='replace').splitlines(),0,F.ACCEPT_RE,sequence)
            if accepted:
                row['dispatch_observation']=dict(bytes=len(serial),sha256=__import__('hashlib').sha256(serial).hexdigest(),
                    line=accepted[0][0],observed_monotonic=time.monotonic())
                break
            time.sleep(.02)
        E.require('dispatch_observation' in row,'input Enter dispatch not observed within release deadline')
        snapshot('release',enter['completed_monotonic']+5,lambda s:s['shell_snapshot_stable'] and s['keyboard_release_observed'])
    finally:
        qmp.close();row['qmp_log_sha256']=E.sha(log)
        E.write(runtime/f'input-{sequence}-capture.json',row)
    verify_input(runtime,plan,index,sequence,payload)
    return row


def verify_input(runtime,plan,index,sequence,payload):
    F=module('foundation-qemu');V=module('verify-foundation-regression')
    row=json.loads((runtime/f'input-{sequence}-capture.json').read_text())
    E.require(row['contract']==CONTRACT and row['release_deadline_seconds']==5 and row['sequence']==sequence and
        row['payload']==payload and row['candidate_id']==plan['candidate_id'] and row['elf_sha256']==plan['elf_sha256'] and
        row['vm_id']==plan['vm_ids'][index], 'input role/candidate/boot identity')
    identity=json.loads((runtime/'input-identity.json').read_text());address=json.loads((runtime/'input-address-map.json').read_text())
    E.require(identity['elf_sha256']==plan['elf_sha256'] and row['input_identity_sha256']==E.sha(runtime/'input-identity.json') and
        identity['address_sha256']==E.sha(runtime/'input-address-map.json') and identity['layout_sha256']==E.sha(runtime/'input-layout.json') and
        identity['symbols_sha256']==E.sha(runtime/'input-symbols.txt'),'input saved ELF tool binding')
    virtual={k:int(v,0) for k,v in address['virtual_addresses'].items()}
    proof=dict(candidate_sha256=plan['elf_sha256'],input_address_map_artifact='input-address-map.json',
        input_address_map_sha256=identity['address_sha256'],input_load_segments=address['load_segments'],
        input_symbol_physical_addresses=address['physical_addresses'])
    physical,errors=V.input_address_map_errors(proof,runtime,virtual,
        elf_facts=(plan['elf_sha256'],(runtime/'input-symbols.txt').read_text(),address['readelf_stdout']))
    E.require(not errors,'input address derivation: '+str(errors))
    _,layout=F.load_input_layout(runtime/'input-layout.json');events=list(row['events']);samples={k:[] for k in ('clean','frame','release')}
    kinds=[]
    for entry in row['snapshots']:
        E.require(entry['kind'] in samples,'input snapshot kind')
        decoded=V.qmp_snapshot_values(entry['sample'],virtual,physical,layout,events,errors)
        samples[entry['kind']].append((decoded,entry['sample']['observed_monotonic']));kinds.append(entry['kind'])
    E.require(not errors and all(samples.values()),'input raw snapshot/release completeness: '+str(errors))
    E.require(kinds==sorted(kinds,key=('clean','frame','release').index),'input snapshot stage order')
    crc,frame=F.build_command_frame(sequence,payload);E.require(row['crc32']==crc,'input CRC identity')
    clean=samples['clean'][-1][0];delivered=samples['frame'][-1][0];released,at=samples['release'][-1]
    E.require(clean[0:3]==(1,0,0) and clean[5] and clean[7],'input dirty boundary')
    E.require(delivered[0:4]==(1,len(frame),len(frame),True) and delivered[4]==frame.encode() and delivered[5] and delivered[7], 'input actual frame bytes')
    expected=[('query-uuid',{})]+[('human-monitor-command',{'command-line':'sendkey '+qemu_hmp.keyname(c)+' 30'}) for c in frame]+[('human-monitor-command',{'command-line':'sendkey ret 30'})]
    E.require([(e['qmp_execute'],e['arguments']) for e in row['events']]==expected,'input actual key ledger')
    E.require(row['events'][0]['expected_return']=={'UUID':plan['vm_ids'][index]},'input QMP UUID')
    enter=row['events'][-1];E.require(enter['request_id']==row['enter_request_id'] and
        samples['frame'][-1][1]<=enter['started_monotonic']<=enter['completed_monotonic']<=at<=enter['completed_monotonic']+5 and
        released[5] and released[7], 'input five-second Enter release')
    dispatch=row['dispatch_observation'];prefix=(runtime/'qemu-serial.log').read_bytes()[:dispatch['bytes']]
    accepts=F.matching(prefix.decode(errors='replace').splitlines(),0,F.ACCEPT_RE,sequence)
    first_release=next(e['sample'] for e in row['snapshots'] if e['kind']=='release')
    E.require(__import__('hashlib').sha256(prefix).hexdigest()==dispatch['sha256'] and len(accepts)==1 and
        accepts[0][0]==dispatch['line'] and accepts[0][1].group(2)==crc and int(accepts[0][1].group(3))==len(payload) and
        enter['completed_monotonic']<=dispatch['observed_monotonic']<=first_release['shell_memory_before_event']['started_monotonic'],
        'input release precedes actual Enter dispatch')
    for _,sample_at in samples['release']:E.require(enter['completed_monotonic']<=sample_at<=enter['completed_monotonic']+5,'input late release sample')
    log=runtime/f'input-{sequence}.jsonl';E.require(E.sha(log)==row['qmp_log_sha256'],'input QMP raw hash')
    errors=V.qmp_input_log_errors(log,events);E.require(not errors,'input raw transaction: '+str(errors))
    return dict(status='PASS',contract=CONTRACT,sequence=sequence,release_seconds=at-enter['completed_monotonic'])
