"""Additional stopped pointer-owner capture; no guest calls or writes."""
import gdb
import hashlib
import json
from pathlib import Path
import struct
cfg=json.loads(Path(MEMORY_CAPTURE_CONFIG).read_text())
directory=Path(cfg['output']);symbols=cfg['symbols'];inferior=gdb.selected_inferior()
meta=json.loads((directory/'table-snapshot.json').read_text())
bootstrap=meta['bootstrap_root']
def read(address,size):
    if not 0<size<=1048576:raise ValueError('RAM owner read bound')
    return bytes(inferior.read_memory(address,size))
def integer(name,size=8):return int.from_bytes(read(symbols[name],size),'little')
bootstrap_pointer=integer('g_observer_boot_root')
ranges=[struct.unpack_from('<IIQQQQ',bytes.fromhex(meta['memory_map_hex']),i)
    for i in range(0,len(bytes.fromhex(meta['memory_map_hex'])),meta['descriptor_stride'])]
def ram(address,size):
    if not any(t in (1,2,3,4,5,6,7,9,10) and start<=address and address+size<=start+pages*4096
        for t,_,start,_,pages,_ in ranges):raise ValueError('RAM owner physical extent')
    answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:1',to_string=True)
    if 'received: "OK"' not in answer:raise ValueError('RAM owner physical mode unavailable')
    try:return read(address,size)
    finally:
        answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:0',to_string=True)
        if 'received: "OK"' not in answer:raise ValueError('RAM owner virtual mode restore')

if cfg.get('ram_layout'):
    layout=cfg['ram_layout'];count=integer('g_cpu_count',4)
    if not 1<=count<=1024:raise ValueError('RAM CPU owner capture bound')
    sizes={'g_cpus':count*layout['cpu']['size'],'g_owned_window':layout['window']['size'],
        'g_boot_build':layout['build']['size'],'g_bitmap':layout['bitmap']['size_bytes'],
        'g_reserved_bitmap':layout['bitmap']['size_bytes'],'g_bitmap_buffer':8,'g_reserved_bitmap_buffer':8}
    raw={name:read(symbols[name],size) for name,size in sizes.items()}
    objects={}
    for name,data in raw.items():
        (directory/('ram-owner-'+name+'.bin')).write_bytes(data)
        objects[name]={'address':symbols[name],'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    children=[]
    def word(data,offset,size=8):return int.from_bytes(data[offset:offset+size],'little')
    def owned_read(address,size):
        if cfg.get('stack_layout') and 0xffffe00000000000<=address<0xffffe00006800000:
            frames_raw=(directory/'table-frames.bin').read_bytes()
            frames={pa:frames_raw[i*4096:(i+1)*4096] for i,pa in enumerate(meta['frame_addresses'])}
            table=meta['active_root'];physical=None
            for level in (4,3,2,1):
                if table not in frames:raise ValueError('RAM stack inverse missing table')
                entry=word(frames[table],((address>>(12+9*(level-1)))&511)*8)
                if not entry&1 or level>1 and entry&128:raise ValueError('RAM stack payload absent/large leaf')
                if level==1:physical=(entry&0x000ffffffffff000)+(address&4095)
                else:table=entry&0x000ffffffffff000
            if physical is None or (address&4095)+size>4096:raise ValueError('RAM stack inverse extent')
        elif 0xffff800000000000<=address<0xffffc00000000000:physical=address-0xffff800000000000
        else:raise ValueError('RAM retained CPU pointer has no admitted owner domain')
        # Compare real reads through the CPU pointer with its physical backing.
        actual=read(address,size);backing=ram(physical,size)
        if actual!=backing:raise ValueError('RAM owner virtual and physical reads differ')
        return actual
    for slot in range(count):
        cpu=raw['g_cpus'][slot*layout['cpu']['size']:(slot+1)*layout['cpu']['size']]
        if cpu[layout['cpu']['is_bsp']]:continue
        tss=word(cpu,layout['cpu']['tss_ptr']);gdt=word(cpu,layout['cpu']['gdt_ptr']);stack=word(cpu,layout['cpu']['stack_top'])
        tss_bytes=owned_read(tss,layout['tss']['size']);gdt_bytes=owned_read(gdt,layout['gdt']['size'])
        stack_first=owned_read(stack-layout['cpu']['stack_bytes'],8)
        ist=[]
        for name in ('ist1','ist2','ist3'):
            top=word(tss_bytes,layout['tss'][name]);ist.append({'top':top,'first_hex':owned_read(top-32768,8).hex()})
        children.append({'slot':slot,'tss':tss,'tss_hex':tss_bytes.hex(),'gdt':gdt,'gdt_hex':gdt_bytes.hex(),
            'stack_top':stack,'stack_first_hex':stack_first.hex(),'ist':ist})
    proof={'schema':1,'candidate_id':cfg['candidate_id'],'vm_id':cfg['vm_id'],'root':meta['active_root'],
        'cpu_count':count,'layout':layout,'objects':objects,'children':children,
        'access':'STOPPED_VIRTUAL_AND_PHYSICAL_READS_EQUAL','bootstrap_root':bootstrap,
        'bootstrap_pointer':bootstrap_pointer}
    (directory/'ram-owner-readback.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
    print('RAM_OWNER_CAPTURE_COMPLETE cpus=%d children=%d' % (count,len(children)))
