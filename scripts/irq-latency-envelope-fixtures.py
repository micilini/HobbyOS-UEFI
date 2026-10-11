#!/usr/bin/env python3
"""Exercise the actual witness checker with consistently re-encoded mutations."""
import argparse
import collections
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import irq_latency_evidence as V

spec = importlib.util.spec_from_file_location('raw_fixtures', Path(__file__).with_name('irq-latency-fixtures.py'))
F = importlib.util.module_from_spec(spec)
spec.loader.exec_module(F)


def witness(serial, cpu, path):
    rows = [r for r in V.records(serial)['WITNESS'] if r['kind'] == 'maximum' and
            V.number(r, 'cpu') == cpu and V.number(r, 'index') == path]
    return [V.number({'v':v}, 'v') for r in sorted(rows, key=lambda r: V.number(r,'first'))
            for v in r['cells'].split(',')]


def rewrite(serial, cpu, path, changes, cycles=None):
    cells = witness(serial, cpu, path)
    for at, value in changes.items():
        cells[at] = value
        first = at // 8 * 8
        def transform(row):
            values = row['cells'].split(','); values[at-first] = str(value)
            return dict(row, cells=','.join(values))
        serial = F.change_line(serial, 'WITNESS',
            lambda r: r['kind']=='maximum' and V.number(r,'cpu')==cpu and
            V.number(r,'index')==path and V.number(r,'first')==first, transform)
    raw = V.records(serial)
    hz = V.number(next(r for r in raw['TIMEBASE'] if r['role']=='0' and V.number(r,'cpu')==cpu),'conversion_hz')
    total = cells[1]-cells[0]+cells[14] if cycles is None else cycles
    ns = V.ceildiv(total*10**9, hz)
    serial = F.change_line(serial,'MAXIMUM',lambda r:V.number(r,'cpu')==cpu and V.number(r,'path')==path,
        lambda r:dict(r,maximum_cycles=str(total),maximum_ns=str(ns)))
    return serial


