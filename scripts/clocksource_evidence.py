"""Reconstruct the source-selected production provider; never grant IRQ acceptance."""
import re
import irq_latency_evidence as V

CONTRACT='qualified-kvm-clocksource-v3'
MARKERS=('CLOCKSOURCE','CLOCKSOURCE_CPU','CLOCKSOURCE_POINT')

def observations(raw,selection):
    cpus=len(selection['cpus']);n=V.number
    clock=V.index(raw['CLOCK_BODY'],('cpu',));tick=V.index(raw['TICK_BODY'],('cpu',));samples=V.index(raw['TICK_SAMPLE'],('cpu',))
    V.require(set(clock)==set(tick)==set(samples)=={(c,) for c in range(cpus)},'clocksource observation CPU coverage')
    for key,r in clock.items():
        if n(r,'count'):
            V.require(n(r,'phase') in ((4,) if selection['source']==2 else (1,2,3)),
                      'clock body provider differs from selected source')
        if n(tick[key],'count'):
            s=samples[key]
            V.require((n(s,'source'),n(s,'source_epoch'))==(selection['source'],selection['epoch']),
                      'tick raw counter provider differs from selected source')
    return dict(status='PASS',source=selection['source'],epoch=selection['epoch'],cpus=cpus)

def signed(row,key):
    value=row[key]
    V.require(re.fullmatch(r'0|-?[1-9][0-9]*',value),'clocksource signed integer')
    result=int(value);V.require(-(1<<63)<=result<(1<<63),'clocksource signed overflow')
    return result

