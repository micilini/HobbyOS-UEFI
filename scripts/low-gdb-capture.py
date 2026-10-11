"""Read physical bootstrap bytes before AP launch, or retired owner bytes later."""
import gdb
import hashlib
import json
from pathlib import Path
import struct
cfg=json.loads(Path(LOW_CAPTURE_CONFIG).read_text())
directory=Path(cfg['output']);symbols=cfg['symbols'];layout=cfg['layout'];phase=cfg['phase']
inferior=gdb.selected_inferior()
def read(address,size):
    if not 0<size<=1024*1024:raise ValueError('low-domain read bound')
    return bytes(inferior.read_memory(address,size))
def physical(address,size):
    if address&4095 or not 0<address<1<<32:raise ValueError('bootstrap physical extent')
    answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:1',to_string=True)
    if 'received: "OK"' not in answer:raise ValueError('physical read mode unavailable')
    try:return read(address,size)
    finally:
        answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:0',to_string=True)
        if 'received: "OK"' not in answer:raise ValueError('physical read mode restore')
def word(data,offset,size=8):return int.from_bytes(data[offset:offset+size],'little')
objects={};raw={}
for name,size in {'g_bootstrap_tables':layout['tables']['size'],'g_bootstrap_life':layout['life']['size'],
    'g_bootstrap_snapshot':layout['snapshot']['size'],'g_bootstrap_freed':8,
    'g_bootstrap_free_before':8,'g_bootstrap_free_after':8,'g_observer_boot_root':8,
    'g_kernel_root_physical':8,'g_free_frames':8}.items():
    data=read(symbols[name],size);raw[name]=data
    path=directory/(phase+'-'+name+'.bin');path.write_bytes(data)
    objects[name]={'address':symbols[name],'size':size,'sha256':hashlib.sha256(data).hexdigest()}
root=word(raw['g_kernel_root_physical'],0)
main=physical(root,4096);(directory/(phase+'-main-root.bin')).write_bytes(main)
result={'schema':1,'phase':phase,'candidate_id':cfg['candidate_id'],'vm_id':cfg['vm_id'],
    'boot_id':cfg['boot_id'],'root':root,'cr3':int(gdb.parse_and_eval('$cr3')),
    'rip':int(gdb.parse_and_eval('$rip')),'rsp':int(gdb.parse_and_eval('$rsp')),
    'objects':objects,'main_sha256':hashlib.sha256(main).hexdigest(),'layout':layout}
if phase=='bootstrap-before':
    tables=raw['g_bootstrap_tables'];life=raw['g_bootstrap_life']
    if word(tables,layout['tables']['count'],4)!=4 or word(life,layout['life']['state'],4)!=1:
        raise ValueError('bootstrap capture is not before AP launch')
    if int(gdb.parse_and_eval('$rip'))!=symbols['paging_bootstrap_start']:
        raise ValueError('bootstrap diagnostic stop at wrong site')
    addresses=[word(tables,i*16+8) for i in range(4)]
    data=b''.join(physical(a,4096) for a in addresses)
    (directory/'bootstrap-before-private.bin').write_bytes(data)
    result['private_addresses']=addresses;result['private_sha256']=hashlib.sha256(data).hexdigest()
    trampoline=word(tables,layout['tables']['claim']);blob=physical(trampoline,4096)
    (directory/'bootstrap-before-trampoline.bin').write_bytes(blob)
    result['trampoline']=trampoline;result['trampoline_sha256']=hashlib.sha256(blob).hexdigest()
else:
    if phase=='bootstrap-retired' and int(gdb.parse_and_eval('$rip'))!=symbols['kernel_tlb_runtime_init']:
        raise ValueError('retirement diagnostic stop at wrong site')
    if word(raw['g_bootstrap_life'],layout['life']['state'],4)!=3:
        raise ValueError('bootstrap capture is not after retirement')
if phase in ('bootstrap-before','bootstrap-retired'):
    previous=result if phase=='bootstrap-before' else json.loads((directory/'bootstrap-before.json').read_text())
    owners=[]
    for pa in [*previous['private_addresses'],previous['trampoline']]:
        bits={}
        for name in ('g_bitmap_buffer','g_reserved_bitmap_buffer'):
            pointer=word(read(symbols[name],8),0)
            if not 0xffff800000000000<=pointer<0xffffc00000000000:raise ValueError('bitmap owner domain')
            index=pa//4096;address=pointer+index//8;byte=read(address,1)[0]
            bits[name]={'pointer':pointer,'byte_address':address,'byte':byte,'value':(byte>>(index%8))&1}
        owners.append({'physical':pa,'bits':bits})
    result['allocation_bits']=owners
path=directory/(phase+'.json');path.write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print('LOW_ROOT_GDB '+json.dumps(result,sort_keys=True,separators=(',',':')))
