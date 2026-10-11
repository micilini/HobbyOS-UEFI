#!/usr/bin/env python3
"""Additional control-policy predicates over the unchanged observation evidence."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import memory_evidence as E
import memory_table_proof as T

ROOT = Path(__file__).resolve().parent.parent
POLICY = {'cr0': 0x80010033, 'cr4': 0x20, 'pat': 0x0007010600070406}
REQUIRED_BASIC = 0x10060
READY = re.compile(r'^\[CPU_STATE\]\[READY\] slot=(\d+) apic=(\d+) role=(BSP|AP) '
                   r'cr0=(0x[0-9a-f]{16}) cr3=(0x[0-9a-f]{16}) cr4=(0x[0-9a-f]{16}) '
                   r'efer=(0x[0-9a-f]{16}) pat=(0x[0-9a-f]{16}) '
                   r'basic_edx=(0x[0-9a-f]{8}) extended_edx=(0x[0-9a-f]{8})$')


def observation():
    spec = importlib.util.spec_from_file_location('control_observation', ROOT / 'scripts/verify-memory-observation.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def number(row, key):
    value = row[key]
    return value if isinstance(value, int) else int(value, 0)


def policy_cpu(row, root=None, nx_disabled=False):
    basic = number(row, 'cpuid1_edx' if 'cpuid1_edx' in row else 'basic_edx')
    ext = number(row, 'extended_edx')
    E.require(basic & REQUIRED_BASIC == REQUIRED_BASIC and ext & (1 << 29), 'policy required local feature missing')
    nx = bool(ext & (1 << 20))
    E.require(nx != nx_disabled, 'policy NX capability differs from named profile')
    for key, expected in POLICY.items():
        E.require(number(row, key) == expected, 'policy ' + key + ' readback mismatch')
    E.require(number(row, 'efer') == (0xd00 if nx else 0x500), 'policy EFER/NX readback mismatch')
    if root is not None: E.require(number(row, 'cr3') == root, 'policy active root mismatch')
    return {'nx': nx, **POLICY, 'efer': 0xd00 if nx else 0x500}


def policy_observation(lines, topology, nx_disabled=False):
    V = observation()
    generation, summary, cpus = V.verify_cpu(lines, topology)
    root = number(cpus[0], 'cr3')
    expected = [policy_cpu(row, root, nx_disabled) for row in cpus]
    E.require(all(row == expected[0] for row in expected) and summary['controls_uniform'] == 'true',
              'policy incomplete uniformity')
    return {'generation': generation, 'root': root, 'policy': expected[0], 'cpus': cpus}


def readiness(lines, topology, nx_disabled=False):
    keys = ['slot','apic','role','cr0','cr3','cr4','efer','pat','basic_edx','extended_edx']
    rows = []
    for index, line in enumerate(lines):
        if '[CPU_STATE][READY]' not in line: continue
        match = READY.fullmatch(line)
        E.require(match, 'policy readiness record malformed')
        row = dict(zip(keys, match.groups())); row['slot'] = int(row['slot']); row['apic'] = int(row['apic'])
        row['line_index'] = index; rows.append(row)
    E.require(len(rows) == len(topology) and len({r['slot'] for r in rows}) == len(rows), 'policy readiness missing/duplicate CPU')
    E.require({r['slot']:(r['apic'],r['role']) for r in rows} == topology, 'policy readiness identity/role mismatch')
    root = number(rows[0], 'cr3')
    for row in rows:
        policy_cpu(row, root, nx_disabled)
        marker = f"[SMP][CPU] ONLINE slot={row['slot']} apic={row['apic']} role={row['role']}"
        E.require(lines.count(marker) == 1 and lines.index(marker) > row['line_index'], 'policy ONLINE precedes local validation')
    E.require(not any('[CPU_STATE][FAIL]' in l or '[CPU_STATE][AP_FAIL]' in l for l in lines), 'policy startup failure')
    return rows


def early_readback(directory, lines, ready, nx_disabled=False):
    early = [line for line in lines if '[CPU_STATE][EARLY]' in line]
    E.require(len(early)==1, 'policy early BSP readback missing/duplicate')
    match = READY.fullmatch(early[0].replace('[EARLY]','[READY]',1))
    E.require(match, 'policy early BSP record malformed')
    keys = ['slot','apic','role','cr0','cr3','cr4','efer','pat','basic_edx','extended_edx']
    row = dict(zip(keys,match.groups()))
    roots = [int(parts[0],16) for line in (directory/'symbols.txt').read_text().splitlines()
             if len(parts:=line.split())==3 and parts[2]=='boot_pml4']
    E.require(len(roots)==1, 'policy bootstrap root symbol missing/duplicate')
    bsp = next(r for r in ready if r['role']=='BSP')
    E.require(number(row,'slot')==0xffffffff and row['role']=='BSP' and number(row,'apic')==bsp['apic'],
              'policy early/discovered BSP identity differs')
    policy_cpu(row,roots[0],nx_disabled)
    for key in ('cr0','cr4','efer','pat','basic_edx','extended_edx'):
        E.require(number(row,key)==number(bsp,key), 'policy early/final BSP readback differs')
    E.require(lines.count('[CORE] Init PMM...')==1 and lines.index(early[0])<lines.index('[CORE] Init PMM...'),
              'policy BSP initialization happened after memory setup')
    limitations = [l for l in lines if l.startswith('[CPU_STATE][LIMIT]')]
    E.require(limitations==(['[CPU_STATE][LIMIT] nx=UNSUPPORTED nxe=0'] if nx_disabled else []),
              'policy NX limitation record differs')
    return {'apic':bsp['apic'],'bootstrap_root':roots[0],'policy':policy_cpu(row,roots[0],nx_disabled)}


def cache_snapshot(directory):
    if 'origin=owned_BootInfo_framebuffer' in (directory/'qemu-serial.log').read_text():
        from device_mapping_evidence import cache_snapshot as owned_cache_snapshot
        return owned_cache_snapshot(directory)
    meta = json.loads((directory / 'table-snapshot.json').read_text())
    raw = (directory / 'table-frames.bin').read_bytes()
    E.require(E.sha(directory / 'table-frames.bin') == meta['frames_sha256'], 'cache topology raw binding')
    addresses = meta['frame_addresses']
    E.require(len(raw) == len(addresses)*4096 and len(set(addresses)) == len(addresses), 'cache table frame coverage')
    frames = {a:raw[i*4096:(i+1)*4096] for i,a in enumerate(addresses)}
    result = []
    for root in (meta['active_root'],meta['bootstrap_root'],meta['static_frames']['boot_pml4']):
        levels = T.traverse(root,frames,meta['physical_bits'],meta['page1g'])
        slots = {}; links = {}
        for address,level in levels.items():
            for entry, in struct.iter_unpack('<Q',frames[address]):
                if not entry & 1: continue
                leaf = level == 1 or (level in (2,3) and entry & 128)
                slot = (entry >> 3) & 3
                if leaf: slot |= ((entry >> (7 if level == 1 else 12)) & 1) << 2
                E.require(slot != 5, 'cache spare slot used by actual mapping')
                target = slots if leaf else links
                target[str(slot)] = target.get(str(slot),0)+1
        result.append({'root':root,'frames':len(levels),'leaf_slots':slots,'nonleaf_slots':links})
    return result


def cache_comparison(previous, current):
    """Compare semantic representatives after each candidate's own raw replay."""
    O = observation()
    old = (previous/'qemu-serial.log').read_text().splitlines()
    new = (current/'qemu-serial.log').read_text().splitlines()
    topologies = [cache_snapshot(directory) for directory in (previous,current)]
    before, after = 0x0007040600070406, POLICY['pat']
    for topology in topologies:
        for tree in topology:
            for slot in set(tree['leaf_slots']) | set(tree['nonleaf_slots']):
                shift = int(slot)*8
                E.require((before>>shift)&255 == (after>>shift)&255, 'cache selected PAT byte changed')
    results = []
    for sample in ('kernel','ram','mmio','framebuffer'):
        rows = [next(r for r in O.records(lines,'PAGE_TABLE','RESULT') if r['sample']==sample) for lines in (old,new)]
        fields = ('status','leaf','page_size','effective_rw','effective_us','effective_nx',
                  'pat_index','pat_type_raw','pat_type_valid','pat_type','effective_type')
        semantic = [{k:r[k] for k in fields} for r in rows]
        E.require(semantic[0] == semantic[1], 'cache representative selection/permission changed: '+sample)
        controls = [next(r for r in O.records(lines,'PAGE_TABLE','CONTROL') if r['sample']==sample) for lines in (old,new)]
        E.require(controls[0]['origin']==controls[1]['origin'], 'cache representative origin changed')
        if sample in ('mmio','framebuffer'):
            E.require(rows[0]['physical']==rows[1]['physical'], 'cache device representative region changed')
        results.append({'sample':sample,'origin':controls[0]['origin'],**semantic[1]})
    # The retained firmware-owned variable MTRRs are comparable on this exact
    # Q35/KVM profile. Fixed ranges remain explicitly uncollected.
    controls = [O.one(lines,'MTRR','CONTROL') for lines in (old,new)]
    fields = ('supported','physical_bits','capability','capability_valid','defaults','defaults_valid',
              'count','enabled','fixed_supported','fixed_enabled','default_type','wc_supported','fixed_coverage','effective_type')
    E.require({k:controls[0][k] for k in fields}=={k:controls[1][k] for k in fields}, 'firmware MTRR controls changed')
    ranges = [[{k:v for k,v in row.items() if k not in ('schema','id')} for row in O.records(lines,'MTRR','RANGE')] for lines in (old,new)]
    E.require(ranges[0]==ranges[1], 'firmware variable MTRR inputs changed')
    return {'status':'PASS','previous_candidate':json.loads((previous/'result.json').read_text())['candidate_id'],
            'current_candidate':json.loads((current/'result.json').read_text())['candidate_id'],
            'previous_topology':topologies[0],'current_topology':topologies[1],
            'representatives':results,'variable_mtrrs_unchanged':len(ranges[0]),
            'fixed_coverage':'NOT_COLLECTED','effective_type':'NOT_COMPUTED'}