def run(path, reproduce=False):
    original=path.read_text(); raw=V.records(original); base=V.inspect(original)
    clock=V.clocks(raw,base['cpus'])
    probe=next(r for r in raw['OVERHEAD'] if (r['role'],r['index'],r['probe'],r['cpu'])==('1','460','0','3'))
    timing={key:V.number(probe,key) for key in ('outer_begin','inner_begin','inner_end','outer_end')}
    timing.update(leading=timing['inner_begin']-timing['outer_begin'],
                  interior=timing['inner_end']-timing['inner_begin'],
                  trailing=timing['outer_end']-timing['inner_end'])
    assert (timing['leading'],timing['interior'],timing['trailing'])==(18162,13024,508)
    shapes=collections.Counter()
    pieces=collections.defaultdict(dict)
    for r in raw['WITNESS']:
        pieces[(r['cpu'],r['kind'],r['index'])][int(r['first'])]=[int(v) for v in r['cells'].split(',')]
    for key, chunks in pieces.items():
        w=[v for first in sorted(chunks) for v in chunks[first]]
        if w[0] and not w[11]: shapes[(w[19],w[20],w[7],bool(w[14]))]+=1
    missing=rewrite(original,0,0,{14:0})
    cases=[('missing-allowance',missing,'primitive/calibration allowance binding')]
    if not reproduce:
        for cpu in (1,4):
            cases.append(('borrow-cpu-'+str(cpu),rewrite(original,0,0,{14:2*clock[(0,cpu)]['overhead_max']}),
                          'primitive/calibration allowance binding'))
        cases.append(('substitute-post-allowance',rewrite(original,0,0,{14:2*clock[(1,0)]['overhead_max']}),
                      'primitive/calibration allowance binding'))
        cases.append(('kind-to-zero',rewrite(original,0,0,{19:0,20:0,14:0}), 'primitive endpoint meaning'))
        cells=witness(original,0,0)
        cases.append(('truncate-outer-ticket-interval',rewrite(original,0,0,{0:cells[0]+1}), 'primitive endpoint meaning'))
        for name, transform, boundary in [
            ('delete-worst-post',lambda r:None,'probe coverage'),
            ('discard-worst-post',lambda r:dict(r,valid='0'),'empty IRQ probe eligibility changed')]:
            cases.append((name,F.change_line(original,'OVERHEAD',
                lambda r:(r['role'],r['index'],r['probe'],r['cpu'])==('1','460','0','3'),transform),boundary))
        cases.append(('qualified-despite-post-failure',F.change_line(original,'CALIBRATION_END',lambda r:True,
                     lambda r:dict(r,status='VALID')),'false calibration success'))
    results=[]
    for name,synthetic,boundary in cases:
        outcomes=[]
        checks=['maxima','inspect'] if name in ('missing-allowance','borrow-cpu-1','borrow-cpu-4',
            'substitute-post-allowance','kind-to-zero') else ['inspect']
        for check in checks:
            try:
                if check=='maxima':V.maxima(V.records(synthetic),base['cpus'],clock)
                else:V.inspect(synthetic)
            except ValueError as error:
                if reproduce: raise
                V.require(boundary in str(error),'envelope fixture wrong rejection: '+str(error))
                outcomes.append({'checker':check,'result':'REJECTED','reason':str(error)})
            else:
                V.require(reproduce,'envelope mutation accepted: '+name)
                outcomes.append({'checker':check,'result':'ACCEPTED_DEFECT_REPRODUCED'})
        results.append({'name':name,'synthetic':True,'sha256':hashlib.sha256(synthetic.encode()).hexdigest(),
                        'outcomes':outcomes})
        V.require(V.inspect(original)==base,'restored original differs')
    current=current_cases(witness(original,0,0),clock) if not reproduce else None
    return {'schema':1,'status':'DEFECT_REPRODUCED' if reproduce else 'PASS',
        'base_sha256':hashlib.sha256(original.encode()).hexdigest(),'cpu3_probe':timing,
        'historical_shapes':[{'begin':k[0],'end':k[1],'context':k[2],'adjusted':k[3],'count':n}
                             for k,n in sorted(shapes.items())],
        'cases':results,'current_schema':current,'restored_positive_replays':len(cases),'implementation_acceptance':False}


