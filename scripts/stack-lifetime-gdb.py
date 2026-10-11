"""Read one actual task stack before removal and after acknowledged retirement.

Hardware execution breakpoints are installed by the exact host script. This
reader never calls a guest function or writes guest memory or registers.
"""
import gdb
import hashlib
import json
from pathlib import Path
import struct

cfg=json.loads(Path(STACK_LIFETIME_CONFIG).read_text());phase=STACK_LIFETIME_PHASE
directory=Path(cfg['output']);symbols=cfg['symbols'];inferior=gdb.selected_inferior()
def read(address,size):
    if not 0<size<=1048576:raise ValueError('stack lifetime bounded read')
    return bytes(inferior.read_memory(address,size))
def word(data,at,size=8):return int.from_bytes(data[at:at+size],'little')
def integer(name,size=8):return word(read(symbols[name],size),0,size)
def register(name):return int(gdb.parse_and_eval('$'+name))
if phase not in ('before','after'):raise ValueError('stack lifetime phase')
stop=symbols['kernel_stack_retire'] if phase=='before' else cfg['return_site']
if register('rip')!=stop:raise ValueError('stack lifetime incorrect hardware stop')
if phase=='before':
    identity={'generation':register('rdi'),'slot':register('rsi')&0xffffffff,
              'task':register('rdx'),'lifecycle':register('rcx')}
else:identity=json.loads((directory/'stack-lifetime-before.json').read_text())['identity']
if not 128<=identity['slot']<1664 or not all(identity[k] for k in ('generation','task','lifecycle')):
    raise ValueError('stack lifetime invalid task handle')
owner_address=symbols['g_stack_owners']+identity['slot']*400
owner=read(owner_address,400);sequence,*cells=struct.unpack('<50Q',owner)
if sequence&1 or cells[0]!=identity['generation'] or cells[5]!=identity['task'] or cells[6]!=identity['lifecycle']:
    raise ValueError('stack lifetime inconsistent owner identity')
if cells[13]!=(5 if phase=='before' else 0) or cells[14]!=1 or cells[16]!=1:
    raise ValueError('stack lifetime incorrect owner state')
guard,base,end,backing=cells[1:5]
if guard!=0xffffe00000000000+identity['slot']*65536 or base!=guard+4096 or end!=base+16384:
    raise ValueError('stack lifetime payload geometry')
root=integer('g_kernel_root_physical')
if register('cr3')!=root:raise ValueError('stack lifetime wrong current root')
map_header=read(symbols['g_map'],32);pointer,size,stride,_=struct.unpack('<4Q',map_header)
if not 40<=stride<=4096 or not 0<size<=1048576 or size%stride:raise ValueError('stack lifetime firmware map extent')
map_bytes=read(pointer,size);ranges=[struct.unpack_from('<IIQQQQ',map_bytes,at) for at in range(0,size,stride)]
def physical(pa):
    if pa&4095 or not any(kind in (1,2,3,4,5,6,7,9,10) and first<=pa and pa+4096<=first+pages*4096
        for kind,_,first,_,pages,_ in ranges):raise ValueError('stack lifetime table outside descriptor-selected RAM')
    answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:1',to_string=True)
    if 'received: "OK"' not in answer:raise ValueError('stack lifetime physical mode unavailable')
    try:return read(pa,4096)
    finally:
        answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:0',to_string=True)
        if 'received: "OK"' not in answer:raise ValueError('stack lifetime virtual mode restore failed')
frames={};mask=0x000ffffffffff000
for va in range(guard,end,4096):
    table=root;seen=set()
    for level in (4,3,2,1):
        if table in seen:raise ValueError('stack lifetime page table cycle')
        seen.add(table)
        if table not in frames:frames[table]=physical(table)
        entry=word(frames[table],((va>>(12+9*(level-1)))&511)*8)
        if not entry&1 or level==1 or level in (2,3) and entry&128:break
        table=entry&mask
if len(frames)>8:raise ValueError('stack lifetime excessive path tables')
addresses=sorted(frames);data=b''.join(frames[pa] for pa in addresses)
audit=b'';completed=integer('g_audit_completed')
if phase=='after':
    generation=cells[7]
    if not 0<generation<=completed<=8192:raise ValueError('stack lifetime ACK not published')
    audit=read(symbols['g_audit']+(generation-1)*cfg['layout']['audit']['size'],cfg['layout']['audit']['size'])
bits=[]
for pa in range(backing,backing+16384,4096):
    fields={}
    for name in ('g_bitmap_buffer','g_reserved_bitmap_buffer'):
        bitmap=integer(name)
        if not 0xffff800000000000<=bitmap<0xffffc00000000000:raise ValueError('stack lifetime bitmap CPU domain')
        index=pa//4096;at=bitmap+index//8;value=read(at,1)[0]
        fields[name]={'pointer':bitmap,'byte_address':at,'byte':value,'bit':(value>>(index%8))&1}
    bits.append({'physical':pa,'fields':fields})
if read(owner_address,400)!=owner:raise ValueError('stack lifetime owner changed during all-stop capture')
prefix='stack-lifetime-'+phase
blobs={'owner':owner,'tables':data,'audit':audit,'memory-map':map_bytes}
for name,value in blobs.items():(directory/(prefix+'-'+name+'.bin')).write_bytes(value)
proof={'schema':1,'phase':phase,'candidate_id':cfg['candidate_id'],'vm_id':cfg['vm_id'],'boot_id':cfg['boot_id'],
    'root':root,'identity':identity,'rip':register('rip'),'rsp':register('rsp'),'cr0':register('cr0'),
    'cr4':register('cr4'),'efer':register('efer'),'owner_address':owner_address,'frame_addresses':addresses,
    'map_stride':stride,'audit_completed':completed,'allocation_bits':bits,
    'blobs':{name:{'size':len(value),'sha256':hashlib.sha256(value).hexdigest()} for name,value in blobs.items()},
    'memory_access':'QEMU_PHYSICAL_PACKET_VERIFIED','authority':'READ_ONLY_BEFORE_REMOVAL_AND_AFTER_ACK; NO_RETIRED_PAYLOAD_READ'}
(directory/(prefix+'.json')).write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
print('STACK_LIFETIME_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))
