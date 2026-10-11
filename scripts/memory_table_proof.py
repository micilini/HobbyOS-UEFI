"""Portable traversal of bounded, stopped-guest table snapshots."""
import json
import struct
from pathlib import Path
from memory_evidence import require, sha

ADDRESS = 0x000ffffffffff000
STATIC_NAMES = ('boot_pml4', 'boot_pdpt_low', 'boot_pdpt_high',
                'boot_pd0', 'boot_pd1', 'boot_pd2', 'boot_pd3')


def traverse(root, frames, width, page1g=True):
    require(32 <= width <= 52, 'topology physical width')
    mask = ((1 << width) - 1) & ADDRESS
    levels, pending = {}, [(root, 4)]
    while pending:
        address, level = pending.pop()
        if address in levels:
            require(levels[address] == level, 'topology cycle or inconsistent shared level')
            continue
        require(address and not address & 4095 and address in frames, 'topology missing or malformed child frame')
        raw = frames[address]
        require(len(raw) == 4096, 'topology missing dump bytes')
        levels[address] = level
        for value, in struct.iter_unpack('<Q', raw):
            if not value & 1: continue
            require(not value & ADDRESS & ~mask, 'topology reserved child address')
            huge = level in (2, 3) and bool(value & 128)
            require(level != 4 or not value & 128, 'topology malformed root huge link')
            require(not (level == 3 and huge) or page1g, 'topology unsupported huge leaf')
            if huge:
                size = 1 << (12 + 9 * (level - 1))
                require(not value & (size - 1) & ~0x1fff, 'topology malformed huge leaf')
            if level == 1 or huge: continue
            pending.append((value & mask, level - 1))
    return levels


def replay(directory, candidate_id, vm_id, observation):
    meta = json.loads((directory / 'table-snapshot.json').read_text())
    require(meta['schema'] == 1 and meta['candidate_id'] == candidate_id and meta['vm_id'] == vm_id,
            'topology mixed candidate or VM')
    require(meta['epoch_before'] == meta['epoch_after'] == int(observation['epoch_after']),
            'topology mixed epochs')
    require(not meta['observer_failed'] and not meta['observer_nonidentity'], 'unsafe observer registry')
    require(sha(directory / 'table-frames.bin') == meta['frames_sha256'], 'topology dump hash')
    data = (directory / 'table-frames.bin').read_bytes()
    registry = list(struct.unpack('<' + 'Q' * meta['allocations'], bytes.fromhex(meta['registry_hex'])))
    addresses = meta['frame_addresses']
    require(len(registry) == len(set(registry)) and 0 < len(registry) <= 8192, 'duplicate allocation frames')
    require(addresses == sorted(set(addresses)) and len(data) == len(addresses) * 4096, 'duplicate frames or missing dump bytes')
    frames = {a: data[i * 4096:(i + 1) * 4096] for i, a in enumerate(addresses)}
    static = meta['static_frames']
    require(set(static) == set(STATIC_NAMES) and len(set(static.values())) == 7, 'static bootstrap frame inventory')
    require(set(addresses) == set(registry) | set(static.values()), 'unlisted/missing topology frames')
    map_bytes = bytes.fromhex(meta['memory_map_hex']); stride = meta['descriptor_stride']
    require(40 <= stride <= 4096 and len(map_bytes) % stride == 0 and len(map_bytes) <= 1048576,
            'topology descriptor bounds')
    ranges = [struct.unpack_from('<IIQQQQ', map_bytes, i) for i in range(0, len(map_bytes), stride)]
    for address in registry:
        require(address and not address & 4095 and address + 4096 <= meta['identity_limit'], 'unsafe registry address')
        require(any(t == 7 and base <= address and address + 4096 <= base + pages * 4096
                    for t, _, base, _, pages, _ in ranges), 'table frame is not proven conventional RAM')
    for address in static.values():
        require(not address & 4095 and any(t in range(1, 8) and base <= address and
                address + 4096 <= base + pages * 4096 for t, _, base, _, pages, _ in ranges),
                'static table frame is not proven RAM')
    root = meta['cr3'] & ADDRESS
    require(root == meta['active_root'] == int(observation['root'], 0), 'topology active root mismatch')
    active = traverse(root, frames, meta['physical_bits'], meta['page1g'])
    bootstrap = traverse(meta['bootstrap_root'], frames, meta['physical_bits'], meta['page1g']) if meta['bootstrap_root'] else {}
    guest_bytes = bytes.fromhex(meta['guest_reachable_hex'])
    require(len(guest_bytes) == int(observation['reachable']) * 8, 'guest frame enumeration length')
    guest = [x[0] for x in struct.iter_unpack('<Q', guest_bytes)]
    require(len(guest) == len(set(guest)) and set(guest) == set(active), 'guest reachable frame set/count differs from independent traversal')
    bootstrap_only = set(bootstrap) - set(active)
    orphan = set(registry) - set(active) - set(bootstrap)
    require(not orphan and set(active) | set(bootstrap) == set(registry), 'unclassified/orphan PMM table allocations')
    require(not set(static.values()) & (set(active) | set(bootstrap)), 'static bootstrap frames classified as active PMM tables')
    require(int(observation['pmm_allocations']) == len(registry) and
            int(observation['extra_roots']) == len(bootstrap_only) and
            int(observation['reachable']) == len(active), 'independent frame count reconciliation')
    return {'active': len(active), 'bootstrap_only': len(bootstrap_only), 'static': len(static),
            'allocations': len(registry), 'orphan': len(orphan), 'active_frames': sorted(active)}
