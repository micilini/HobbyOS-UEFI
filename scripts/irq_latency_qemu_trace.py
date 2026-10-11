"""Diagnostic HPET-value joins to QEMU userspace events; never timing authority."""
import json
import hashlib
import lzma
import re
import shutil
import threading
import time
from collections import Counter, deque
import memory_evidence as E
import irq_latency_authority as A
import irq_latency_scheduling as S
import irq_latency_evidence as V

CONTRACT = 'qemu-hpet-read-correlation-v1'
COUNTER_CONTRACT = 'qemu-hpet-counter-correlation-v2'
EVENTS = ('memory_region_ops_read',)
COUNTER_EVENTS = (*EVENTS, 'kvm_vcpu_ioctl', 'kvm_run_exit')
MAX_BYTES = 512 * 1024**2
RESERVE = 4 * 1024**3 + 64 * 1024**2


def declaration(method=CONTRACT):
    E.require(method in (CONTRACT, COUNTER_CONTRACT), 'unknown QEMU trace method')
    return dict(method=method, events=list(COUNTER_EVENTS if method==COUNTER_CONTRACT else EVENTS), max_bytes=MAX_BYTES,
                max_seconds=90, phase='whole-stress' if method==COUNTER_CONTRACT else 'after-mapping-completed-192', clock='QEMU_LOG_REALTIME_US')


def contract(plan):
    value = plan.get('qemu_exit_trace')
    if value is None:
        return None
    method=value.get('method')
    E.require(value == declaration(method) and plan['role'] == 'DIAGNOSTIC_NO_ACCEPTANCE' and
              plan['boots'] == 1 and plan['retries'] == 0 and plan['profile'] ==
              dict(machine='q35', accel='kvm', smp=8, memory='2G', placement='default') and
              plan.get('ram_backend') is None, 'QEMU trace diagnostic plan/role')
    if method==COUNTER_CONTRACT:
        E.require(plan.get('tick_body_contract')=='lapic-handler-body-v4' and
                  plan.get('request_timing_contract')=='request-local-cycles-v1' and
                  'writeback_clock_contract' not in plan, 'current QEMU counter source contract')
    else:
        E.require(plan.get('writeback_clock_contract')=='continuous-mask-writeback-clock-v1' and
                  'request_timing_contract' not in plan, 'historical QEMU clock source contract')
    return value


def launch(plan, row):
    E.require(contract(plan), 'QEMU trace declaration missing')
    E.require(A.option(row['argv'], '-msg') == 'timestamp=on' and
              A.option(row['argv'], '-machine') == 'q35' and
              A.option(row['argv'], '-m') == '2G' and '-trace' not in row['argv'] and
              A.option(row['argv'], '-d') == 'guest_errors', 'QEMU trace launch differs')


def anchor():
    before = time.monotonic_ns()
    wall = time.time_ns()
    return dict(monotonic_before_ns=before, realtime_ns=wall,
                monotonic_after_ns=time.monotonic_ns())


def mapping_trigger(line):
    raw=V.records(line+'\n')
    E.require(len(raw['MAPPING'])==1 and raw['MAPPING'][0]['status']=='PASS' and
              V.number(raw['MAPPING'][0],'completed')==V.number(raw['MAPPING'][0],'required')==192 and
              V.number(raw['MAPPING'][0],'backing_released')==1, 'trace mapping trigger failed')
    return raw['MAPPING'][0]


