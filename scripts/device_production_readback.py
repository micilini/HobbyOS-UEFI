"""Collect and replay actual owned mappings in an ordinary production binary."""
import json
from pathlib import Path
import struct
import memory_evidence as E
import device_mapping_evidence as D
import direct_map_evidence as RAM
import low_current as LOW
import kernel_image_current as WX
import stack_current as STACK

SIZES={'g_views':112*512,'g_dma':72*2048,'xhci_driver':145376,'g_framebuffer':32,
       'g_objects':17*8,'g_map':32,'g_kernel_root_physical':8,'g_kernel_pml4':8}

STACK_NAMES={'g_stack_owners','g_stack_phase','g_stack_count_sequence','g_stack_allocated',
    'g_stack_retired','g_stack_quarantined','g_stack_high_water','g_cpus','g_cpu_count'}
def collect(root,directory,runtime,candidate,result,run,*,cpu_identity=None):
    needed=set(SIZES)|(STACK_NAMES if STACK.enabled() else set())
    symbols={s[2]:int(s[0],16) for line in (directory/'symbols.txt').read_text().splitlines()
             if len(s:=line.split())==3 and s[2] in needed}
    E.require(set(symbols)==needed,'production mapping symbol coverage')
    cfg={'output':str(directory),'symbols':symbols,'candidate_id':result['candidate_id'],'vm_id':result['vm_id']}
    if RAM.enabled():cfg['direct_map']=True
    if STACK.enabled():
        run('stack-layout-build',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
            'scripts/stack-readback-layout.c','-o',str(directory/'stack-layout')])
        run('stack-layout',[str(directory/'stack-layout')])
        cfg.update(stack_layout=json.loads((directory/'stack-layout.stdout.log').read_text()),
            stack_production=True,cpu_identity=cpu_identity)
    E.write(directory/'production-capture-config.json',cfg)
    source=directory/'production-capture.py';source.write_bytes((root/'scripts/device-production-gdb.py').read_bytes())
    script=directory/'production-memory.gdb'
    lines=['set pagination off','set confirm off','target remote '+str(runtime/'gdb.sock'),
        f'python exec(compile(open({json.dumps(str(source))}).read(), "production-memory", "exec"), dict(DEVICE_PRODUCTION_CONFIG={json.dumps(str(directory/"production-capture-config.json"))}))']
    if STACK.enabled():
        target=directory/'stack-capture.py';target.write_bytes((root/'scripts/stack-gdb-capture.py').read_bytes())
        lines+=[f'python exec(compile(open({json.dumps(str(target))}).read(), "stack-owner-capture", "exec"), dict(STACK_CAPTURE_CONFIG={json.dumps(str(directory/"production-capture-config.json"))}))']
    script.write_text('\n'.join(lines+['detach','quit'])+'\n')
    run('production-memory',['gdb','-q','-batch','-x',str(script),str(candidate/'kernel.elf')])