def point(row):
    n=V.number
    for side in ('0','1'):
        V.require(0<n(row,'ver'+side)<1<<32 and n(row,'ver'+side)%2==0 and
            0<n(row,'mul'+side)<1<<32 and -64<signed(row,'shift'+side)<64 and
            n(row,'flags'+side)==1 and n(row,'retry'+side)<4,'clocksource snapshot version/scale')
    V.require(n(row,'mul0')==n(row,'mul1') and signed(row,'shift0')==signed(row,'shift1') and
        0<n(row,'tsc0')<n(row,'tsc1') and 0<n(row,'ns0')<n(row,'ns1')<(1<<63),'clocksource reference bracket')
    period=n(row,'period');ticks=n(row,'ticks');ns=n(row,'ns')
    V.require(0<period<=100000000 and ticks>0 and n(row,'valid')==1 and
        ((n(row,'mode')==1 and n(row,'retries')<=4) or (n(row,'mode')==2 and n(row,'retries')==0)),
        'clocksource independent HPET sample')
    V.require(0<ns<(1<<63) and ns==ticks*1000000000//(10**15//period),'clocksource HPET conversion')
    tick=(period+999999)//1000000
    return ns-n(row,'ns1')-tick,ns-n(row,'ns0')+tick

def offset_at(row,origin):
    lo,hi=point(row)
    V.require(0<origin<(1<<63),'clocksource common reference epoch')
    reserve=(abs(V.number(row,'ns')-origin)+99)//100
    V.require(-(1<<63)<=lo-reserve<=hi+reserve<(1<<63),'clocksource projected offset overflow')
    return lo-reserve,hi+reserve

def drift(a,b,bootstrap=False):
    point(a);point(b);n=V.number;delta=n(b,'ns')-n(a,'ns');uncertainty=2*((n(a,'period')+999999)//1000000)
    V.require(a['period']==b['period'] and a['mode']==b['mode'] and a['mul0']==b['mul0'] and a['shift0']==b['shift0'] and
        n(a,'ticks')<n(b,'ticks') and n(a,'tsc1')<n(b,'tsc0') and n(a,'ns1')<n(b,'ns0') and delta>uncertainty and
        (n(b,'ns0')-n(a,'ns1'))*1000000>=(delta+uncertainty)*990000 and
        (n(b,'ns1')-n(a,'ns0'))*1000000<=(delta-uncertainty)*1010000,
        'clocksource independent rate agreement')
    if bootstrap:V.require(10000000<=delta<=100000000 and all(n(p,'ns1')-n(p,'ns0')<=100000 for p in (a,b)),
                           'clocksource qualification duration')

def boot(serial,cpus,expected=None):
    rows={m:[] for m in MARKERS}
    for line in serial.splitlines():
        if not line.startswith('[CLOCKSOURCE'):continue
        match=re.fullmatch(r'\[([A-Z_]+)\] (.+)',line)
        V.require(match and match[1] in rows,'clocksource boot marker')
        pairs=[p.split('=',1) for p in match[2].split()]
        V.require(all(len(p)==2 for p in pairs),'clocksource boot syntax')
        row=dict(pairs);V.require(len(row)==len(pairs) and V.number(row,'version')==3,'clocksource boot schema')
        rows[match[1]].append(row)
    V.require(len(rows['CLOCKSOURCE'])==1,'clocksource selection missing/duplicate')
    selected=rows['CLOCKSOURCE'][0];source=V.number(selected,'source');epoch=V.number(selected,'epoch')
    V.require((source,epoch) in ((1,1),(2,2)) and V.number(selected,'cpus')==cpus and
        V.number(selected,'reference_ns')==1000000000 and V.number(selected,'rate_ppm')==10000,'clocksource selected identity')
    origin=V.number(selected,'origin_ns')
    bsp=V.number(selected,'bsp');V.require(bsp<cpus,'clocksource BSP slot')
    if expected is not None:V.require(source==expected,'clocksource required provider unavailable')
    cpus_raw=V.index(rows['CLOCKSOURCE_CPU'],('cpu',));points=V.index(rows['CLOCKSOURCE_POINT'],('cpu','index'))
    V.require(set(cpus_raw)=={(c,) for c in range(cpus)},'clocksource all-CPU preparation')
    V.require(set(points)=={(c,i) for (c,),r in cpus_raw.items() for i in range(V.number(r,'points'))},
              'clocksource raw point dispositions')
    result=[];physical=set();offsets=[]
    for (cpu,),row in cpus_raw.items():
        count=V.number(row,'points');status=V.number(row,'status');reason=V.number(row,'reason')
        V.require(count<=4 and status in range(1,6) and reason<=14 and V.number(row,'registered') in (0,1),
                  'clocksource preparation state')
        if source==2:V.require(status==3 and reason==0,'clocksource selected unqualified CPU')
        local=[points[(cpu,i)] for i in range(count)]
        if status==3:
            address=V.number(row,'physical')
            V.require(count==4 and V.number(row,'features')&((1<<3)|(1<<24))==((1<<3)|(1<<24)) and
                address>0 and not address%64 and address not in physical and
                V.number(row,'registration')==(address|1) and V.number(row,'registered')==1,
                'clocksource registration lifetime/feature binding')
            physical.add(address);bounds=[offset_at(p,origin) for p in local]
            lo=max(p[0] for p in bounds);hi=min(p[1] for p in bounds)
            V.require(lo<=hi and (signed(row,'lower'),signed(row,'upper'))==(lo,hi),'clocksource CPU offset intersection')
            for a,b in zip(local,local[1:]):
                drift(a,b,True)
            offsets.append((lo,hi))
        result.append(dict(cpu=cpu,status=status,reason=reason,points=local,preparation=row))
    offset=signed(selected,'offset')
    if source==2:
        V.require(origin==V.number(points[(bsp,0)],'ns'),'clocksource common epoch not original BSP reference')
        lo=max(p[0] for p in offsets);hi=min(p[1] for p in offsets)
        midpoint=(abs(lo+hi)//2)*(1 if lo+hi>=0 else -1)
        V.require(lo<=hi and offset==midpoint,'clocksource all-CPU common epoch')
    else:V.require(offset==0,'clocksource HPET origin changed')
    return dict(status='PASS',contract=CONTRACT,source=source,epoch=epoch,offset=offset,origin_ns=origin,cpus=result,
                timing_acceptance=False,functional_fallback=source==1)

def terminal(raw,selection):
    n=V.number;cpus=len(selection['cpus'])
    prepared={r['cpu']:r for r in selection['cpus']}
    V.require(len(prepared)==cpus and set(prepared)==set(range(cpus)),
              'clocksource prepared CPU identity')
    rows=V.index(raw['CLOCKSOURCE_TERMINAL'],('cpu',));refs=V.index(raw['CLOCKSOURCE_REFERENCE'],('cpu','point'))
    V.require(set(rows)=={(c,) for c in range(cpus)},'clocksource terminal CPU coverage')
    expected=set();result=[]
    for (cpu,),r in rows.items():
        V.require(n(r,'version')==3 and n(r,'source')==selection['source'] and n(r,'epoch')==selection['epoch'] and
            n(r,'busy')==0,'clocksource terminal provider/ownership')
        count=n(r,'references');reads=n(r,'reads')
        V.require(reads<V.U64 and count<V.U64,'clocksource exhausted counts')
        if selection['source']==2:
            V.require(n(r,'reason')==0 and n(r,'registered')==1 and reads>0 and n(r,'last_tsc')>0 and n(r,'last_ns')>0,
                      'clocksource failed or unavailable runtime')
            original=prepared[cpu]['points'][-1]
            # Successful reads advance strictly from the final qualification sample.
            # last_ns is raw pvclock time, before the public clock's common offset.
            V.require(n(r,'last_tsc')>n(original,'tsc1') and n(r,'last_ns')>n(original,'ns1'),
                      'clocksource last successful sample predates qualification')
            # A reference attempt can fail before reads is incremented. This bound
            # applies only after the successful source/state checks above.
            V.require(count<=reads,'clocksource references exceed successful reads')
        if count:
            V.require(selection['source']==2,'clocksource references without selected KVM provider')
            expected.update(((cpu,0),(cpu,1)));previous,p=(refs[(cpu,side)] for side in range(2))
            V.require(all(n(v,'version')==3 and n(v,'epoch')==selection['epoch'] for v in (previous,p)),'clocksource reference epoch')
            drift(previous,p)
            V.require(n(r,'maximum_reference_ns')==n(p,'ns1')-n(p,'ns0'),
                      'clocksource mandatory reference maximum')
            V.require(p['mul0']==original['mul0'] and p['shift0']==original['shift0'] and
                p['period']==original['period'] and p['mode']==original['mode'] and
                n(p,'tsc0')>n(original,'tsc1') and n(p,'ns0')>n(original,'ns1'),
                'clocksource runtime scale/epoch')
            same_boot=reference_facts(previous)==reference_facts(original)
            V.require(count!=1 or same_boot,'clocksource first reference predecessor is not final boot reference')
            if not same_boot:
                # Only the maximum and its immediate predecessor are retained.
                # Rate/ordering bounds compose across the omitted valid references.
                drift(original,previous)
                V.require(n(previous,'ns1')-n(previous,'ns0')<n(r,'maximum_reference_ns'),
                          'clocksource retained maximum did not exceed predecessor')
            # A public read samples first, then may collect its HPET reference.
            # Its last successful sample need not follow either retained endpoint;
            # the retained maximum also need not be the latest reference.
        else:V.require(n(r,'maximum_reference_ns')==0,'clocksource lost reference maximum')
        result.append(r)
    V.require(set(refs)==expected,'clocksource reference loss/duplicate')
    return dict(status='PASS',rows=result,references=list(refs.values()),timing_acceptance=False)

def reference_facts(row):
    """Compare the copied reference, independently of boot/runtime record labels."""
    keys=('tsc0','ns0','ver0','mul0','flags0','retry0',
          'tsc1','ns1','ver1','mul1','flags1','retry1',
          'ticks','ns','period','mode','retries','valid')
    return tuple(V.number(row,k) for k in keys)+(signed(row,'shift0'),signed(row,'shift1'))