def control(runtime, plan, index, label, enabled, client_type):
    """Use the established raw-transcript QMP adapter, with no guest input."""
    row = dict(begin=anchor(), enabled=enabled, candidate_id=plan['candidate_id'],
               vm_id=plan['vm_ids'][index], states={}, label=label)
    transcript = runtime / ('exit-trace-' + label + '.jsonl')
    client = None
    try:
        client = client_type(runtime / 'qmp.sock', transcript, time.monotonic() + 5)
        def call(name, args):
            return client.command(name, args, time.monotonic() + 5)[1]['return']
        row['uuid'] = call('query-uuid', {})
        row['cpus'] = call('query-cpus-fast', {})
        E.require(row['uuid'] == {'UUID': row['vm_id']}, 'QEMU trace wrong VM')
        for event in plan['qemu_exit_trace']['events']:
            prior = call('trace-event-get-state', dict(name=event))
            if label == 'enable':
                E.require(prior == [dict(name=event, state='disabled', vcpu=False)], 'trace unexpectedly enabled before stress')
            call('trace-event-set-state', dict(name=event, enable=enabled, **{'ignore-unavailable': False}))
            row['states'][event] = call('trace-event-get-state', dict(name=event))
            E.require(row['states'][event] == [dict(name=event, state='enabled' if enabled else 'disabled', vcpu=False)],
                      'QEMU trace state not verified')
        row['status'] = 'PASS'
    except Exception as error:
        row.update(status='UNAVAILABLE', reason=str(error))
        raise
    finally:
        if client is not None:
            client.socket.close()
        row['end'] = anchor()
        E.write(runtime / ('exit-trace-' + label + '.json'), row)
    return row


class Window:
    """Bound optional tracing independently of the unchanged guest workload."""
    def __init__(self, runtime, plan, index, client_type):
        self.runtime, self.plan, self.index, self.client_type = runtime, plan, index, client_type
        self.done = threading.Event()
        self.thread = None
        self.error = None
        self.limit = None
        self.samples = []

    def begin(self):
        contract(self.plan)
        E.require(shutil.disk_usage(self.runtime).free > RESERVE + MAX_BYTES + 128 * 1024**2,
                  'BLOCKED_STORAGE QEMU trace provision')
        self.started = None
        self.armed = anchor()
        self.serial_offset = (self.runtime / 'qemu-serial.log').stat().st_size
        if self.plan['qemu_exit_trace']['method']==COUNTER_CONTRACT:
            self.started=control(self.runtime,self.plan,self.index,'enable',True,self.client_type)
        self.thread = threading.Thread(target=self.watch, name='owned-qemu-trace-bound', daemon=False)
        self.thread.start()

    def watch(self):
        try:
            buffer = ''
            with (self.runtime / 'qemu-serial.log').open() as serial:
                serial.seek(self.serial_offset)
                while not self.done.wait(.1):
                    point = anchor()
                    point['bytes'] = (self.runtime / 'qemu-trace.log').stat().st_size
                    point['free_bytes'] = shutil.disk_usage(self.runtime).free
                    self.samples.append(point)
                    if self.started is None:
                        buffer += serial.read()
                        lines = buffer.split('\n'); buffer = lines.pop()
                        E.require(len(buffer) < 65536, 'trace trigger line bound')
                        for line in lines:
                            if line.startswith('[IRQ_LATENCY][MAPPING] '):
                                mapping_trigger(line)
                                self.trigger = dict(line=line, observed=anchor())
                                E.write(self.runtime/'exit-trace-trigger.json',self.trigger)
                                self.started = control(self.runtime,self.plan,self.index,'enable',True,self.client_type)
                                break
                    elapsed = point['monotonic_after_ns'] - self.armed['monotonic_after_ns']
                    if point['bytes'] >= MAX_BYTES or point['free_bytes'] <= RESERVE or elapsed >= 90 * 10**9:
                        self.limit = dict(reason='DECLARED_TRACE_LIMIT', observation=point)
                        control(self.runtime,self.plan,self.index,'limit-disable',False,self.client_type)
                        return
        except Exception as error:
            self.error = str(error)
            try:
                control(self.runtime,self.plan,self.index,'error-disable',False,self.client_type)
            except Exception as stop_error:
                self.error += '; trace disable unavailable: ' + str(stop_error)

    def finish(self):
        self.done.set()
        if self.thread is not None:
            self.thread.join(6)
            E.require(not self.thread.is_alive(), 'QEMU trace watcher did not retire')
        stopped = control(self.runtime, self.plan, self.index, 'disable', False, self.client_type)
        row = dict(method=self.plan['qemu_exit_trace']['method'], candidate_id=self.plan['candidate_id'], vm_id=self.plan['vm_ids'][self.index],
                   begin=self.started, end=stopped, armed=self.armed, trigger=getattr(self,'trigger',None), samples=self.samples, limit=self.limit, error=self.error,
                   status='CAPTURED' if self.started and not self.limit and not self.error else 'UNAVAILABLE',
                   loss_counter='UNAVAILABLE_QEMU_LOG_HAS_NO_LOSS_COUNTER', timing_authority=False)
        E.write(self.runtime / 'exit-trace-window.json', row)
        return row


