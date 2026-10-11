#!/usr/bin/env python3
"""Address-domain and allocation predicates over the maintained raw campaign."""
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys
import memory_evidence as E
import direct_map_evidence as RAM

ROOT=Path(__file__).resolve().parent.parent
def module(name):
    spec=importlib.util.spec_from_file_location('physical_verify_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

PRESERVED_BASELINES={
    'original-observation':('ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c',
        [1411852460,1415497788,1446679785],[157258679,155190823,156314361]),
    'explicit-controls':('413053169c0efc3b8a05d403e8963eb733be5fd8b3f49d6fa27d382a61813678',
        [1387257401,1386365974,1391752690],[157212027,153721843,157642242]),
    'acknowledged-invalidation':('747c204de63f5ea4e5fffd11307806a3b38fc474a4cc4cdea806e8a7cc437732',
        [1330591597,1400517537,1317141737],[156774130,153319291,156445620])}
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
    PRESERVED_BASELINES['checked-translation']=(
        'b167cfe76eb1c35b6921eb7f5ac23cff3957d53e8742145f65b27f09cef26509',
        [2098003665,2191778591,2756748052],[170838209,172532462,173073803])
if RAM.enabled():
    PRESERVED_BASELINES['owned-mappings']=(
        '57ce4c800acb7ca38cae6a84d44e02a7084bc808b5c2af30b3150d8f032dc391',
        [2564996943,2741836097,2485873142],[176817922,179255304,175681729])
if RAM.LOW.enabled():
    PRESERVED_BASELINES['ram-direct-map']=(
        '6fb07baa23a2ea30ce6c534090e15a1ea0208efe3c9e9a80413156a19a9ec752',
        [396122705,237750383,267276187],[29652781,30233620,31684209])
if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
    PRESERVED_BASELINES['low-address-isolation']=(
        '6f0beae7401cb320c23b4fca1feee163aac99d4a14109209f93312358c759766',
        [24117763,26479156,25235907],[11121628,10613554,10733011])
    PRESERVED_BASELINES['image-protection']=(
        'b545ac1f779f753d273731ae028d92484b08a98fb989c1aa59ebfef9819267f7',
        [44751179,43725421,46300120],[14120494,14351355,14597098])

def preserved_baseline_summary(name, baseline):
    E.require(name in PRESERVED_BASELINES,'unknown preserved paging baseline')
    _,tcg,kvm=PRESERVED_BASELINES[name]
    expected={profile:{'samples_ns':samples,'median_ns':sorted(samples)[1]}
              for profile,samples in [('q35-tcg-smp4',tcg),('q35-kvm-smp4',kvm)]}
    E.require(baseline==expected,'preserved paging baseline changed')
    return expected

def baseline_references(directory):
    """Replay only the original timing subset, not retired runtime campaigns."""
    name=('index-guarded-stacks.json' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks'
          else 'index-low-address.json' if RAM.LOW.enabled() else 'index.json')
    index=json.loads((directory/name).read_text())
    E.require(index['schema']==1 and {r['name'] for r in index['baselines']}==set(PRESERVED_BASELINES)
              and len(index['baselines'])==len(PRESERVED_BASELINES),'preserved baseline inventory')
    observed={};identities=set();O=module('verify-memory-observation')
    for row in index['baselines']:
        name=row['name'];E.safe(name);root=directory/name
        paths=[r['path'] for r in row['files']]
        E.require(len(paths)==len(set(paths)) and set(paths)==
            {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()},'preserved timing subset completeness')
        for item in row['files']:
            E.safe(item['path']);path=root/item['path']
            E.require(not path.is_symlink() and path.stat().st_size==item['size'] and
                      E.sha(path)==item['sha256'],'preserved timing source bytes differ')
        candidate=json.loads((root/'candidate.json').read_text());identity=E.identity(candidate)
        E.require(identity==PRESERVED_BASELINES[name][0]==row['candidate_id'],'preserved timing candidate identity')
        baseline=preserved_baseline_summary(name,json.loads((root/'baseline.json').read_text())['baseline'])
        E.require(set(row['runs'])==set(baseline),'preserved timing profiles')
        for profile,selected in row['runs'].items():
            E.require(len(selected)==3 and len(set(selected))==3,'preserved timing repetitions')
            samples=[];prior=-1
            for relative in selected:
                E.safe(relative);run=root/relative
                launch=json.loads((run/'launch.json').read_text());result=json.loads((run/'result.json').read_text())
                E.require(result['profile']==profile and result['status']=='COLLECTED' and result['cleanup'] and
                    not result['trace_enabled'] and not result['readback'] and not launch['fault_trace'],
                    'preserved timing run is not a normal sample')
                for field in ('candidate_id','launch_id','vm_id','boot_id'):
                    E.require(launch[field]==result[field],'preserved timing launch binding')
                pair=(result['vm_id'],result['boot_id']);E.require(pair not in identities,'duplicated preserved timing boot');identities.add(pair)
                E.require(result['candidate_id']==identity and launch['source_sha256']==candidate['source_sha256'] and
                    result['candidate_elf']==candidate['artifacts']['kernel.elf']['sha256'] and
                    result['candidate_image']==candidate['artifacts']['hobbyos.img']['sha256'],
                    'preserved timing artifact binding')
                argv=launch['argv'];accel='tcg' if '-tcg-' in profile else 'kvm'
                for flag,value in (('-machine','q35'),('-accel',accel),('-smp','4'),('-m','2G'),('-cpu','max' if accel=='tcg' else 'host')):
                    E.require(argv.count(flag)==1 and argv[argv.index(flag)+1].split(',')[0]==value,
                              'preserved timing launch profile differs')
                timing=O.timing(run,(run/'qemu-serial.log').read_bytes())
                E.require(prior<result['started_ns']<timing['markers'][0]['monotonic_ns']<
                    timing['markers'][1]['monotonic_ns']<result['finished_ns'],'preserved timing chronology')
                prior=result['finished_ns'];samples.append(timing['duration_ns'])
            E.require(samples==baseline[profile]['samples_ns'],'preserved timing raw samples differ')
        observed[name]={'candidate_id':identity,'baseline':baseline,'samples':6,
                        'scope':'Original serial timing subset only; retired full runtime campaign is not replayed.'}
    return observed

def records(lines, kind):
    prefix='[PHYSICAL]['+kind+'] '
    rows=[]
    for line in lines:
        if not line.startswith(prefix):continue
        fields=line[len(prefix):].split();row={}
        for token in fields:
            key,separator,value=token.partition('=')
            E.require(separator and key not in row,'physical record malformed/duplicate field')
            row[key]=value
        rows.append(row)
    return rows

def number(row,key):
    E.require(key in row and re.fullmatch(r'(?:0x[0-9a-f]+|[0-9]+)',row[key]) is not None,'physical numeric field: '+key)
    return int(row[key],0) if row[key].startswith('0x') else int(row[key])

def fixture(lines, topology, root, image,ram=None):
    searches=records(lines,'SEARCH')
    plan=[(65,1,64,100,64),(65,1,65,100,None),(65,2,63,64,63),(65,2,64,65,None),
          (65,2,2,3,2),(65,0,2,3,None),(129,1,2,3,None),(65,2**64-1,2,3,None),
          (0,1,2,3,None),(64,1,63,100,63)]
    E.require(len(searches)==len(plan),'physical synthetic search coverage')
    for i,(row,(limit,count,a,b,expected)) in enumerate(zip(searches,plan)):
        E.require(set(row)==set('schema case synthetic bits start limit count allocated_low allocated_high found index words unchanged'.split()),'physical search field set')
        E.require([number(row,k) for k in ('schema','case','synthetic','bits','start','limit','count')]==[1,i,1,128,1,limit,count],
                  'physical synthetic plan changed')
        allocated=((1<<128)-1)^(1<<a)^(1<<b)
        E.require(number(row,'allocated_low')==allocated&((1<<64)-1) and number(row,'allocated_high')==allocated>>64,
                  'physical search backing differs')
        observed=None
        if 0<count<=128 and 1<limit<=128:
            for start in range(1,limit-count+1):
                if all(not ((allocated>>bit)&1) for bit in range(start,start+count)):
                    observed=start;break
        E.require(observed==expected,'independent search plan oracle')
        E.require(number(row,'found')==int(expected is not None) and number(row,'index')==(expected if expected is not None else 2**64-1) and
                  number(row,'unchanged')==1 and number(row,'words')<=2,'physical bounded search result/extent/mutation')
    result=records(lines,'RESULT')
    E.require(len(result)==1,'physical result missing/duplicate');result=result[0]
    E.require(set(result)==set('schema synthetic_cases represented_frames free_frames eligible_free_frames above_ceiling_free_frames ceiling minimum bitmap_physical bitmap_bytes root validations rejected owned_frames released_frames status'.split()),'physical result field set')
    E.require(result['status']=='PASS' and number(result,'schema')==1 and number(result,'synthetic_cases')==10 and
              number(result,'rejected')==0 and number(result,'validations')>0,'physical boundary violation/result failure')
    ceiling=number(result,'ceiling');total=number(result,'represented_frames');free=number(result,'free_frames')
    E.require(0<ceiling<=(ram['ceiling'] if ram else 2**32) and ceiling%4096==0 and total*4096>=ceiling and
              number(result,'minimum')==2**20,'physical ceiling/global span contradiction')
    E.require(number(result,'eligible_free_frames')+number(result,'above_ceiling_free_frames')==free<=total,
              'physical free/eligible accounting differs')
    E.require(number(result,'bitmap_bytes')>=2*((total+7)//8) and
              number(result,'bitmap_physical')+number(result,'bitmap_bytes')<=ceiling,'physical bitmap extent invalid')
    E.require(number(result,'root')==root and number(result,'owned_frames')==number(result,'released_frames')==4,
              'physical root/owned-frame retirement mismatch')
    pointers=records(lines,'POINTER');expected_names=['active_root','image','heap','single_frame','contiguous_frames']
    E.require([r['name'] for r in pointers]==expected_names,'physical round-trip coverage')
    for row in pointers:
        E.require(set(row)==set('schema name virtual bytes physical walked valid'.split()),'physical pointer field set')
        va,pa,size=(number(row,k) for k in ('virtual','physical','bytes'))
        E.require(number(row,'schema')==number(row,'valid')==1 and pa==number(row,'walked') and size>0,
                  'physical pointer/walker contradiction')
        if row['name']=='image':E.require(va==image and pa==va-0xffffffff80000000 and size==1,'image-only physical inverse mismatch')
        else:E.require(va==pa+(RAM.BASE if ram else 0) and 0<pa<ceiling and size<=ceiling-pa,'heap/identity image-offset misuse or ceiling crossing')
        if row['name']=='active_root':E.require(pa==root and size==4096,'physical root pointer differs')
        if row['name']=='single_frame':E.require(size==4096 and pa%4096==0,'single allocation frame extent')
        if row['name']=='contiguous_frames':E.require(size==12288 and pa%4096==0,'contiguous allocation frame extent')
    cpus=records(lines,'CPU');aps={s:a for s,(a,role) in topology.items() if role=='AP'}
    E.require([number(r,'slot') for r in cpus]==sorted(aps),'physical CPU structure completeness')
    for row in cpus:
        E.require(set(row)==set('schema slot apic stack_top stack_physical stack_bytes tss_virtual tss_physical gdt_virtual gdt_physical valid'.split()),'physical CPU field set')
        slot=number(row,'slot');base=number(row,'stack_physical');top=number(row,'stack_top')
        stack_alias=top==base+16384+(RAM.BASE if ram else 0)
        if __import__('stack_current').enabled():
            import stack_guard_evidence as SG
            stack_alias=SG.BASE<=top<SG.BASE+SG.CPU_SLOTS*SG.PITCH and (top-SG.BASE)%SG.PITCH==4096+16384
        E.require(number(row,'apic')==aps[slot] and number(row,'schema')==number(row,'valid')==1 and
                  number(row,'stack_bytes')==16384 and stack_alias and base+16384<=ceiling and base>0,
                  'physical AP stack extent/identity mismatch')
        for kind,size in [('gdt',56),('tss',104)]:
            pa=number(row,kind+'_physical')
            E.require(pa+(RAM.BASE if ram else 0)==number(row,kind+'_virtual') and 0<pa and pa+size<=ceiling,'physical CPU structure alias/extent')
    return {'searches':len(searches),'roundtrips':pointers,'ap_structures':len(cpus),'accounting':result}

def boot(lines):
    prefix='[PHYSICAL][BOOT] READY '
    selected=[(i,line) for i,line in enumerate(lines) if line.startswith(prefix)]
    E.require(len(selected)==1,'physical boot readiness missing/duplicate')
    index,line=selected[0];data=records([line.replace(prefix,'[PHYSICAL][BOOT] ',1)],'BOOT')[0]
    E.require(set(data)==set('schema early_sealed nested_objects descriptors identity_limit ceiling handoff map map_bytes'.split()),'physical boot field set')
    E.require(all(number(data,k)==1 for k in ('schema','early_sealed','nested_objects')) and
              0<number(data,'descriptors')<=1024 and 0<number(data,'ceiling')==number(data,'identity_limit')<=2**32,
              'physical early boundary/ceiling invalid')
    first=next((i for i,line in enumerate(lines) if '[CORE] Init PMM' in line),len(lines))
    E.require(index<first,'physical boundary initialized after PMM')
    return data

def production(symbols, strings):
    for name in ('physical_boot_adapt','physical_pointer_range','physical_address_range','bitmap_find_zero_run_below',
                 'pmm_alloc_frame','pmm_alloc_contiguous_frames','kernel_tlb_invalidate'):
        E.require(re.search(r'\b'+name+r'$',symbols,re.M),'physical production enforcement compiled out: '+name)
    for name in ('phystest','physical_fixture','physical_host_backing','g_fixture_owner','[PHYSICAL][SEARCH]'):
        E.require(name not in symbols and name not in strings,'physical production fixture payload present: '+name)

def readback(directory, result, observations):
    proof=json.loads((directory/'physical-readback.json').read_text())
    stdout=(directory/'page-readback.stdout.log').read_text()
    marker='PHYSICAL_STATE_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':'))
    E.require(stdout.splitlines().count(marker)==1,'physical raw debugger proof missing/contradicted')
    E.require(proof['schema']==1 and proof['candidate_id']==result['candidate_id'] and proof['vm_id']==result['vm_id'],
              'physical debugger candidate/VM mismatch')
    meta=json.loads((directory/'table-snapshot.json').read_text());data=(directory/'table-frames.bin').read_bytes()
    E.require(E.sha(directory/'table-frames.bin')==meta['frames_sha256'],'physical table bytes changed')
    E.require(proof['root_physical']==proof['root_pointer']-(RAM.BASE if RAM.enabled() else 0)==(proof['cr3']&0x000ffffffffff000)==meta['active_root'],
              'physical raw root/address domain mismatch')
    addresses=meta['frame_addresses']
    E.require(addresses==sorted(set(addresses)) and len(data)==len(addresses)*4096,'physical missing table bytes')
    frames={a:data[i*4096:(i+1)*4096] for i,a in enumerate(addresses)}
    def walk(va):
        table=proof['root_physical']
        for level in (4,3,2,1):
            E.require(table in frames,'physical round-trip table absent')
            entry=struct.unpack_from('<Q',frames[table],((va>>(12+9*(level-1)))&511)*8)[0]
            E.require(entry&1,'physical known range is unmapped')
            if level==1 or (level in (2,3) and entry&128):
                mask=(1<<(12+9*(level-1)))-1
                return (entry&0x000ffffffffff000&~mask)|(va&mask)
            table=entry&0x000ffffffffff000
        raise ValueError('physical walker exhausted')
    points=0
    for observation in observations:
        for row in observation['roundtrips']:
            va,pa,size=(number(row,k) for k in ('virtual','physical','bytes'))
            # Data may have retired since the command; only stable mappings
            # are read. No old data frame contents are dereferenced here.
            for offset in sorted({0,size-1,*range(4096-(va&4095),size,4096)}):
                E.require(walk(va+offset)==pa+offset,'physical independent pointer round-trip differs');points+=1
    objects=struct.unpack('<17Q',bytes.fromhex(proof['objects_hex']))
    raw={n:bytes.fromhex(v) for n,v in proof['originals'].items()}
    retained={n:bytes.fromhex(v) for n,v in proof['retained'].items()};symbols=proof['symbols']
    E.require(set(retained)==set(symbols)=={'g_view','g_map','g_framebuffer','g_font','g_font_header','g_logo'},
              'physical nested retained coverage')
    handoff=struct.unpack_from('<5Q',raw['handoff']);view=struct.unpack_from('<5Q',retained['g_view'])
    E.require(handoff==(objects[1],objects[8],objects[12],objects[15],objects[4]),'physical original nested origins differ')
    E.require(view[0]==symbols['g_framebuffer'] and view[4]==symbols['g_map'] and
              (walk(view[3])==objects[15] if RAM.enabled() else view[3]==objects[15]) and
              view[1]==(symbols['g_font'] if objects[8] else 0) and view[2] in (0,symbols['g_logo']),
              'physical nested boot conversion missing')
    managed='origin=owned_BootInfo_framebuffer' in (directory/'qemu-serial.log').read_text()
    E.require(struct.unpack_from('<2Q',raw['framebuffer'])==(objects[2],objects[3]),'physical framebuffer origin/range differs')
    if managed:
        pointer=struct.unpack_from('<Q',retained['g_framebuffer'])[0]
        E.require(raw['framebuffer'][8:]==retained['g_framebuffer'][8:] and
            0xffffd00000000000<=pointer<pointer+objects[3]<=0xffffd00040000000,
            'owned framebuffer descriptor domain')
        for offset in range(0,objects[3],4096):
            E.require(walk(pointer+offset)==objects[2]+offset,'owned framebuffer physical continuity')
    else:E.require(raw['framebuffer']==retained['g_framebuffer'],'physical identity framebuffer changed')
    original_map=struct.unpack('<4Q',raw['map']);adapted_map=struct.unpack('<4Q',retained['g_map'])
    E.require(original_map[:3]==tuple(objects[5:8]) and original_map[1:]==adapted_map[1:] and
              adapted_map[0]!=original_map[0] and walk(adapted_map[0])==adapted_map[0]-0xffffffff80000000,
              'physical immutable map copy domain mismatch')
    if objects[8]:
        E.require(struct.unpack('<2Q',raw['font'])==(objects[9],objects[10]) and
                  struct.unpack('<2Q',retained['g_font'])==(symbols['g_font_header'],objects[10]+(RAM.BASE if RAM.enabled() else 0)) and
                  raw['font_header']==retained['g_font_header'] and
                  (512 if raw['font_header'][2]&1 else 256)*raw['font_header'][3]==objects[11],
                  'physical nested glyph/header conversion missing')
    if objects[12]:
        E.require(raw['logo'][:16]==retained['g_logo'][:16] and
                  struct.unpack_from('<Q',retained['g_logo'],16)[0]==objects[13]+(RAM.BASE if RAM.enabled() else 0) and
                  struct.unpack_from('<Q',raw['logo'],16)[0]==objects[13] and
                  struct.unpack_from('<I',raw['logo'],8)[0]==objects[14],'physical logo retained origin differs')
    E.require(len(raw['rsdp'])==objects[16] and raw['rsdp'][:8]==b'RSD PTR ','physical RSDP extent/provenance')
    return {'status':'PASS','mapped_endpoints':points,'nested_boot_objects':'RAW_ORIGINAL_AND_ADAPTED',
            'root':proof['root_physical'],'retired_logo_pixels':'NOT_DEREFERENCED'}

def verify_run(directory):
    directory=Path(directory);tlb=module('verify-tlb-shootdown').verify_run(directory)
    lines=(directory/'qemu-serial.log').read_text().splitlines();topology={}
    for line in lines:
        m=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if m:topology[int(m[1])]=(int(m[2]),m[3])
    early=boot(lines);rows=[json.loads(line) for line in (directory/'framed.jsonl').read_text().splitlines()]
    ram=RAM.activation(lines) if RAM.enabled() else None
    current=json.loads((directory/'candidate.json').read_text());results=[]
    if tlb['policy']['observation'].get('production'):
        production((directory/'symbols.txt').read_text(),(directory/'compiled-strings.txt').read_text())
        E.require(any(r['payload']=='phystest' and r['handler_status']==127 for r in rows),'physical production rejection missing')
    else:
        O=module('verify-memory-observation')
        for row in rows:
            if row['payload']=='phystest':
                results.append(fixture(O.FOUNDATION.transaction_segment(lines,row),topology,tlb['policy']['root'],current['symbols']['_kernel_start'],ram=ram))
        E.require(len(results)==2,'physical repeated fixture coverage')
        E.require(all(number(r['accounting'],'ceiling')==(ram['ceiling'] if ram else number(early,'ceiling')) for r in results),'physical boot/runtime ceiling differs')
        if json.loads((directory/'result.json').read_text())['profile']=='q35-tcg-smp1':
            E.require(all(number(r['accounting'],'represented_frames')==268435456 for r in results),'full PMM represented span shortened')
    result=json.loads((directory/'result.json').read_text())
    independent=readback(directory,result,results) if result['readback'] else None
    return {'status':'PASS','invalidation':tlb,'boot':early,'fixtures':results,'independent':independent}

if __name__=='__main__':
    print(json.dumps(verify_run(Path(sys.argv[1])),sort_keys=True))
