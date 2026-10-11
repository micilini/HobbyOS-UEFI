"""Bounded read-only production capture; no guest observer or function calls."""
import gdb
import hashlib
import json
from pathlib import Path
import struct

cfg=json.loads(Path(DEVICE_PRODUCTION_CONFIG).read_text())
directory=Path(cfg['output']);symbols=cfg['symbols'];inferior=gdb.selected_inferior()
def read(address,size):
    if not 0<size<=1048576:raise ValueError('production capture read bound')
    return bytes(inferior.read_memory(address,size))
sizes={'g_views':112*512,'g_dma':72*2048,'xhci_driver':145376,'g_framebuffer':32,
       'g_objects':17*8,'g_map':32,'g_kernel_root_physical':8,'g_kernel_pml4':8}
objects={name:read(symbols[name],size) for name,size in sizes.items()}
base,size,stride,_=struct.unpack('<4Q',objects['g_map'])
if not 40<=stride<=4096 or not 0<size<=1048576 or size%stride:raise ValueError('production firmware map extent')
memory_map=read(base,size)
ranges=[struct.unpack_from('<IIQQQQ',memory_map,i) for i in range(0,size,stride)]
def table(address):
    if address&4095 or not any(1<=t<=10 and start<=address and address+4096<=start+pages*4096
        for t,_,start,_,pages,_ in ranges):raise ValueError('production table outside retained RAM')
    if not cfg.get('direct_map'):return read(address,4096)
    answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:1',to_string=True)
    if 'received: "OK"' not in answer:raise ValueError('production physical read mode unavailable')
    try:return read(address,4096)
    finally:
        answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:0',to_string=True)
        if 'received: "OK"' not in answer:raise ValueError('production virtual mode restore failed')
root=int.from_bytes(objects['g_kernel_root_physical'],'little');cr3=int(gdb.parse_and_eval('$cr3'))
if cr3&0x000ffffffffff000!=root:raise ValueError('production active root mismatch')
frames={};levels={};pending=[(root,4)]
while pending:
    address,level=pending.pop()
    if address in frames:
        if levels[address]!=level:raise ValueError('production table level alias')
        continue
    if len(frames)>=8192:raise ValueError('production traversal bound')
    raw=table(address);frames[address]=raw;levels[address]=level
    if level==1:continue
    for entry, in struct.iter_unpack('<Q',raw):
        if entry&1 and not(level in (2,3) and entry&128):pending.append((entry&0x000ffffffffff000,level-1))
addresses=sorted(frames);data=b''.join(frames[a] for a in addresses)
(directory/'production-table-frames.bin').write_bytes(data)
for name,raw in objects.items():(directory/('production-'+name+'.bin')).write_bytes(raw)
proof={'schema':1,'candidate_id':cfg['candidate_id'],'vm_id':cfg['vm_id'],'cr3':cr3,'active_root':root,
    'frame_addresses':addresses,'frame_levels':[levels[a] for a in addresses],
    'memory_access':'QEMU_PHYSICAL_PACKET_VERIFIED' if cfg.get('direct_map') else 'IDENTITY_VIRTUAL',
    'frames_sha256':hashlib.sha256(data).hexdigest(),'descriptor_stride':stride,'memory_map_hex':memory_map.hex(),
    'objects':{n:{'address':symbols[n],'size':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in objects.items()},
    'authority':'Stopped production read-only RAM/table capture after ordinary keyboard commands; no timing authority.'}
(directory/'production-memory.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
print('PRODUCTION_MEMORY_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))