LINE = re.compile(r'^(\d+)@(\d+)\.(\d{6}):(' + '|'.join(COUNTER_EVENTS) + r') (.*)$')
READ = re.compile(r"^cpu (-?\d+) mr (0x[0-9a-f]+) addr 0x([0-9a-f]+) value 0x([0-9a-f]+) size (\d+) name '([^']*)'$")
IOCTL = re.compile(r'^cpu_index (\d+), type 0x([0-9a-f]+), arg (?:\(nil\)|0x[0-9a-f]+)$')
EXIT = re.compile(r'^cpu_index (\d+), reason (\d+)$')


def correlate(lines, clocks, tids, lower_ns, upper_ns, run_events=False, partial_window=False):
    """Join only complete exact HPET triples; no guest/host time interpolation."""
    wanted = {(r['cpu'],r['ticks']):r for r in clocks}
    E.require(len(wanted)==len(clocks), 'QEMU trace ambiguous guest clock values')
    selected={r['cpu'] for r in clocks}
    E.require((selected<=set(tids) if run_events else selected==set(tids)) and selected and
              len(set(tids.values()))==len(tids), 'trace CPU/TID inventory')
    recent, last, counts, matches = {}, {}, Counter(), {}
    enters, exits = {}, {}
    for number,line in enumerate(lines,1):
        E.require(line.endswith('\n'), 'QEMU trace incomplete last line')
        match=LINE.fullmatch(line.rstrip('\n'))
        if not match:
            E.require(not any(event in line for event in (*EVENTS,'kvm_run_exit','kvm_vcpu_ioctl')), 'QEMU trace malformed or undeclared event')
            counts['other_lines']+=1
            continue
        tid,seconds,micros,event,fields=match.groups()
        stamp=int(seconds)*10**9+int(micros)*1000;tid=int(tid)
        E.require(run_events or event in EVENTS, 'undeclared QEMU run event')
        m=(READ if event in EVENTS else IOCTL if event=='kvm_vcpu_ioctl' else EXIT).fullmatch(fields)
        E.require(m is not None, 'QEMU event fields');cpu=int(m[1])
        if cpu<0:
            counts['non_vcpu_reads']+=1
            continue
        E.require(cpu in tids and tids[cpu]==tid, 'QEMU trace vCPU/TID changed')
        E.require(stamp>=last.get(cpu,0), 'QEMU trace per-thread wall clock reversed')
        last[cpu]=stamp;counts[event]+=1
        if event=='kvm_vcpu_ioctl':
            if int(m[2],16)==0xae80:
                enters[cpu]=dict(line=number,time_ns=stamp)
                exits.pop(cpu,None)
            continue
        if event=='kvm_run_exit':
            exits[cpu]=dict(line=number,time_ns=stamp,reason=int(m[2]),enter=enters.pop(cpu,None))
            continue
        item=dict(line=number,time_ns=stamp,tid=tid,cpu=cpu,address=int(m[3],16),value=int(m[4],16),size=int(m[5]),region=m[6],mr=m[2])
        if run_events:item['run_exit']=exits.pop(cpu,None)
        if item['region']!='hpet':continue
        E.require(item['size']==4 and item['address'] in (0xfed000f0,0xfed000f4), 'unexpected traced HPET access')
        queue=recent.setdefault(cpu,deque(maxlen=3));queue.append(item)
        if len(queue)!=3:continue
        a,b,c=queue
        if [x['address'] for x in queue]!=[0xfed000f4,0xfed000f0,0xfed000f4] or a['value']!=c['value']:continue
        # QEMU logs the device callback's uint64_t return before access-width
        # masking; the guest receives the requested 32-bit value.
        E.require(a['value']<=0xffffffff and c['value']<=0xffffffff and b['value']<=V.U64, 'HPET callback width overflow')
        ticks=(a['value']<<32)|(b['value']&0xffffffff);key=(cpu,ticks)
        if key not in wanted:continue
        E.require(key not in matches, 'ambiguous reused HPET value')
        guest=wanted[key]
        E.require(guest['mode']==1 and guest['retries']==0, 'clock-call retry correlation unsupported')
        inside=lower_ns<=a['time_ns']<=c['time_ns']<=upper_ns
        if partial_window and not inside:
            counts['matching_sample_outside_declared_prefix']+=1
            continue
        E.require(inside, 'clock-call match crosses trace control boundary')
        E.require(len({x['mr'] for x in queue})==1, 'HPET triple domain changed')
        matches[key]=dict(cpu=cpu,site=guest['site'],ticks=ticks,guest=guest,events=list(queue),
            read_gaps_ns=[b['time_ns']-a['time_ns'],c['time_ns']-b['time_ns']],
            first_to_last_read_ns=c['time_ns']-a['time_ns'],timestamp_quantization_ns=1000,timing_authority=False,
            duration_authority='WALLCLOCK_DIAGNOSTIC_NOT_A_MINIMUM_PROOF')
        if run_events:
            regions=[]
            for previous,current in ((a,b),(b,c)):
                exit_row=current['run_exit'];enter=exit_row.get('enter') if exit_row else None
                complete=bool(exit_row and enter and exit_row['reason']==6 and
                    previous['time_ns']<=enter['time_ns']<=exit_row['time_ns']<=current['time_ns'])
                regions.append(dict(status='MATCHED' if complete else 'UNAVAILABLE',
                    after_read_to_run_ns=enter['time_ns']-previous['time_ns'] if complete else None,
                    run_and_postrun_ns=exit_row['time_ns']-enter['time_ns'] if complete else None,
                    exit_to_read_ns=current['time_ns']-exit_row['time_ns'] if complete else None))
            matches[key]['callback_gap_regions']=regions
    rows=sorted(matches.values(),key=lambda r:(r['cpu'],r['site']))
    missing=[dict(cpu=r['cpu'],site=r['site'],ticks=r['ticks']) for key,r in wanted.items() if key not in matches]
    return dict(method=CONTRACT,status='MATCHED' if not missing else 'PARTIAL',rows=rows,missing=missing,event_counts=dict(counts),
        timing_authority=False,implementation_acceptance=False,global_loss_accounting='UNAVAILABLE_QEMU_LOG_HAS_NO_LOSS_COUNTER',
        unique_host_cause='NOT_ESTABLISHED',tracing_perturbs_elapsed_time=True)