def verify(directory):
    result=json.loads((directory/'result.json').read_text());proof=json.loads((directory/'production-memory.json').read_text())
    cfg=json.loads((directory/'production-capture-config.json').read_text());launch=json.loads((directory/'launch.json').read_text())
    E.require(result['production'] and proof['candidate_id']==cfg['candidate_id']==result['candidate_id'] and
        proof['vm_id']==cfg['vm_id']==result['vm_id'],'production readback candidate/VM binding')
    E.require(E.sha(directory/'production-capture.py')==E.sha(E.ROOT/'scripts/device-production-gdb.py'),
              'production capture source changed')
    commands=[json.loads(l) for l in (directory/'commands.jsonl').read_text().splitlines()]
    captured=[r for r in commands if r['name']=='production-memory']
    E.require(len(captured)==1 and captured[0]['exit_status']==0 and captured[0]['argv']==
        ['gdb','-q','-batch','-x',str(Path(cfg['output'])/'production-memory.gdb'),launch['elf']['path']],
        'production read-only debugger command')
    script=(directory/'production-memory.gdb').read_text().splitlines()
    expected_script=['set pagination off','set confirm off','target remote '+str(Path(result['runtime_session'])/'r/gdb.sock'),
        f'python exec(compile(open({json.dumps(str(Path(cfg["output"])/"production-capture.py"))}).read(), "production-memory", "exec"), dict(DEVICE_PRODUCTION_CONFIG={json.dumps(str(Path(cfg["output"])/"production-capture-config.json"))}))']
    if STACK.enabled():
        expected_script+=[f'python exec(compile(open({json.dumps(str(Path(cfg["output"])/"stack-capture.py"))}).read(), "stack-owner-capture", "exec"), dict(STACK_CAPTURE_CONFIG={json.dumps(str(Path(cfg["output"])/"production-capture-config.json"))}))']
    E.require(script==expected_script+['detach','quit'],'production readback command must be exact/read-only')
    E.require((directory/'production-memory.stdout.log').read_text().splitlines().count(
        'PRODUCTION_MEMORY_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))==1,'production raw GDB record missing')
    needed=set(SIZES)|(STACK_NAMES if STACK.enabled() else set())
    symbols={s[2]:int(s[0],16) for line in (directory/'symbols.txt').read_text().splitlines()
        if len(s:=line.split())==3 and s[2] in needed}
    E.require(cfg['symbols']==symbols and set(symbols)==needed and set(proof['objects'])==set(SIZES),'production current ELF symbols')
    objects={}
    for name,size in SIZES.items():
        path=directory/('production-'+name+'.bin');objects[name]=path.read_bytes();r=proof['objects'][name]
        E.require(r=={'address':symbols[name],'size':size,'sha256':E.sha(path)} and len(objects[name])==size,'production object bytes')
    ram=RAM.activation((directory/'qemu-serial.log').read_text().splitlines()) if RAM.enabled() else None
    E.require(bool(cfg.get('direct_map'))==bool(ram),'production readback address contract')
    root=proof['active_root'];E.require(root==int.from_bytes(objects['g_kernel_root_physical'],'little')==
        int.from_bytes(objects['g_kernel_pml4'],'little')-(RAM.BASE if ram else 0)==proof['cr3']&D.MASK,'production physical root')
    addresses=proof['frame_addresses'];raw=(directory/'production-table-frames.bin').read_bytes()
    E.require(addresses==sorted(set(addresses)) and 0<len(addresses)<=8192 and len(raw)==4096*len(addresses) and
        E.sha(directory/'production-table-frames.bin')==proof['frames_sha256'],'production raw table completeness')
    frames={a:raw[i*4096:(i+1)*4096] for i,a in enumerate(addresses)}
    if LOW.enabled():E.require(frames[root][:2048]==bytes(2048),'production main root contains lower-half mapping')
    _,map_size,stride,_=struct.unpack('<4Q',objects['g_map'])
    memory_map=bytes.fromhex(proof['memory_map_hex'])
    E.require(40<=stride<=4096 and 0<map_size<=1048576 and map_size%stride==0 and
        len(memory_map)==map_size and proof['descriptor_stride']==stride,'production retained firmware map')
    ranges=[struct.unpack_from('<IIQQQQ',memory_map,i) for i in range(0,map_size,stride)]
    E.require(all(not start&4095 and pages>0 and start+pages*4096<=1<<64
        for _,_,start,_,pages,_ in ranges),'production firmware descriptor arithmetic')
    E.require(all(any(1<=kind<=10 and start<=address and address+4096<=start+pages*4096
        for kind,_,start,_,pages,_ in ranges) for address in addresses),'production table physical provenance')
    import memory_table_proof as T
    levels=T.traverse(root,frames,ram['physical_bits'] if ram else 48,True)
    E.require(set(levels)==set(addresses) and proof['frame_levels']==[levels[a] for a in addresses],'production unique table traversal')
    def leaf(va):
        table=root
        for level in (4,3,2,1):
            entry=struct.unpack_from('<Q',frames[table],((va>>(12+9*(level-1)))&511)*8)[0]
            if not entry&1:return 0,level
            if level==1 or level in (2,3) and entry&128:return entry,level
            table=entry&D.MASK
        raise ValueError('production walk depth')
    origin=struct.unpack('<17Q',objects['g_objects']);fb,fbsize=origin[2:4]
    cpu,size=struct.unpack_from('<QQ',objects['g_framebuffer']);E.require(size==fbsize,'production framebuffer adapted extent')
    active=[];physical={};framebuffer=[]
    for slot in range(512):
        b=objects['g_views'][slot*112:(slot+1)*112];state,kind=struct.unpack_from('<II',b,100)
        if state==0:continue
        generation,index,domain,va,pa,count=struct.unpack_from('<QIIQQQ',b);flags=struct.unpack_from('<Q',b,80)[0]
        E.require(state==2 and index==slot and domain==1 and generation>0 and count>0 and va&4095==pa&4095,'production live view')
        nx=(1<<63) if WX.enabled() and ram['nx'] else 0
        E.require(flags==1|nx if kind==0 else flags==0x8b|nx if kind==1 else flags in (0x13|nx,0x1b|nx),'production owned cache/permission')
        if kind==1:framebuffer.append((va,pa,count))
        for off in range(0,((pa&4095)+count+4095)&~4095,4096):
            addr=(va&~4095)+off;p=(pa&~4095)+off;e,level=leaf(addr)
            E.require(level==1 and e&~0x60==p|flags,'production actual device/firmware leaf')
            old,_=leaf(p)
            E.require(old==0 if kind else not old or old&0x98==0,'production incompatible identity alias')
            E.require(p not in physical or physical[p]==flags&0x98,'production incompatible managed aliases');physical[p]=flags&0x98
        active.append(slot)
    E.require(framebuffer==[(cpu,fb,fbsize)],'production framebuffer WC view not activated')
    for table,level in levels.items():
        for entry, in struct.iter_unpack('<Q',frames[table]):
            if not entry&1:continue
            is_leaf=level==1 or level in (2,3) and entry&128
            slot=(entry>>3)&3|(((entry>>(7 if level==1 else 12))&1)<<2 if is_leaf else 0)
            if slot==5:E.require(is_leaf and level==1 and fb&~4095<=entry&D.MASK<(fb+fbsize+4095)&~4095,'production unexpected WC')
    dma=0
    for slot in range(2048):
        b=objects['g_dma'][slot*72:(slot+1)*72]
        if not b[68]:continue
        generation,index,domain,va,pa,maximum,requested,backing,alignment=struct.unpack_from('<QII6Q',b)
        E.require(generation>0 and index==slot and domain==2 and not b[69] and va==pa+(RAM.BASE if ram else 0) and 0<requested<=backing and
            backing%4096==0 and pa+backing-1<=maximum and alignment>0 and not alignment&(alignment-1) and
            pa%max(alignment,4096)==0,'production DMA ownership/constraint');e,level=leaf(va)
        leaf_bytes=1<<(12+9*(level-1))
        E.require(((e&D.MASK)&~(leaf_bytes-1))+(va&(leaf_bytes-1))==pa and e&0x18==0 and
            not(e&(128 if level==1 else 4096)),'production DMA WB mapping');dma+=1
    E.require(dma>0,'production devices did not allocate DMA')
    protection=None
    if WX.enabled():
        import kernel_image_evidence as W
        coverage=[]
        for kind,_,first,_,pages,_ in ranges:
            if kind not in (1,2,3,4,5,6,7,9,10):continue
            end=first+pages*4096;a=fb&~4095;b=(fb+fbsize+4095)&~4095
            if first<b and a<end:
                if first<a:coverage.append((first,a))
                if b<end:coverage.append((b,end))
            else:coverage.append((first,end))
        W.boot(directory,ram)
        protection=W.physical_tables(directory,ram,RAM.merge(coverage),production=True)
    return {'image_protection':protection,'views':len(active),'frames':len(addresses),'dma_objects':dma,'framebuffer_pages':(fbsize+(fb&4095)+4095)//4096,
        'selection':'PAT_SELECTED_WC','effective_type':'NOT_COMPUTED','authority':proof['authority']}
