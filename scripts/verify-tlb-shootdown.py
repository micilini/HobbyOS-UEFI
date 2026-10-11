#!/usr/bin/env python3
"""Replay the running A/B proof in addition to observation and CPU policy."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
import zlib
import memory_evidence as E
import kernel_image_current as WX
import stack_current as STACK
import request_local_cycles as CYCLES

ROOT=Path(__file__).resolve().parent.parent
BASE=0xffffc000001ff000
STRIDE=0x40000000
GEOMETRY=(1,2,63,64,65)
KEYS={
 'BEGIN':'schema invocation cpus x2apic sequential concurrent_each threshold deadline_ns window_pages',
 'REPLACE':'invocation window round pte old new frame_a frame_b marker_a marker_b readers',
 'REQUEST':'invocation window round phase generation first last pages strategy targets ack missing failed sent send_failed elapsed_ns contention status',
 'CPUS':'generation group count entries',
 'LOCAL':'data',
 'RETIRE':'invocation window generation data_frames released retained_tables',
 'INITIATOR':'invocation window start_ns end_ns rounds contention',
 'END':'invocation status replacements retained',
 'TABLES':'invocation before after retained',
 'DEFERRED':'invocation window round token slot queued initial completed generation old_marker handle_released',
 'AUDIT':'invocation before completed_before after emitted_before root',
 'SERVICE':'generation operation mapping root first last pages strategy targets ack missing failed sent send_failed notifications status elapsed_ns identities',
 'OBSERVER':'invocation generation targets complete errors start_ns end_ns status'}

if WX.enabled():
    KEYS['RIGHTS']='invocation stage va physical bytes shift flags writable nx'
    KEYS['RIGHTSSCAN']='invocation stage root tables leaves bytes image_bytes direct_bytes managed_bytes scratch_bytes rwx_bytes nx complete'
if STACK.enabled():
    KEYS['RIGHTSSCAN']='invocation stage root tables leaves bytes image_bytes direct_bytes managed_bytes scratch_bytes stack_bytes rwx_bytes nx complete'
    KEYS['STACK']='invocation stage slot generation kind state task lifecycle cpu apic guard base end physical capacity owned published retirement'
    KEYS['WORKER_REAP']='invocation window task lifecycle slot generation observed_generation retirement complete'

def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'scripts'/f'{name}.py')
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def expand_trace(lines):
    """Decode declared compact records without repairing or joining raw lines."""
    checked=any(re.match(r'\[TLB\]\[BEGIN\] schema=[4567] ',line) for line in lines)
    current=any(re.match(r'\[TLB\]\[BEGIN\] schema=7 ',line) for line in lines)
    result=[]
    columns={
        'R':('REQUEST','invocation window round phase generation first last pages strategy targets ack missing failed sent send_failed notifications elapsed_ns irq_ns contention status'.split(),
             {'first','last','targets','ack','missing','failed','sent','send_failed'}),
        'X':('REPLACE',KEYS['REPLACE'].split(),{'pte','old','new','frame_a','frame_b','marker_a','marker_b'})}
    if current:
        columns['R'][1][17]='timing_schema'
    for line in lines:
        if line.startswith('[TLB]') and checked:
            body,separator,checksum=line.rpartition(' crc=')
            E.require(separator and re.fullmatch(r'[0-9a-f]{8}',checksum) and
                      checksum==f'{zlib.crc32(body.encode("ascii"))&0xffffffff:08x}',
                      'TLB record checksum mismatch')
            line=body
        match=re.fullmatch(r'\[TLB\]\[([RX])\] data=([0-9a-f:]+)',line)
        if match:
            kind,keys,hexadecimal=columns[match[1]];values=match[2].split(':')
            E.require(len(values)==len(keys),'TLB compact column inventory')
            line='[TLB]['+kind+'] '+' '.join(key+'='+('0x' if key in hexadecimal else '')+value
                                           for key,value in zip(keys,values))
        elif line.startswith('[TLB][C] '):
            match=re.fullmatch(r'\[TLB\]\[C\] generation=([1-9][0-9]*) entries=([0-9a-f:]+)',line)
            E.require(match and len(match[2].split(':'))==(6 if current else 8),'TLB compact CPU inventory')
            slot=match[2].split(':')[0]
            line=f'[TLB][CPUS] generation={match[1]} group={slot} count=1 entries={match[2]}'
        result.append(line)
    return result

def rows(lines):
    result={name:[] for name in KEYS}
    for line in expand_trace(lines):
        if not line.startswith('[TLB]'):continue
        match=re.fullmatch(r'\[TLB\]\[([A-Z_]+)\] (.*)',line)
        E.require(match and match[1] in KEYS,'TLB record grammar')
        fields={}
        for pair in match[2].split():
            key,sep,value=pair.partition('=')
            E.require(sep and key not in fields,'TLB duplicate/malformed field')
            fields[key]=value
        keys=set(KEYS[match[1]].split())
        if match[1]=='BEGIN' and fields.get('schema') in ('2','3','4','5','6','7'):keys|={'deferred','observers'}
        if match[1]=='REQUEST' and 'notifications' in fields:keys|={'notifications','timing_schema' if 'timing_schema' in fields else 'irq_ns'}
        E.require(set(fields)==keys,'TLB record field inventory')
        result[match[1]].append(fields)
    return result

def n(row,key):
    E.require(re.fullmatch(r'(?:0x[0-9a-f]+|0|[1-9][0-9]*)',row[key]) is not None,'TLB numeric grammar')
    value=int(row[key],0);E.require(0<=value<1<<64,'TLB numeric range');return value

def verify_service_sequence(services,topology,root,first,last):
    """The complete absolute-generation ledger, shared by workload and terminal audits."""
    targets=sum(1<<slot for slot in topology)
    E.require(set(topology)==set(range(len(topology))) and 1<=len(topology)<=32,'TLB independent topology')
    E.require([n(r,'generation') for r in services]==list(range(first,last+1)),
              'TLB service missing/duplicate/stale generation')
    for row in services:
        E.require(set(row)==set(KEYS['SERVICE'].split()),'TLB service field inventory')
        E.require(n(row,'root')==root and n(row,'targets')==n(row,'ack')==targets and
            not any(n(row,k) for k in ('missing','failed','send_failed','status')), 'TLB service root/target/completion failure')
        E.require(row['identities']==','.join(f'{slot}:{topology[slot][0]}' for slot in sorted(topology)), 'TLB service target identity')
        E.require(n(row,'sent')&~targets==0 and n(row,'notifications')==n(row,'sent').bit_count() and
            n(row,'elapsed_ns')<1000000000,'TLB service notification/deadline')
        operation,mapping=n(row,'operation'),n(row,'mapping')
        E.require(mapping in (0,1) and operation in (1,2,3,4) and
            (operation not in (2,4) or mapping==0) and (operation!=3 or mapping==1),'TLB service operation/domain')
        if operation==4:
            E.require(all(n(row,k)==0 for k in ('first','last','pages','strategy')),'TLB protection interval')
        else:
            pages,begin,end=n(row,'pages'),n(row,'first'),n(row,'last')
            E.require(pages>0 and begin%4096==0 and end==begin+(pages-1)*4096 and
                n(row,'strategy')==(1 if pages<=64 else 2),'TLB service normalized interval')
            E.require(operation!=3 or (begin==end==0 and pages==1),'TLB cache interval')
    return dict(first=first,last=last,count=len(services),notifications=sum(n(r,'notifications') for r in services))

def verify_tlb(lines,topology,root,actual_x2,previous_generation=0,*,timing_contract=None):
    data=rows(lines)
    E.require(len(data['BEGIN'])==len(data['END'])==1,'TLB transaction begin/end cardinality')
    begin,end=data['BEGIN'][0],data['END'][0]
    count=len(topology);targets=sum(1<<slot for slot in topology)
    E.require(set(topology)==set(range(count)) and 1<=count<=32,'TLB independent topology')
    invocation=n(begin,'invocation')
    schema=n(begin,'schema')
    E.require(schema in (1,2,3,4,5,6,7) and invocation>0 and n(begin,'cpus')==count and
              n(begin,'x2apic')==int(actual_x2),'TLB topology/APIC mode binding')
    E.require(n(begin,'sequential')==256 and n(begin,'concurrent_each')==(64 if count>1 else 0) and
              n(begin,'threshold')==64 and n(begin,'deadline_ns')==1000000000 and
              n(begin,'window_pages')==65,'TLB frozen workload contract')
    E.require((schema==7 and timing_contract==CYCLES.CONTRACT) or (schema<=6 and timing_contract is None),
              'TLB source-bound current/historical timing schema')
    expected={(0,round) for round in range(256)}
    if count>1:expected|={(window,round) for window in range(2) for round in range(256,320)}
    if schema>=2:
        E.require(n(begin,'deferred')==1 and n(begin,'observers')==3,'TLB deferred/observer contract')
        expected.add((0,320))
    windows={w for w,_ in expected}
    replacements={}
    for row in data['REPLACE']:
        key=n(row,'window'),n(row,'round')
        E.require(key in expected and key not in replacements and n(row,'invocation')==invocation,'TLB replacement coverage/identity')
        window,round=key;a=n(row,'frame_a');b=n(row,'frame_b')
        E.require(a and b and a!=b and not ((a|b)&4095) and max(a,b)<1<<32,'TLB data-frame ownership')
        E.require(n(row,'marker_a')==0xa000000000000000|(invocation<<24)|(window<<20)|round and
                  n(row,'marker_b')==0xb000000000000000|(invocation<<24)|(window<<20)|round,'TLB independent markers')
        old,new=n(row,'old'),n(row,'new');address=0x000ffffffffff000
        E.require(old&address==a and new&address==b and old&~(address|0x60) in ((3,3|(1<<63)) if WX.enabled() else (3,)) and
                  new&~address==old&~address,'TLB owned PTE transition/attributes')
        E.require(n(row,'pte') and not n(row,'pte')%8 and n(row,'readers')==0,'TLB PTE identity/readers not quiescent')
        replacements[key]=row
    E.require(set(replacements)==expected,'TLB incomplete replacement generations')
    for window in windows:
        subset=[r for (w,_),r in replacements.items() if w==window]
        E.require(len({(n(r,'pte'),n(r,'frame_a'),n(r,'frame_b')) for r in subset})==1,'TLB storage reused before retirement')
    if len(windows)==2:
        ownership=[{n(r,'frame_a'),n(r,'frame_b')} for key,r in replacements.items() if key in ((0,256),(1,256))]
        E.require(len(ownership)==2 and not ownership[0]&ownership[1],'TLB initiators share data-frame ownership')
    required={(w,r,p) for w,r in expected for p in range(4)}|{(w,0xffffffff,0) for w in windows}
    requests={};generations={};max_latency=0;max_local=0;max_handler=0;max_irq=0;notifications=0;loads=0
    for row in data['REQUEST']:
        key=n(row,'window'),n(row,'round'),n(row,'phase');generation=n(row,'generation')
        E.require(key in required and key not in requests and generation not in generations and
                  n(row,'invocation')==invocation,'TLB request identity/duplicate')
        window,round,phase=key;pages=65 if round==0xffffffff else GEOMETRY[round%5]
        first=BASE+window*STRIDE
        E.require(n(row,'first')==first and n(row,'last')==first+(pages-1)*4096 and
                  n(row,'pages')==pages and n(row,'strategy')==(1 if pages<=64 else 2),'TLB normalized strategy/range')
        E.require(n(row,'targets')==n(row,'ack')==targets and not any(n(row,k) for k in
                  ('missing','failed','send_failed','status')),'TLB missing/failed remote completion')
        sent=n(row,'sent');E.require(sent&~targets==0 and (count>1 or sent==0),'TLB notification target set')
        if schema>=2:
            E.require('notifications' in row and n(row,'notifications')==sent.bit_count(),'TLB per-page or duplicate notification round')
            if schema==7:
                E.require(n(row,'timing_schema')==1 and 'irq_ns' not in row,'TLB local cycle schema')
            else:
                E.require(n(row,'irq_ns')<1000000000,'TLB initiator IRQ measurement invalid')
                max_irq=max(max_irq,n(row,'irq_ns'))
        E.require(0<=n(row,'elapsed_ns')<1000000000,'TLB service deadline')
        max_latency=max(max_latency,n(row,'elapsed_ns'));notifications+=sent.bit_count()
        requests[key]=row;generations[generation]=key
    E.require(set(requests)==required,'TLB incomplete request phases')
    service_last = max(generations)
    if schema in (6,7):
        E.require(len(data['AUDIT']) == 1, 'TLB boot audit checkpoint missing')
        audit = data['AUDIT'][0]
        before, after = n(audit, 'before'), n(audit, 'after')
        E.require(n(audit,'invocation')==invocation and n(audit,'root')==root and
                  n(audit,'completed_before')==before and n(audit,'emitted_before')==previous_generation and
                  previous_generation<=before<after, 'TLB audit checkpoint binding')
        E.require([n(r,'generation') for r in data['SERVICE']]==list(range(previous_generation+1,after+1)),
                  'TLB boot audit missing/duplicate/stale generation')
        verify_service_sequence(data['SERVICE'],topology,root,previous_generation+1,after)
        by_generation={n(r,'generation'):r for r in data['SERVICE']}
        for generation, service in by_generation.items():
            E.require(n(service,'root')==root and n(service,'targets')==n(service,'ack')==targets and
                      not any(n(service,k) for k in ('missing','failed','send_failed','status')),
                      'TLB service root/target/completion failure')
            E.require(service['identities']==','.join(f'{slot}:{topology[slot][0]}' for slot in sorted(topology)),
                      'TLB service target identity')
            E.require(n(service,'sent')&~targets==0 and n(service,'notifications')==n(service,'sent').bit_count() and
                      n(service,'elapsed_ns')<1000000000, 'TLB service notification/deadline')
            operation=n(service,'operation'); mapping=n(service,'mapping')
            E.require(mapping in (0,1) and operation in ((1,2,3,4) if WX.enabled() else (1,2,3)), 'TLB service operation/domain')
            if generation in generations:
                request=requests[generations[generation]]
                E.require(before<generation<=after and mapping==0 and
                          operation==(2 if n(request,'phase') in (1,3) else 1), 'TLB workload admission/operation')
                E.require(all(n(request,k)==n(service,k) for k in ('first','last','pages','strategy','targets','ack',
                          'missing','failed','sent','send_failed','notifications','elapsed_ns','status')),
                          'TLB fixture/service records disagree')
            else:
                if WX.enabled() and operation==4:
                    E.require(mapping==0 and generation<=before and all(n(service,k)==0 for k in ('first','last','pages','strategy')),
                        'TLB invalid fixed protection service interval')
                    continue
                E.require(mapping==1 and operation in (1,3), 'TLB unexplained service generation')
                pages=n(service,'pages'); first=n(service,'first'); last=n(service,'last')
                E.require(pages>0 and not first%4096 and last==first+(pages-1)*4096 and
                          n(service,'strategy')==(1 if pages<=64 else 2), 'TLB maintenance normalized interval')
                if operation==3:
                    E.require(first==last==0 and pages==1, 'TLB cache maintenance interval')
        service_last=after
    else:
        E.require(not data['AUDIT'] and not data['SERVICE'], 'TLB historical schema audit mismatch')
        E.require(sorted(generations)==list(range(previous_generation+1,previous_generation+len(required)+1)),
                  'TLB missing/stale global request generation')
    for window,round in expected:
        values=[n(requests[(window,round,phase)],'generation') for phase in range(4)]
        E.require(values==sorted(set(values)),'TLB prime/invalidate/sample ordering')
    cpus={}
    for row in data['CPUS']:
        generation=n(row,'generation');group=n(row,'group')
        E.require(generation in generations and group in (topology if schema>=4 else (0,8,16,24)),'TLB CPU batch identity')
        key=generations[generation];window,round,phase=key
        expected_slots={group} if schema>=4 else {slot for slot in topology if group<=slot<group+8}
        parsed=[]
        for entry in row['entries'].split(','):
            values=entry.split(':');E.require(len(values)==(6 if schema==7 else 8 if schema>=2 else 7),'TLB actual-load record shape')
            E.require(all(re.fullmatch(r'[0-9a-f]+',v) for v in values),'TLB actual-load record numeric')
            if schema==7:
                slot,apic,value,cr3,actual_loads,success=[int(v,16 if i in (2,3) else 10) for i,v in enumerate(values)]
            else:
                slot,apic,value,cr3,actual_loads,elapsed,success=[int(v,16 if i in (2,3) else 10) for i,v in enumerate(values[:7])]
                if schema>=2:
                    handler=int(values[7]);E.require(elapsed<=handler<1000000000,'TLB handler measurement invalid');max_handler=max(max_handler,handler)
            E.require(slot in expected_slots and (generation,slot) not in cpus,'TLB duplicate/missing CPU completion')
            E.require(apic==topology[slot][0] and cr3==root and success==1,'TLB slot/APIC/root/operation mismatch')
            pages=n(requests[key],'pages')
            if phase in (1,3):
                marker=n(replacements[(window,round)],'marker_a' if phase==1 else 'marker_b')
                E.require(value==marker and actual_loads==pages,'TLB stale marker or missing actual load')
            else:E.require(value==actual_loads==0,'TLB invalidation substituted for sampling')
            if schema!=7:
                E.require(elapsed<1000000000,'TLB local measurement invalid')
                max_local=max(max_local,elapsed)
            loads+=actual_loads
            cpus[generation,slot]=values;parsed.append(slot)
        E.require(n(row,'count')==len(parsed) and set(parsed)==expected_slots and parsed==sorted(parsed),
                  'TLB CPU batch completeness/order')
    E.require(set(cpus)=={(g,s) for g in generations for s in topology},'TLB per-target execution evidence missing')
    local_cycles=[]
    if schema==7:
        seen=set()
        for row in data['LOCAL']:
            r=CYCLES.decode(row);key=(r['generation'],r['target'])
            E.require(key in cpus and key not in seen,'TLB cycle target inventory')
            local_cycles.append(CYCLES.validate(row,topology,root,*key,allow_clock_failure=True));seen.add(key)
        E.require(seen==set(cpus),'TLB missing local-cycle target')
    else:E.require(not data['LOCAL'],'TLB historical record has current cycles')
    E.require(len(data['RETIRE'])==len(windows),'TLB retirement coverage')
    retired=set()
    for row in data['RETIRE']:
        window=n(row,'window');E.require(window in windows and window not in retired,'TLB duplicate retirement')
        final=n(requests[(window,0xffffffff,0)],'generation')
        E.require(n(row,'invocation')==invocation and n(row,'generation')==final and
                  all(n(r,'generation')<final for key,r in requests.items() if key[0]==window and key[1]!=0xffffffff) and
                  n(row,'data_frames')==n(row,'released')==2 and row['retained_tables']=='OWNED',
                  'TLB premature or incomplete retirement')
        retired.add(window)
    E.require(len(data['INITIATOR'])==(2 if count>1 else 0),'TLB concurrent initiator coverage')
    if count>1:
        active={n(r,'window'):r for r in data['INITIATOR']}
        E.require(set(active)=={0,1} and all(n(r,'rounds')==64 and n(r,'invocation')==invocation and
                  n(r,'start_ns')<n(r,'end_ns') for r in active.values()),'TLB initiator identity')
        E.require(n(active[0],'start_ns')<n(active[1],'end_ns') and n(active[1],'start_ns')<n(active[0],'end_ns') and
                  sum(n(r,'contention') for r in active.values())>0,'TLB no observed concurrent admission')
    E.require(len(data['DEFERRED'])==(1 if schema>=2 else 0),'TLB deferred proof missing/duplicated')
    E.require(len(data['OBSERVER'])==(3 if schema>=2 else 0),'TLB observer traffic missing/duplicated')
    if schema>=2:
        row=data['DEFERRED'][0]
        E.require(n(row,'invocation')==invocation and n(row,'window')==0 and n(row,'round')==320 and
                  n(row,'token')>0 and n(row,'slot')<8 and n(row,'queued')==0 and n(row,'initial')==1 and
                  n(row,'completed')==2 and n(row,'handle_released')==1 and
                  n(row,'generation')==n(requests[(0,320,2)],'generation') and
                  n(row,'old_marker')==n(replacements[(0,320)],'marker_a'),'TLB queued work mistaken for completed retirement')
        prior=0
        for row in data['OBSERVER']:
            E.require(n(row,'invocation')==invocation and n(row,'generation')>prior and
                      n(row,'targets')==n(row,'complete')==targets and n(row,'errors')==0 and
                      n(row,'status')==1 and n(row,'start_ns')<n(row,'end_ns'),'TLB vector-35 observation incomplete')
            prior=n(row,'generation')
        if count>1:
            E.require(any(n(row,'start_ns')<min(n(r,'end_ns') for r in active.values()) and
                          n(row,'end_ns')>max(n(r,'start_ns') for r in active.values()) for row in data['OBSERVER']),
                      'TLB observer did not overlap request workload')
    E.require(n(end,'invocation')==invocation and end['status']=='PASS' and
              n(end,'replacements')==len(expected) and n(end,'retained')==0,'TLB final result contradicts raw proof')
    retained_tables=4 if count==1 else 7
    if schema>=3:
        E.require(len(data['TABLES'])==1,'TLB scratch accounting missing')
        table=data['TABLES'][0]
        E.require(n(table,'invocation')==invocation and n(table,'after')==retained_tables and
                  n(table,'before')==(0 if invocation==1 else retained_tables) and table['retained']=='OWNED',
                  'TLB scratch topology grows between invocations')
    else:E.require(not data['TABLES'],'TLB scratch schema mismatch')
    return {'status':'PASS','schema':schema,'invocation':invocation,'replacements':len(expected),'requests':len(required),
            'acknowledgements':len(cpus),'notifications':notifications,'actual_loads':loads,
            'generation_first':min(generations),'generation_last':service_last,
            'maximum_request_ns':max_latency,'maximum_local_operation_ns':max_local if schema!=7 else None,
            'maximum_handler_body_ns':max_handler if schema!=7 else None,'maximum_initiator_irq_section_ns':max_irq if schema!=7 else None,
            'local_cycles':dict(records=len(local_cycles),valid=sum(r['status']==1 and r['send_status']==1 for r in local_cycles),
                invalid=sum(r['status'] not in (1,2) or r['send_status'] not in (1,2) for r in local_cycles),
                unavailable=sum(r['status']==2 or r['send_status']==2 for r in local_cycles),unit='cycles',whole_window_acceptance=False) if schema==7 else None,
            'timing_scope':CYCLES.CONTRACT if schema==7 else 'HPET observations include sample overhead; handler body excludes interrupt entry, final publication, EOI and exit; initiator measures the explicit IRQ exclusion',
            'x2apic':bool(actual_x2),'retained_tables':'fixed owned scratch topology'}

def trace_blocks(directory):
    """Read the existing debug-console channel without joining broken records."""
    path=directory/'qemu-debugcon.log'
    E.require(path.is_file(),'TLB debug-console trace missing')
    raw=path.read_bytes()
    if b'[MAPPING][BEGIN]' in raw:
        from device_mapping_evidence import split_traces
        return split_traces(raw)['TLB']
    E.require(len(raw)<=32*1024*1024,'TLB trace exceeds frozen bound')
    blocks=[];start=0
    for match in re.finditer(rb'^\[TLB\]\[END\] [^\n]+\n',raw,re.M):
        block=raw[start:match.end()];start=match.end()
        E.require((block.startswith(b'[TLB][BEGIN] schema=5 ') or block.startswith(b'[TLB][BEGIN] schema=6 ') or block.startswith(b'[TLB][BEGIN] schema=7 ')) and b'\r' not in block,
                  'TLB debug-console framing differs')
        blocks.append(block)
    E.require(start==len(raw) and blocks,'TLB debug-console trace incomplete')
    return blocks

def bind_trace(serial_segment,raw,proof_policy=None):
    E.require(proof_policy in (None,'immutable-tlb-proof-v2'),'unknown TLB proof policy')
    receipts=[line for line in serial_segment if line.startswith('[TLB_TRACE]')]
    E.require(len(receipts)==1,'TLB serial trace receipt missing/duplicated')
    event='COMPLETE' if proof_policy is None else 'CAPTURED'
    match=re.fullmatch(r'\[TLB_TRACE\]\['+event+r'\] invocation=([1-9][0-9]*) channel=debugcon port=0x402 '
        r'records=([1-9][0-9]*) bytes=([1-9][0-9]*) fnv1a=0x([0-9a-f]+) status=PASS',receipts[0])
    E.require(match is not None,'TLB serial trace receipt grammar')
    value=14695981039346656037
    for byte in raw:value=((value^byte)*1099511628211)&0xffffffffffffffff
    E.require(int(match[3])==len(raw) and int(match[2])==raw.count(b'\n') and
              int(match[4],16)==value,'TLB serial/debug-console binding mismatch')
    lines=raw.decode('ascii').splitlines();data=rows(lines)
    E.require(len(data['BEGIN'])==1 and n(data['BEGIN'][0],'invocation')==int(match[1]),
              'TLB trace invocation mismatch')
    return lines

def verify_run(directory):
    policy=module('verify-cpu-control-state');base=policy.verify_run(directory)
    lines=(directory/'qemu-serial.log').read_text().splitlines();topology={}
    for line in lines:
        match=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if match:topology[int(match[1])]=(int(match[2]),match[3])
    modes=[line for line in lines if re.fullmatch(r'\[LAPIC\] System using X2?APIC',line)]
    E.require(len(modes)==1,'TLB actual APIC mode unavailable')
    candidate=json.loads((directory/'candidate.json').read_text())
    local_inputs=[r for r in candidate['build_inputs']['files'] if r['path']=='kernel/src/smp/cpu_request_timing.h']
    E.require(len(local_inputs)<=1,'TLB source timing identity')
    timing_contract=CYCLES.CONTRACT if local_inputs else None
    if local_inputs:E.require(local_inputs[0]['sha256']==E.sha(ROOT/'kernel/src/smp/cpu_request_timing.h'),'TLB current cycle source mismatch')
    observed=policy.observation();previous=0;workloads=[]
    blocks=[] if base['observation'].get('production') else trace_blocks(directory)
    for line in (directory/'framed.jsonl').read_text().splitlines():
        row=json.loads(line)
        if row['payload'] not in ('tlbtest','tlbtest run') or base['observation'].get('production'):continue
        segment=observed.FOUNDATION.transaction_segment(lines,row)
        E.require(len(workloads)<len(blocks),'TLB command has no bound trace')
        segment=bind_trace(segment,blocks[len(workloads)])
        value=verify_tlb(segment,topology,base['root'],'X2APIC' in modes[0],previous,timing_contract=timing_contract)
        if WX.enabled():
            expected=3|((1<<63) if base['policy']['nx'] else 0)
            E.require(all(n(r,'old')&~(0x000ffffffffff000|0x60)==expected for r in rows(segment)['REPLACE']),
                'TLB current data NX capability mismatch')
        previous=value['generation_last'];workloads.append(value)
    result=json.loads((directory/'result.json').read_text())
    launch=json.loads((directory/'launch.json').read_text());argv=launch['argv']
    E.require(argv.count('-debugcon')==1 and argv[argv.index('-debugcon')+1]==
              'file:'+result['runtime_session']+'/r/qemu-debugcon.log' and
              'isa-debugcon.iobase=0x402' in argv,'TLB debug-console launch binding differs')
    if result['profile'].endswith('-xapic'):
        E.require(modes==['[LAPIC] System using XAPIC'],'TLB supplemental actual APIC mode differs')
    if result.get('production'):
        symbols=(directory/'symbols.txt').read_text()
        production_symbols(symbols)
        production_payload((directory/'compiled-strings.txt').read_text())
        payloads={json.loads(line)['payload'] for line in (directory/'framed.jsonl').read_text().splitlines()}
        E.require('tlbtest' in payloads,'TLB production command rejection missing')
        E.require(not workloads,'TLB production diagnostic workload present')
    else:
        E.require(len(workloads)==len(blocks)==2 and all(w['schema']==(7 if timing_contract else 6 if 'origin=owned_BootInfo_framebuffer' in '\n'.join(lines) else 5) for w in workloads),
                  'TLB current repeated workload coverage')
        E.require(workloads[1]['invocation']==workloads[0]['invocation']+1,
                  'TLB repeated fixture invocation identity')
    proof=readback(directory,base['root']) if result['readback'] else None
    return {'status':'PASS','policy':base,'workloads':workloads,'independent_dispatch':proof}

def production_symbols(symbols):
    for name in ('cpu_policy_init_bsp','cpu_policy_confirm_local','lapic_delivery_send',
                 'cpu_request_irq','cpu_request_run','kernel_tlb_invalidate',
                 'kernel_tlb_defer','kernel_tlb_runtime_init','deferred_worker'):
        E.require(re.search(r'\b'+name+r'$',symbols,re.M),'TLB production service/initializer compiled out: '+name)
    for name in ('cmd_tlbtest','tlb_fixture','cpu_request_test_delay','g_delayed_slot',
                 'cmd_cpustate','cmd_ptinspect','cmd_memmap'):
        E.require(name not in symbols,'TLB production test payload present: '+name)

def production_payload(strings):
    for text in ('[TLB]', '[TLB_TRACE]', '[TLB_FAILURE]', 'tlbtest',
                 'context-failure', 'delayed-response'):
        E.require(text not in strings,'TLB production test marker present: '+text)

def failure_records(directory,lines,ends,topology,commands,kind,result):
    O=module('verify-memory-observation');P=module('verify-cpu-control-state')
    P.readiness(lines,topology)
    A=module('verify-assert-list-evidence')
    def failure_rows(group,kind,keys):
        prefix=f'[{group}][{kind}] '
        values=[A.fields(line,prefix,keys.split()) for line in lines if line.startswith(prefix)]
        E.require(all(set(value)==set(keys.split()) for value in values),'failure record strict field set')
        return values
    payload='tlbtest '+kind
    E.require(len(commands)==1 and commands[0]['payload']==payload and commands[0]['sequence']==1 and
              commands[0]['candidate_sha256']==result['candidate_elf'],'failure command/candidate identity')
    row=commands[0];crc=f'{zlib.crc32(payload.encode())&0xffffffff:08x}'
    accept=f'[HARNESS][FRAME] ACCEPT seq=1 crc={crc} len={len(payload)}'
    E.require(lines.count(accept)==1 and lines.count('[HARNESS][BEGIN] seq=1')==1 and
              lines.index(accept)<lines.index('[HARNESS][BEGIN] seq=1'),'failure command framing')
    started=lines.index('[HARNESS][BEGIN] seq=1')
    expected_generation=3
    checkpoints=[line for line in lines if line.startswith('[TLB_FAILURE][CHECKPOINT] ')]
    current=any(r['path']=='kernel/src/memory/device_origin.c' for r in json.loads((directory/'candidate.json').read_text())['build_inputs']['files'])
    E.require(bool(checkpoints)==current,'failure audit source/schema contract')
    if checkpoints:
        import device_mapping_evidence
        expected_generation=device_mapping_evidence.failure_chronology(lines,topology,kind)
    if kind=='delayed-response':
        prof=dict(name=result['profile'],smp='4',vm_id=result['vm_id'],input_profile='framed',
            command_input_observation='qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb')
        errors=O.FOUNDATION.validate_transaction(row,lines,ends,prof,result['candidate_elf'],directory)
        E.require(not errors and row['handler_status']==0,'delayed response transaction: '+'; '.join(errors))
        values=failure_rows('TLB_FAILURE','DELAYED','status generation slot apic expected ack missing result elapsed_ns withheld late_delivery reuse_result frame_a frame_b retained');E.require(len(values)==1,'delayed result cardinality')
        value=values[0];targets=sum(1<<slot for slot in topology)
        E.require(value['status']=='EXPECTED' and n(value,'generation')==expected_generation and n(value,'slot')==1 and
                  n(value,'apic')==topology[1][0] and n(value,'expected')==targets and
                  n(value,'ack')==targets&~2 and n(value,'missing')==2 and n(value,'result')==7 and
                  1000000000<=n(value,'elapsed_ns')<2000000000 and n(value,'withheld')>0 and
                  n(value,'late_delivery')==0 and n(value,'reuse_result')==9 and n(value,'retained')==2 and
                  n(value,'frame_a')!=n(value,'frame_b'),'delayed response ownership/timeout semantics')
        failures=failure_rows('CPU_REQUEST','FAIL','generation operation first last root expected acknowledged missing failed send_failed status');identities=failure_rows('CPU_REQUEST','TARGET_FAILURE','generation slot apic')
        E.require(len(failures)==len(identities)==1 and n(failures[0],'generation')==expected_generation and
                  n(failures[0],'expected')==targets and n(failures[0],'missing')==2 and
                  n(identities[0],'slot')==1 and n(identities[0],'apic')==topology[1][0],
                  'timeout diagnostics missing target identity')
        return value
    A=module('verify-assert-list-evidence')
    index,bug=A.unique_line(lines,'[ASSERT][BUG] ','TLB_CONTEXT_ASSERT_CARDINALITY')
    owner_index,owner=A.unique_line(lines,'[PANIC][OWNER] ','TLB_CONTEXT_OWNER_CARDINALITY')
    required=('file_hex','file_bytes','file_valid','file_truncated','line','expression_hex','expression_bytes',
              'expression_valid','expression_truncated','cpu_id','cpu_valid','cpu_slot','slot_valid',
              'task_id','task_generation','task_valid')
    fields=A.fields(bug,'[ASSERT][BUG] ',required)
    E.require(set(fields)==set(required) and started<owner_index<index,'context assertion strict shape/order')
    for key,text in [('file','kernel/src/smp/cpu_request.c'),('expression','!context')]:
        E.require(fields[key+'_hex']==text.encode().hex() and n(fields,key+'_bytes')==len(text) and
                  n(fields,key+'_valid')==1 and n(fields,key+'_truncated')==0,'context assertion callsite')
    slot=n(fields,'cpu_slot');cpu=n(fields,'cpu_id')
    E.require(slot in topology and topology[slot][0]==cpu and all(n(fields,k)==1 for k in
              ('cpu_valid','slot_valid','task_valid')) and n(fields,'task_id')>0 and n(fields,'task_generation')>0,
              'context assertion runtime identity')
    ownership=A.fields(owner,'[PANIC][OWNER] ',('cpu_id','cpu_slot','origin','depth'))
    E.require(n(ownership,'cpu_id')==cpu and n(ownership,'cpu_slot')==slot and ownership['origin']=='assertion' and
              n(ownership,'depth')==1 and '[PANIC][DUMP_END] status=complete channel=serial' in lines,
              'context assertion panic ownership')
    E.require(not any(l.startswith('[HARNESS][END]') or l.startswith('[CPU_REQUEST]') for l in lines[started:]),
              'invalid context sent a request or returned success')
    E.require(row['handler_status'] is None and row['completed_monotonic']-row['dispatcher_observed_monotonic']<5,
              'context assertion waited for host deadline')
    return {'assertion':fields,'normal_transport_status':row['classification'],
            'scope':'Expected fatal context guard; no successful command END is claimed'}

def readback(directory,root):
    proof=json.loads((directory/'tlb-readback.json').read_text())
    cfg=json.loads((directory/'capture-config.json').read_text())
    E.require(proof['schema']==1 and proof['candidate_id']==cfg['candidate_id'] and
              proof['vm_id']==cfg['vm_id'] and proof['root']==root and proof['vector']==225,
              'TLB debugger candidate/root/vector binding')
    raw=(directory/'page-readback.stdout.log').read_text().splitlines()
    E.require(raw.count('TLB_STATE_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))==1,
              'TLB debugger raw proof missing or contradictory')
    symbols=cfg['symbols'];limit,base=struct.unpack('<HQ',bytes.fromhex(proof['idtr_hex']))
    E.require(limit==4095 and base==symbols['g_idt'],'TLB debugger IDT identity')
    gate=bytes.fromhex(proof['gate_hex'])
    low,selector,ist,attr,mid,high,reserved=struct.unpack('<HHBBHII',gate)
    address=low|(mid<<16)|(high<<32)
    E.require(address==symbols['irq_vector_225']==proof['stub_address'] and
              selector==8 and ist==0 and attr==0x8e and reserved==0 and
              int.from_bytes(bytes.fromhex(proof['stub_pointer_hex']),'little')==address,
              'TLB debugger IDT dispatch mismatch')
    stub=bytes.fromhex(proof['stub_hex'])
    E.require(len(stub)==10 and stub[:5]==b'\x68\xe1\x00\x00\x00' and stub[5]==0xe9 and
              address+10+struct.unpack('<i',stub[6:])[0]==symbols['irq_external_common_entry']==proof['common_entry'] and
              proof['dispatcher']==symbols['irq_external_dispatch'],'TLB debugger stub ABI mismatch')
    lines=[line for block in trace_blocks(directory) for line in block.decode('ascii').splitlines()]
    expected=sorted({int(dict(pair.split('=',1) for pair in line.split()[1:])['pte'],16)
                     for line in expand_trace(lines) if line.startswith('[TLB][REPLACE] ')})
    E.require(len(expected)==2 and cfg['tlb_readback']=={'vector':225,'fixture_entries':expected} and
              [row['address'] for row in proof['fixture']]==expected,'TLB debugger fixture identity')
    meta=json.loads((directory/'table-snapshot.json').read_text());data=(directory/'table-frames.bin').read_bytes()
    for row in proof['fixture']:
        table=row['address']&~4095;offset=row['address']&4095
        E.require(table in meta['frame_addresses'],'TLB debugger unregistered fixture table')
        index=meta['frame_addresses'].index(table)*4096+offset
        E.require(row['bytes_hex']=='0000000000000000' and data[index:index+8].hex()==row['bytes_hex'],
                  'TLB debugger retired fixture entry differs')
    return {'status':'PASS','vector':225,'stub':address,'fixture_entries':expected,
            'scope':'Stopped RAM/IDT identity proof; not a measurement of TLB contents'}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();value=verify_run(args.run)
    if args.output:E.write(args.output,value)
    print(json.dumps(value,sort_keys=True))
if __name__=='__main__':main()