def verify(plan, launched, runtime, clocks, hosts):
    launch(plan, launched)
    A.verify_profile_series(hosts, launched, plan['role'], plan['profile']['smp'])
    S.verify(hosts)
    window = json.loads((runtime/'exit-trace-window.json').read_text())
    method=plan['qemu_exit_trace']['method']
    E.require(window['method']==method and window['candidate_id']==plan['candidate_id'] and
              window['vm_id']==A.option(launched['argv'],'-uuid') and window['status']=='CAPTURED' and
              window['limit'] is None and window['error'] is None, 'trace failed/incomplete window')
    E.require(all(set(window[k]['states'])==set(plan['qemu_exit_trace']['events']) for k in ('begin','end')),
              'trace observed event set differs from plan')
    E.require(0<(runtime/'qemu-trace.log').stat().st_size<MAX_BYTES, 'trace empty/overflow')
    mappings = [{c['cpu-index']:c['thread-id'] for c in h['qmp_identity']['cpus']} for h in hosts]
    E.require(mappings and all(m==mappings[0] for m in mappings), 'trace host thread lifetime mapping')
    import irq_latency_ram_backend as RAM
    topology=RAM.guest_inventory((runtime/'qemu-serial.log').read_text())['cpus']
    E.require(all(int(r['slot'])==int(r['apic']) for r in topology) and
              all(c['props']=={'core-id':c['cpu-index'],'socket-id':0,'thread-id':0} and c['target']=='x86_64'
                  for h in hosts for c in h['qmp_identity']['cpus']), 'trace guest APIC/QEMU CPU topology binding')
    verify_controls(runtime,window,plan['candidate_id'],window['vm_id'],mappings)
    lo=window['begin']['end']['realtime_ns'];hi=window['end']['begin']['realtime_ns']
    anchors=sorted([window['armed'],window['begin']['begin'],window['begin']['end'],*window['samples'],window['end']['begin'],window['end']['end']],key=lambda a:a['monotonic_before_ns'])
    if method==COUNTER_CONTRACT:
        verify_stress_window(runtime,window)
    else:
        trigger=json.loads((runtime/'exit-trace-trigger.json').read_text())
        E.require(trigger==window['trigger'] and trigger['observed']['monotonic_after_ns']<=window['begin']['begin']['monotonic_before_ns'], 'trace trigger identity/order')
        serial=(runtime/'qemu-serial.log').read_text().splitlines()
        E.require(serial.count(trigger['line'])==1 and trigger['line'].startswith('[IRQ_LATENCY][MAPPING] '), 'trace trigger absent from actual guest')
        mapping_trigger(trigger['line'])
    E.require(len(anchors)>4 and all(a['monotonic_before_ns']<=a['monotonic_after_ns'] for a in anchors), 'trace clock anchors unavailable')
    E.require(all(a['monotonic_after_ns']<=b['monotonic_before_ns'] and a['realtime_ns']<=b['realtime_ns'] for a,b in zip(anchors,anchors[1:])),
              'trace anchor clock reversed')
    with (runtime/'qemu-trace.log').open() as stream:
        result=correlate(stream,clocks,mappings[0],lo,hi,run_events=method==COUNTER_CONTRACT)
    result['method']=method
    offsets=[a['realtime_ns']-(a['monotonic_before_ns']+a['monotonic_after_ns'])//2 for a in anchors]
    result['observed_anchor_offset_range_ns']=max(offsets)-min(offsets)
    result['clock_limit']='Microsecond realtime markers; anchors do not rule out every intervening clock adjustment.'
    return result


def counter_clocks(tick):
    """Input is the independently reconstructed current raw tick result."""
    E.require(tick['status']=='PASS' and tick['contract']=='lapic-handler-body-v4' and
              len(tick['rows'])==8 and {r['cpu'] for r in tick['rows']}==set(range(8)),
              'current QEMU counter witness inventory')
    result=[]
    for r in tick['rows']:
        w=r['witness']
        if r.get('count')==0:
            E.require(w is None and r['raw_cycles']==0 and r['complete'] and not r['invalid'] and
                      r['authority']=='UNAVAILABLE', 'erased QEMU counter witness')
            continue
        E.require(r['authority']=='DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY' and w is not None and
                  r['epoch_qualified'] and r['complete'] and not r['invalid'], 'unqualified QEMU guest counter')
        q=w['sample']
        E.require(V.number(q,'cpu')==r['cpu'] and V.number(q,'valid')==1 and
                  V.number(q,'outer_sequence')==w['outer_sequence'] and V.number(q,'source')==1 and V.number(q,'source_epoch')==1, 'QEMU sample/outer/HPET identity')
        result.append(dict(cpu=r['cpu'],site=0,ticks=V.number(q,'ticks'),mode=V.number(q,'mode'),
            retries=V.number(q,'retries'),outer_sequence=w['outer_sequence'],
            begin=w['sample_begin'],end=w['counter_end'],raw_cycles=w['counter_end']-w['sample_begin'],
            minimum_ns=(w['counter_end']-w['sample_begin'])*1000000000//r['frequency_upper'],
            frequency_upper=r['frequency_upper'],authority=r['authority']))
    return result


def verify_stress_window(runtime,window):
    E.require(window['trigger'] is None, 'current trace selected historical trigger')
    rows=[json.loads(line) for line in (runtime/'driver.jsonl').read_text().splitlines()]
    begin=[r for r in rows if r.get('command')=='irqlatency stress' and r.get('event')=='HMP_INPUT_BEGIN']
    E.require(len(begin)==1, 'trace stress command identity')
    marker='[HARNESS][END] seq='+str(begin[0]['sequence'])+' '
    end=[r for r in rows if r.get('marker')==marker]
    E.require(len(end)==1 and window['begin']['end']['monotonic_after_ns']<=begin[0]['monotonic_ns']<
              end[0]['monotonic_ns']<=window['end']['begin']['monotonic_before_ns'], 'trace stress window incomplete')


def verify_controls(runtime,window,candidate_id,vm_id,mappings,end_label='disable'):
    E.require(end_label in ('disable','limit-disable'), 'trace control stop label')
    for label, enabled in (('enable',True),(end_label,False)):
        row=window['begin' if enabled else 'end']
        E.require(row==json.loads((runtime/('exit-trace-'+label+'.json')).read_text()) and row['status']=='PASS' and row['enabled'] is enabled and
                  row['vm_id']==vm_id and row['candidate_id']==candidate_id and
                  {c['cpu-index']:c['thread-id'] for c in row['cpus']}==mappings[0], 'trace control identity')
        transcript=[json.loads(line) for line in (runtime/('exit-trace-'+label+'.jsonl')).read_text().splitlines()]
        sent=[r['message'] for r in transcript if r['direction']=='send']
        replies=[r['message'] for r in transcript if r['direction']=='receive' and 'id' in r['message']]
        E.require(len(sent)==len(replies) and len({r['id'] for r in sent})==len(sent), 'trace QMP missing/duplicate reply')
        reply={r['id']:r for r in replies}
        for request in sent:
            E.require(request['id'] in reply and 'return' in reply[request['id']], 'trace QMP request failed')
        for name,expected in (('query-uuid',{'UUID':vm_id}),('query-cpus-fast',row['cpus'])):
            requests=[r for r in sent if r['execute']==name]
            E.require(len(requests)==1 and reply[requests[0]['id']]['return']==expected, 'trace raw QMP identity claim')
        events=list(row['states'])
        E.require(set(events) in (set(EVENTS),set(COUNTER_EVENTS)), 'trace control unknown events')
        events=list(COUNTER_EVENTS if len(events)==3 else EVENTS)
        E.require([r['execute'] for r in sent]==['qmp_capabilities','query-uuid','query-cpus-fast']+
                  [name for _ in events for name in ('trace-event-get-state','trace-event-set-state','trace-event-get-state')],
                  'trace QMP command plan differs')
        sets=[r for r in sent if r['execute']=='trace-event-set-state']
        E.require([r['arguments'] for r in sets]==[dict(name=e,enable=enabled,**{'ignore-unavailable':False}) for e in events],
                  'trace QMP altered event set')
        for event in events:
            queries=[r for r in sent if r['execute']=='trace-event-get-state' and r['arguments']==dict(name=event)]
            E.require(len(queries)==2 and reply[queries[-1]['id']]['return']==row['states'][event]==
                      [dict(name=event,state='enabled' if enabled else 'disabled',vcpu=False)], 'trace QMP state claim')
            if enabled:
                E.require(reply[queries[0]['id']]['return']==[dict(name=event,state='disabled',vcpu=False)], 'trace already active before declared window')


def verify_counter_prefix(plan,launched,runtime,tick,hosts):
    """A bounded diagnostic subset; never repairs failed full-window capture."""
    E.require(contract(plan)['method']==COUNTER_CONTRACT, 'current counter prefix method')
    E.require(launched['elf']['sha256']==plan['elf_sha256'] and launched['image']['sha256']==plan['image_sha256'] and
              A.option(launched['argv'],'-uuid') in plan['vm_ids'] and not launched['fault_trace'], 'counter prefix candidate/boot')
    launch(plan,launched)
    A.verify_profile_series(hosts,launched,plan['role'],plan['profile']['smp']);S.verify(hosts)
    window=json.loads((runtime/'exit-trace-window.json').read_text())
    limit=window.get('limit')
    E.require(window['method']==COUNTER_CONTRACT and window['status']=='UNAVAILABLE' and window['error'] is None and
              limit and limit['reason']=='DECLARED_TRACE_LIMIT' and
              window['candidate_id']==plan['candidate_id'] and window['vm_id']==A.option(launched['argv'],'-uuid'),
              'counter prefix requires actual declared bound, not a failed or invented trace')
    point=limit['observation']
    E.require(point in window['samples'] and (point['bytes']>=MAX_BYTES or point['free_bytes']<=RESERVE or
              point['monotonic_after_ns']-window['armed']['monotonic_after_ns']>=90*10**9), 'trace limit not evidenced')
    maps=[{c['cpu-index']:c['thread-id'] for c in h['qmp_identity']['cpus']} for h in hosts]
    E.require(maps and set(maps[0])==set(range(8)) and all(m==maps[0] for m in maps), 'counter prefix thread inventory')
    import irq_latency_ram_backend as RAM
    topology=RAM.guest_inventory((runtime/'qemu-serial.log').read_text())['cpus']
    E.require(all(int(r['slot'])==int(r['apic']) for r in topology) and
              all(c['props']=={'core-id':c['cpu-index'],'socket-id':0,'thread-id':0} and c['target']=='x86_64'
                  for h in hosts for c in h['qmp_identity']['cpus']), 'counter prefix guest topology')
    stopped=json.loads((runtime/'exit-trace-limit-disable.json').read_text())
    effective=dict(window,end=stopped)
    E.require(all(set(r['states'])==set(COUNTER_EVENTS) for r in (window['begin'],window['end'],stopped)), 'counter prefix event set')
    verify_controls(runtime,window,plan['candidate_id'],window['vm_id'],maps)
    verify_controls(runtime,effective,plan['candidate_id'],window['vm_id'],maps,end_label='limit-disable')
    verify_stress_window(runtime,window)
    E.require(window['begin']['end']['monotonic_after_ns']<point['monotonic_before_ns']<=
              point['monotonic_after_ns']<=stopped['begin']['monotonic_before_ns']<=
              stopped['end']['monotonic_after_ns']<=window['end']['begin']['monotonic_before_ns'], 'counter prefix stop ordering')
    anchors=sorted([window['armed'],window['begin']['begin'],window['begin']['end'],*window['samples'],
                    stopped['begin'],stopped['end'],window['end']['begin'],window['end']['end']],key=lambda a:a['monotonic_before_ns'])
    E.require(all(a['monotonic_before_ns']<=a['monotonic_after_ns'] for a in anchors) and
              all(a['monotonic_after_ns']<=b['monotonic_before_ns'] and a['realtime_ns']<=b['realtime_ns']
                  for a,b in zip(anchors,anchors[1:])), 'counter prefix clock anchors')
    trace=next(r for r in json.loads((runtime/'seal.json').read_text())['files'] if r['path']=='qemu-trace.log')
    result=correlate(counter_trace_lines(runtime,trace),counter_clocks(tick),maps[0],
                     window['begin']['end']['realtime_ns'],point['realtime_ns'],run_events=True,partial_window=True)
    result.update(method=COUNTER_CONTRACT,matching_status=result['status'],status='BOUNDED_PREFIX_ONLY',
        complete_capture=False,original_capture_status=window['status'],limit=limit,
        guest_inventory=[{k:r[k] for k in ('cpu','count','invalid','authority','epoch_qualified')} for r in tick['rows']],
        candidate_id=plan['candidate_id'],vm_id=window['vm_id'],trace_sha256=trace['sha256'])
    return result


def counter_trace_lines(runtime,sealed):
    """Stream an exact sealed trace; compressed storage never changes raw facts."""
    raw=runtime/'qemu-trace.log'
    if raw.exists():
        E.require(raw.stat().st_size==sealed['bytes'], 'sealed counter trace size')
        stream=raw.open('rb')
    else:
        info=json.loads((runtime/'trace-representation.json').read_text());encoded=runtime/'qemu-trace.log.xz'
        E.require(info['raw_bytes']==sealed['bytes'] and info['raw_sha256']==sealed['sha256'] and
                  encoded.stat().st_size==info['encoded_bytes'] and E.sha(encoded)==info['encoded_sha256'],
                  'compressed counter trace identity')
        stream=lzma.open(encoded,'rb')
    digest=hashlib.sha256();length=0
    with stream:
        for line in stream:
            length+=len(line);E.require(length<=sealed['bytes'], 'counter trace expansion bound')
            digest.update(line);yield line.decode('utf-8')
    E.require(length==sealed['bytes'] and digest.hexdigest()==sealed['sha256'], 'sealed counter trace bytes')
