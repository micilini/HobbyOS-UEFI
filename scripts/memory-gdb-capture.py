"""Executed by GDB Python while QEMU is stopped; reads only proven RAM tables.

The invoking command defines MEMORY_CAPTURE_CONFIG. No guest function is called,
and no register or guest memory is written. Output files belong to the host.
"""
import gdb
import hashlib
import json
from pathlib import Path
import struct
import re

cfg = json.loads(Path(MEMORY_CAPTURE_CONFIG).read_text())
directory = Path(cfg['output'])
symbols = cfg['symbols']
inferior = gdb.selected_inferior()


def read(address, size):
    if not 0 < size <= 1048576: raise ValueError('bounded capture read size')
    return bytes(inferior.read_memory(address, size))

def physical_read(address,size):
    if not cfg.get('direct_map'):return read(address,size)
    answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:1',to_string=True)
    if 'received: "OK"' not in answer:raise ValueError('QEMU physical read mode unavailable')
    try:return read(address,size)
    finally:
        answer=gdb.execute('maintenance packet Qqemu.PhyMemMode:0',to_string=True)
        if 'received: "OK"' not in answer:raise ValueError('QEMU virtual read mode restore failed')


def integer(name, size=8):
    return int.from_bytes(read(symbols[name], size), 'little')


boot = read(symbols['g_boot'], 64)
base, size, stride, count, framebuffer, framebuffer_bytes, rsdp = struct.unpack_from('<7Q', boot)
if not boot[56] or not (40 <= stride <= 4096 and size == stride * count and 0 < size <= 1048576):
    raise ValueError('invalid retained handoff')
map_raw = physical_read(base, size)
ranges = [struct.unpack_from('<IIQQQQ', map_raw, i) for i in range(0, size, stride)]


def ram(address, size, conventional=False, firmware=False):
    types = (7,) if conventional else tuple(range(0 if firmware else 1, 11))
    if not any(t in types and start <= address and address + size <= start + pages * 4096
               for t, _, start, _, pages, _ in ranges):
        raise ValueError('read outside proven RAM: ' + hex(address))
    return physical_read(address, size)


if not any(t == 2 and start <= base and base + size <= start + pages * 4096
           for t, _, start, _, pages, _ in ranges): raise ValueError('handoff backing descriptor')
allocations = integer('g_observer_count', 4)
limit = integer('g_observer_limit')
if not 0 < allocations <= 8192 or integer('g_observer_failed', 1) or integer('g_observer_nonidentity', 1):
    raise ValueError('observer registry unsafe')
epoch = integer('g_observer_epoch')
registry_raw = read(symbols['g_observer_tables'], allocations * 8)
registry = list(struct.unpack('<' + 'Q' * allocations, registry_raw))
if len(set(registry)) != allocations: raise ValueError('duplicate registry frames')
static = {n: symbols[n] for n in cfg['static_names']}
addresses = sorted(set(registry) | set(static.values()))
chunks = []
for address in addresses:
    if not address or address & 4095 or address + 4096 > limit: raise ValueError('unsafe table address')
    chunks.append(ram(address, 4096, address in registry))
data = b''.join(chunks)
(directory / 'table-frames.bin').write_bytes(data)
guest_count = cfg['reachable']
if not 0 < guest_count <= 8192: raise ValueError('guest enumeration bounds')
bootstrap_pointer=integer('g_observer_boot_root')
bootstrap=bootstrap_pointer
if cfg.get('direct_map') and bootstrap:
    if not 0xffff800000000000<=bootstrap<0xffffc00000000000:raise ValueError('bootstrap table pointer domain')
    bootstrap-=0xffff800000000000
meta = dict(schema=1, candidate_id=cfg['candidate_id'], vm_id=cfg['vm_id'],
    cr3=int(gdb.parse_and_eval('$cr3')), active_root=integer('g_kernel_root_physical') if cfg.get('direct_map') else integer('g_kernel_pml4'),
    bootstrap_root=bootstrap, epoch_before=epoch,
    epoch_after=integer('g_observer_epoch'), allocations=allocations,
    observer_failed=False, observer_nonidentity=False, identity_limit=limit,
    physical_bits=cfg['physical_bits'], page1g=cfg['page1g'], frame_addresses=addresses,
    static_frames=static, registry_hex=registry_raw.hex(), memory_map_hex=map_raw.hex(),
    descriptor_stride=stride, boot_metadata_hex=boot.hex(),
    guest_reachable_hex=read(symbols['g_observer_reachable'], guest_count * 8).hex(),
    frames_sha256=hashlib.sha256(data).hexdigest())
if cfg.get('direct_map'):
    meta['memory_access']='QEMU_PHYSICAL_PACKET_VERIFIED'
    meta['coverage_ceiling']=limit
# Retain bounded ACPI bytes for portable provenance checks, never device pages.
acpi = []
for target in cfg['acpi_targets']:
    address, length = target['base'], target['bytes']
    if length: acpi.append(dict(name=target['name'], base=address, hex=ram(address, length,firmware=True).hex()))
meta['acpi'] = acpi
(directory / 'table-snapshot.json').write_text(json.dumps(meta, sort_keys=True, indent=2) + '\n')
print('MEMORY_CAPTURE_COMPLETE frames=%d cr3=0x%x epoch=%d' % (len(addresses), meta['cr3'], epoch))