def control_readback(directory, ready, result):
    cfg=json.loads((directory/'capture-config.json').read_text())
    proof=json.loads((directory/'control-readback.json').read_text())
    E.require(cfg.get('control_readback') is True and proof['schema']==1 and
              proof['candidate_id']==result['candidate_id'] and proof['vm_id']==result['vm_id'],
              'control debugger candidate/VM binding')
    qmp=[json.loads(l) for l in (directory/'vm-identity.jsonl').read_text().splitlines()]
    requests=[r['message'] for r in qmp if r['direction']=='send']
    def answer(request):
        rows=[r['message'] for r in qmp if r['direction']=='receive' and r['message'].get('id')==request['id']]
        E.require(len(rows)==1 and 'return' in rows[0],'control QMP identity response missing/duplicate')
        return rows[0]['return']
    queries=[r for r in requests if r.get('execute')=='query-cpus-fast']
    E.require(len(queries)==1,'control QMP CPU identity query missing/duplicate')
    identities=[]
    for cpu in answer(queries[0]):
        queries2=[r for r in requests if r.get('execute')=='qom-get' and
                  r.get('arguments')=={'path':cpu['qom-path'],'property':'apic-id'}]
        E.require(len(queries2)==1,'control QMP APIC identity missing/duplicate')
        identities.append({'cpu_index':cpu['cpu-index'],'qom_path':cpu['qom-path'],'apic_id':answer(queries2[0])})
    E.require(identities==cfg['cpu_identity'] and {r['apic_id'] for r in identities}=={r['apic'] for r in ready},
              'control QMP/boot identity mismatch')
    output=(directory/'page-readback.stdout.log').read_text()
    thread_info=(directory/'control-threads.log').read_text()
    E.require('CPU_STATE_THREADS_BEGIN\n'+thread_info+'CPU_STATE_THREADS_END' in output,
              'control debugger thread transcript missing')
    descriptions={int(m[1]):m[2] for line in thread_info.splitlines()
                  if (m:=re.match(r'^\s*\*?\s*(\d+)\s+Thread \d+\.\d+ \((CPU#\d+ \[(?:running|halted) ?\])\)',line))}
    captured=[json.loads(l[len('CPU_STATE_GDB '):]) for l in output.splitlines() if l.startswith('CPU_STATE_GDB ')]
    E.require(captured==proof['cpus'] and len(captured)==len(ready) and
              len({r['cpu_index'] for r in captured})==len(captured),'control debugger raw coverage/order')
    for row in captured:
        E.require(descriptions.get(row['thread'])==row['thread_name'],'control debugger thread mapping differs')
        identity=next((i for i in identities if i['cpu_index']==row['cpu_index']),None)
        E.require(identity is not None and identity['apic_id']==row['apic_id'] and
                  re.fullmatch(r'CPU#'+str(row['cpu_index'])+r' \[(running|halted) ?\]',row['thread_name']),
                  'control debugger vCPU identity mismatch')
        boot=next(r for r in ready if r['apic']==row['apic_id'])
        for key in ('cr0','cr3','cr4','efer'):
            E.require(row['registers'][key]==number(boot,key),'control debugger/raw local disagreement: '+key)
        if row['pat_access']=='GDB_REGISTER':
            E.require(row['registers']['pat']==number(boot,'pat'),'control debugger PAT mismatch')
        else:
            E.require(row['pat_access']=='UNAVAILABLE_USE_LOCAL_READBACK' and row['registers']['pat'] is None,
                      'control debugger PAT limitation misrepresented')
    return {'status':'PASS','cpus':len(captured),'pat_access':sorted({r['pat_access'] for r in captured})}