def current_cases(seed, actual_clock):
    """Synthetic current records, explicitly distinct from captured schema one."""
    clock=copy.deepcopy(actual_clock)
    clock[(0,0)]['overhead_max']=20;clock[(0,0)]['conversion_hz']=10**9
    clock[(1,0)]['observer_errors']=0
    def dataset(ns, retired=False):
        w=list(seed)+[40,0,0,0]
        w[0]=100000;w[1]=100000+ns-40;w[2]=w[3]=1
        w[4]=1;w[5]=32;w[6]=32;w[7]=2;w[8]=w[9]=0;w[10]=1;w[11]=0
        w[12]=10;w[13]=20;w[14]=40;w[15]=w[0];w[16]=0
        w[17]=w[1];w[18]=0;w[19]=w[20]=1;w[21]=w[22]=w[23]=w[24]=0
        if retired:
            w[6]=0xffffffff80002000;w[17]=0;w[18]=(1<<32)-1;w[20]=4
            w[21]=w[1];w[22]=1;w[23]=128;w[24]=7;w[26:29]=[128,7,1]
        raw=collections.defaultdict(list)
        raw['BEGIN']=[{'witness_cells':'29','schema':'2','primitive':'ordered-tsc-irq-envelope'}]
        raw['CPU']=[{'cpu':'0','epoch':'1','sequence':'2','failures':'0','cutoff':'999999'}]
        for path in range(14):
            row={'cpu':'0','path':str(path),'maximum_cycles':str(ns if path==0 else 0),
                 'count':'0','invalid':'0','violations':'0','maximum_ns':'0',
                 'status':'INVALID_COVERAGE' if path<11 else 'EXCLUDED_CONTEXT'}
            if path==0:
                row.update(count='1',invalid='0',violations=str(int(ns>=50000)),maximum_ns=str(ns),
                    frequency_hz=str(10**9),ns_available='1',measurement_qualified='1',
                    status='VIOLATION' if ns>=50000 else 'BELOW_BUDGET_OBSERVATION')
            raw['MAXIMUM'].append(row)
            cells=w if path==0 else [0]*29
            for i in range(0,29,8):
                values=cells[i:i+8]
                raw['WITNESS'].append({'cpu':'0','kind':'maximum','index':str(path),'first':str(i),
                    'count':str(len(values)),'cells':','.join(map(str,values))})
        return raw
    def encoded(raw):
        import zlib
        lines=[]
        for kind,rows in raw.items():
            for row in rows:
                body='[IRQ_LATENCY]['+kind+'] '+' '.join(k+'='+v for k,v in row.items())
                lines.append(body+' crc='+format(zlib.crc32(body.encode()),'08x'))
        return '\n'.join(lines)+'\n'
    def check(raw):return V.maxima(V.records(encoded(raw)),1,clock)
    results=[]
    for ns in (49999,50000,50001):
        result=check(dataset(ns))
        V.require(result[0]['below_budget']==(ns<50000),'strict enveloped threshold')
        results.append({'ns':ns,'adjustment':40,'below_budget':result[0]['below_budget']})
    original=dataset(50001,True);positive=check(original);negatives=[]
    for name,field,value,reason in [('retired-adjustment-lost',14,0,'allowance binding'),
        ('retired-close-allowance-lost',25,0,'frozen close allowance'),
        ('retired-owner-generation-changed',24,8,'captured owner/generation/sequence'),
        ('retired-owner-slot-changed',23,129,'captured owner/generation/sequence'),
        ('retired-ticket-changed',28,2,'captured owner/generation/sequence'),
        ('retired-guessed-endpoint',21,150000,'IRQ fallback endpoint'),
        ('retired-invented-raw-endpoint',17,150001,'retired publisher identity'),
        ('unknown-boundary-kind',20,99,'boundary class')]:
        raw=copy.deepcopy(original)
        row=next(r for r in raw['WITNESS'] if r['index']=='0' and int(r['first'])==field//8*8)
        cells=row['cells'].split(',');cells[field%8]=str(value);row['cells']=','.join(cells)
        if field==14:
            raw['MAXIMUM'][0].update(maximum_cycles='49961',maximum_ns='49961',violations='0',status='BELOW_BUDGET_OBSERVATION')
        try:check(raw)
        except ValueError as error:
            V.require(reason in str(error),'current schema fixture rejection: '+str(error))
            negatives.append({'name':name,'reason':str(error),'synthetic':True,
                              'raw_sha256':hashlib.sha256(encoded(raw).encode()).hexdigest()})
        else:raise ValueError('current schema mutation accepted: '+name)
        V.require(check(original)==positive,'current schema restored positive')
    for name, mutation in (
        ('nonempty-count-with-erased-maximum', {'maximum_cycles':'0','maximum_ns':'0','violations':'0'}),
        ('empty-count-with-nonempty-maximum', {'count':'0'}),
        ('invalid-count-exceeds-count', {'invalid':'2'})):
        raw=copy.deepcopy(original);raw['MAXIMUM'][0].update(mutation)
        if name=='nonempty-count-with-erased-maximum':
            for row in raw['WITNESS']:
                if row['index']=='0':row['cells']=','.join('0' for _ in row['cells'].split(','))
        try:check(raw)
        except ValueError as error:
            V.require('count/valid-maximum relationship' in str(error),'count fixture rejection: '+str(error))
            negatives.append({'name':name,'reason':str(error),'synthetic':True,
                              'raw_sha256':hashlib.sha256(encoded(raw).encode()).hexdigest()})
        else:raise ValueError('current count mutation accepted: '+name)
        V.require(check(original)==positive,'count fixture restored positive')
    return {'authority':'SYNTHETIC_CURRENT_SCHEMA_AND_CLOCK_NOT_RUNTIME_ACCEPTANCE',
            'strict_threshold':results,'negative_cases':negatives,'restored_positives':len(negatives)}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('serial',type=Path)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--reproduce',action='store_true')
    a=p.parse_args();r=run(a.serial,a.reproduce)
    a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps(r))