if cfg.get('device_layout'):
    layout=cfg['device_layout']
    sizes={'g_views':layout['view']['size']*512,'g_dma':layout['dma']['size']*2048,
           'xhci_driver':layout['controller']['size']}
    owned={}
    for name,size in sizes.items():
        raw=read(symbols[name],size)
        (directory/(name+'-readback.bin')).write_bytes(raw)
        owned[name]={'address':symbols[name],'size':size,'sha256':hashlib.sha256(raw).hexdigest()}
    proof=dict(schema=1,candidate_id=cfg['candidate_id'],vm_id=cfg['vm_id'],
               root=meta['active_root'],layout=layout,objects=owned)
    (directory/'device-readback.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
    print('DEVICE_MEMORY_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))

if cfg.get('physical_readback'):
    objects_raw = read(symbols['g_objects'], 17 * 8)
    objects = struct.unpack('<17Q', objects_raw)
    sizes = {'g_view':304, 'g_map':32, 'g_framebuffer':32,
             'g_font':16, 'g_font_header':4, 'g_logo':24}
    retained = {name:read(symbols[name], size).hex() for name,size in sizes.items()}
    originals = {'handoff':ram(objects[0],304).hex(),
        'framebuffer':ram(objects[1],32).hex(), 'map':ram(objects[4],32).hex(),
        'rsdp':ram(objects[15],objects[16],firmware=True).hex()}
    if objects[8]:
        originals['font'] = ram(objects[8],16).hex()
        originals['font_header'] = ram(objects[9],4).hex()
    if objects[12]: originals['logo'] = ram(objects[12],24).hex()
    proof = dict(schema=1,candidate_id=cfg['candidate_id'],vm_id=cfg['vm_id'],
        root_physical=integer('g_kernel_root_physical'), root_pointer=integer('g_kernel_pml4'),
        cr3=meta['cr3'], objects_hex=objects_raw.hex(), retained=retained, originals=originals,
        symbols={name:symbols[name] for name in sizes},
        limitation='Retired logo pixel bytes are not dereferenced; original descriptors and retained origins are read.')
    (directory/'physical-readback.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
    print('PHYSICAL_STATE_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))

if cfg.get('tlb_readback'):
    requested = cfg['tlb_readback']
    if requested['vector'] != 225: raise ValueError('unexpected request vector')
    idtr = read(symbols['g_idtr'], 10)
    idt_limit, idt_base = struct.unpack('<HQ', idtr)
    if idt_base != symbols['g_idt'] or idt_limit != 4095:
        raise ValueError('active IDT storage identity')
    gate = read(idt_base + 225 * 16, 16)
    pointer = read(symbols['irq_vector_stub_table'] + (225 - 32) * 8, 8)
    fixture = []
    for address in requested['fixture_entries']:
        if address & 7 or address & ~4095 not in registry:
            raise ValueError('fixture entry is outside registered table RAM')
        fixture.append({'address': address, 'bytes_hex': ram(address, 8, True).hex()})
    if len(fixture) != 2: raise ValueError('fixture window readback coverage')
    proof = dict(schema=1, candidate_id=cfg['candidate_id'], vm_id=cfg['vm_id'],
        vector=225, root=meta['active_root'], idtr_hex=idtr.hex(), gate_hex=gate.hex(),
        stub_pointer_hex=pointer.hex(), stub_address=symbols['irq_vector_225'],
        stub_hex=read(symbols['irq_vector_225'], 10).hex(),
        common_entry=symbols['irq_external_common_entry'],
        dispatcher=symbols['irq_external_dispatch'], fixture=fixture)
    (directory/'tlb-readback.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
    print('TLB_STATE_GDB '+json.dumps(proof,sort_keys=True,separators=(',',':')))

if cfg.get('control_readback'):
    # QMP independently maps QEMU CPU indices to APIC identities. Thread
    # selection changes debugger context only; it does not execute guest code.
    original = gdb.selected_thread()
    identities = {row['cpu_index']:row for row in cfg['cpu_identity']}
    thread_info = gdb.execute('info threads', to_string=True)
    (directory/'control-threads.log').write_text(thread_info)
    print('CPU_STATE_THREADS_BEGIN\n'+thread_info+'CPU_STATE_THREADS_END')
    descriptions = {}
    for line in thread_info.splitlines():
        match = re.match(r'^\s*\*?\s*(\d+)\s+Thread \d+\.\d+ \((CPU#\d+ \[(?:running|halted) ?\])\)', line)
        if match: descriptions[int(match[1])] = match[2]
    controls = []
    for thread in sorted(inferior.threads(), key=lambda t:t.num):
        # QEMU supplies qThreadExtraInfo, not the optional GDB thread.name.
        description = descriptions.get(thread.num, '')
        match = re.fullmatch(r'CPU#(\d+) \[(?:running|halted) ?\]', description)
        if not match or int(match[1]) not in identities: raise ValueError('unmapped debugger vCPU')
        identity = identities[int(match[1])]
        thread.switch()
        values = {name:int(gdb.parse_and_eval('$'+name)) for name in ('cr0','cr3','cr4','efer')}
        try:
            values['pat'] = int(gdb.parse_and_eval('$pat'))
            pat_access = 'GDB_REGISTER'
        except (gdb.error, TypeError):
            values['pat'] = None
            pat_access = 'UNAVAILABLE_USE_LOCAL_READBACK'
        row = dict(cpu_index=int(match[1]),apic_id=identity['apic_id'],thread=thread.num,
                   thread_name=description,ptid=list(thread.ptid),registers=values,pat_access=pat_access)
        controls.append(row)
        print('CPU_STATE_GDB '+json.dumps(row,sort_keys=True,separators=(',',':')))
    if original: original.switch()
    if len(controls)!=len(identities): raise ValueError('debugger vCPU coverage')
    proof=dict(schema=1,candidate_id=cfg['candidate_id'],vm_id=cfg['vm_id'],cpus=controls)
    (directory/'control-readback.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n')