def verify_run(directory):
    V = observation(); observed = V.verify_run(directory / 'qemu-serial.log')
    result = json.loads((directory / 'result.json').read_text())
    lines = (directory / 'qemu-serial.log').read_text().splitlines()
    topology = {}
    for line in lines:
        match = re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if match: topology[int(match[1])] = (int(match[2]),match[3])
    nx_disabled = result['profile'] == 'q35-tcg-smp4-nx-disabled'
    ready = readiness(lines,topology,nx_disabled)
    early = early_readback(directory,lines,ready,nx_disabled)
    root = number(ready[0],'cr3')
    generations = []
    memory = []
    for line in (directory / 'framed.jsonl').read_text().splitlines():
        row = json.loads(line)
        if row['payload'] == 'mem':
            segment = V.FOUNDATION.transaction_segment(lines,row) if not observed.get('production') else lines
            summaries = [dict(item.split('=',1) for item in line.split()[2:])
                         for line in segment if line.startswith('[MEM][SUMMARY] PASS ')]
            # The ordinary memory verifier retains exact handler and both
            # integrity predicates; this additionally pins the expensive span.
            if result['profile']=='q35-tcg-smp1':
                E.require(len(summaries)==1 and number(summaries[0],'pmm_total')==268435456*4096,
                          'control campaign PMM address span changed')
            memory.append({'pmm_total':number(summaries[0],'pmm_total')} if summaries else {})
        if row['payload'] != 'cpustate' or observed.get('production'): continue
        # Use the same exact framed segment as the generic observation verifier.
        segment = V.FOUNDATION.transaction_segment(lines,row)
        checked = policy_observation(segment,topology,nx_disabled)
        E.require(checked['root'] == root, 'policy boot/observer root disagreement')
        for cpu in checked['cpus']:
            boot = next(r for r in ready if r['slot'] == number(cpu,'slot'))
            for key in ('cr0','cr3','cr4','efer','pat','extended_edx'):
                E.require(number(boot,key)==number(cpu,key),'policy boot/raw observer disagreement: '+key)
            E.require(number(boot,'basic_edx')==number(cpu,'cpuid1_edx'),'policy boot/raw CPUID disagreement')
        generations.append(checked['generation'])
    if not observed.get('production'):
        E.require(len(generations)>=3 and all(a<b for a,b in zip(generations,generations[1:])), 'policy CPU collection generations')
    return {'status':'PASS','observation':observed,'profile':result['profile'],
            'candidate_id':result['candidate_id'],'vm_id':result['vm_id'],'boot_id':result['boot_id'],
            'ready_cpus':len(ready),'root':root,'generations':generations,
            'memory':memory,
            'early_bsp':early,
            'policy':policy_cpu(ready[0],root,nx_disabled),
            'independent_controls':control_readback(directory,ready,result) if result['readback'] else None,
            'cache_topology':cache_snapshot(directory) if (directory/'table-snapshot.json').exists() else None}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',type=Path,required=True);a=p.parse_args()
    try: print(json.dumps(verify_run(a.run),sort_keys=True))
    except Exception as error: p.exit(1,str(error)+'\n')
