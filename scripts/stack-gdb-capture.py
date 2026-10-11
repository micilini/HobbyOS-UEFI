"""Stopped HobbyOS stack owners and hardware registers; no guest mutation."""
import gdb
import hashlib
import json
from pathlib import Path
import re
import struct
cfg=json.loads(Path(STACK_CAPTURE_CONFIG).read_text())
directory=Path(cfg['output']);symbols=cfg['symbols'];inferior=gdb.selected_inferior()
production=bool(cfg.get('stack_production'))
meta=json.loads((directory/('production-memory.json' if production else 'table-snapshot.json')).read_text())
layout=cfg['stack_layout']
if layout!={'schema':1,'records':1665,'record_bytes':400,'sequence':0,'cells':8,
    'cell_count':49,'cpu_bytes':48,'tss_bytes':104,'cpu_max':32}:raise ValueError('stack scalar capture layout')
def read(address,size):
    if not 0<size<=1048576:raise ValueError('stack capture extent')
    return bytes(inferior.read_memory(address,size))
def integer(name,size=8):return int.from_bytes(read(symbols[name],size),'little')
count=integer('g_cpu_count',4)
if not 0<count<=32:raise ValueError('stack capture CPU count')
sizes={'g_stack_owners':1665*400,'g_stack_phase':4,'g_stack_count_sequence':8,
       'g_stack_allocated':8,'g_stack_retired':8,'g_stack_quarantined':8,'g_stack_high_water':4,
       'g_cpus':count*48,'g_cpu_count':4}
objects={name:read(symbols[name],size) for name,size in sizes.items()}
if int.from_bytes(objects['g_stack_count_sequence'],'little')&1:raise ValueError('stack capture interrupted accounting writer')
owners=objects['g_stack_owners']
if any(struct.unpack_from('<Q',owners,i*400)[0]&1 for i in range(1665)):raise ValueError('stack capture interrupted owner writer')
raw=(directory/('production-table-frames.bin' if production else 'table-frames.bin')).read_bytes()
frames={pa:raw[i*4096:(i+1)*4096] for i,pa in enumerate(meta['frame_addresses'])}
root=meta['active_root'];mask=0x000ffffffffff000
def translate(address,size):
    if size<1 or (address&4095)+size>4096:raise ValueError('stack capture translation extent')
    table=root
    for level in (4,3,2,1):
        if table not in frames:raise ValueError('stack capture missing actual table')
        entry=struct.unpack_from('<Q',frames[table],((address>>(12+9*(level-1)))&511)*8)[0]
        if not entry&1:raise ValueError('stack capture missing payload translation')
        if level==1 or level in (2,3) and entry&128:
            span=1<<(12+9*(level-1));return (entry&mask&~(span-1))+(address&(span-1))
        table=entry&mask
    raise ValueError('stack capture translation depth')
def physical(address,size):
    answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:1',to_string=True)
    if 'received: "OK"' not in answer:raise ValueError('stack physical read mode unavailable')
    try:return read(address,size)
    finally:
        answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:0',to_string=True)
        if 'received: "OK"' not in answer:raise ValueError('stack virtual mode restore failed')
children=[]
for slot in range(count):
    cpu=objects['g_cpus'][slot*48:(slot+1)*48];tss=struct.unpack_from('<Q',cpu,32)[0]
    data=read(tss,104);pa=translate(tss,104)
    if data!=physical(pa,104):raise ValueError('stack TSS virtual and physical bytes differ')
    children.append({'slot':slot,'tss':tss,'physical':pa,'bytes':data.hex()})
descriptors=gdb.execute('monitor info registers -a',to_string=True)
(directory/'stack-descriptors.log').write_text(descriptors)
original=gdb.selected_thread();threads=gdb.execute('info threads',to_string=True)
identities={r['cpu_index']:r for r in cfg['cpu_identity']};descriptions={}
for line in threads.splitlines():
    m=re.match(r'^\s*\*?\s*(\d+)\s+Thread \d+\.\d+ \((CPU#\d+ \[(?:running|halted) ?\])\)',line)
    if m:descriptions[int(m[1])]=m[2]
registers=[]
for thread in sorted(inferior.threads(),key=lambda t:t.num):
    name=descriptions.get(thread.num,'');m=re.fullmatch(r'CPU#(\d+) \[(?:running|halted) ?\]',name)
    if not m or int(m[1]) not in identities:raise ValueError('stack debugger CPU identity')
    index=int(m[1]);thread.switch()
    values={n:int(gdb.parse_and_eval('$'+n)) for n in ('rip','rsp','cr0','cr3','cr4','efer')}
    registers.append({'cpu_index':index,'apic_id':identities[index]['apic_id'],
        'thread':thread.num,'name':name,'registers':values})
if original:original.switch()
if len(registers)!=count:raise ValueError('stack debugger CPU coverage')
for name,data in objects.items():
    if read(symbols[name],len(data))!=data:raise ValueError('stack owner changed during stopped snapshot')
    (directory/('stack-owner-'+name+'.bin')).write_bytes(data)
proof={'schema':1,'candidate_id':cfg['candidate_id'],'vm_id':cfg['vm_id'],'root':root,
    'layout':layout,'cpu_count':count,'objects':{name:{'address':symbols[name],'size':len(data),
        'sha256':hashlib.sha256(data).hexdigest()} for name,data in objects.items()},
    'children':children,'registers':registers,'descriptors_sha256':hashlib.sha256(descriptors.encode()).hexdigest(),
    'frames_sha256':meta['frames_sha256'],'access':'STOPPED_READ_ONLY_ACTUAL_TABLES_AND_TSS',
    'authority':'Independent current owner/table/register observation; no timing or hardware acceptance.'}
(directory/'stack-owner-readback.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
print('STACK_OWNER_CAPTURE_COMPLETE cpus=%d records=1665' % count)
