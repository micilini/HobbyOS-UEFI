#!/usr/bin/env python3
"""Offline consistency checks for recorded memory observations (no guest execution)."""
import argparse
import base64
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import statistics
import struct
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import memory_evidence as E
import kernel_image_current as WX
import memory_table_proof as TABLE_PROOF

MASK64 = (1 << 64) - 1
ADDRESS = 0x000ffffffffff000
TYPES = {0: 'UC', 1: 'WC', 4: 'WT', 5: 'WP', 6: 'WB', 7: 'UC_MINUS'}
SPEC = importlib.util.spec_from_file_location('foundation', Path(__file__).with_name('verify-foundation-regression.py'))
FOUNDATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FOUNDATION)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(record, key):
    value = record[key]
    return int(value, 16 if value.startswith('0x') else 10)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for part in iter(lambda: f.read(1024 * 1024), b''): h.update(part)
    return h.hexdigest()


def read_json(path):
    return json.loads(path.read_text())


def recorded_address_map(row, directory, parsed_symbols):
    """Portable ELF-fact adapter. All inherited input/handler checks remain active.

    The full ELF is deliberately outside the review ZIP. This boundary validates
    recorded nm/readelf facts and their candidate binding, not the omitted ELF.
    The historical verifier itself is unchanged and still requires its binary.
    """
    try:
        name = row['input_address_map_artifact']
        require(Path(name).name == name, 'unsafe address-map path')
        path = directory / name
        require(digest(path) == row['input_address_map_sha256'], 'address-map hash mismatch')
        value = read_json(path)
        facts = read_json(directory / 'elf-facts.json')
        require(facts['elf_sha256'] == value['elf_sha256'] == row['candidate_sha256'], 'ELF fact candidate mismatch')
        require(digest(directory / 'symbols.txt') == facts['symbols_sha256'] and
                digest(directory / 'elf.txt') == facts['headers_sha256'], 'ELF tool facts changed')
        symbols = {}
        for line in (directory / 'symbols.txt').read_text().splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2] in parsed_symbols:
                require(fields[2] not in symbols, 'duplicate input symbol')
                symbols[fields[2]] = int(fields[0], 16)
        require(symbols == parsed_symbols, 'input virtual address differs from captured nm')
        def segments(text):
            result = []
            for line in text.splitlines():
                f = line.split()
                if f and f[0] == 'LOAD':
                    result.append(dict(zip(('offset', 'virtual_address', 'physical_address', 'file_size', 'memory_size'),
                                           (int(v, 0) for v in f[1:6]))))
            return result
        loads = segments(value['readelf_stdout'])
        require(loads and loads == segments((directory / 'elf.txt').read_text()) == value['load_segments'] == row['input_load_segments'],
                'LOAD segments differ from captured readelf')
        physical = {}
        for symbol, address in symbols.items():
            matching = [s for s in loads if s['virtual_address'] <= address < s['virtual_address'] + s['memory_size']]
            require(len(matching) == 1, 'input symbol has no unique LOAD mapping')
            s = matching[0]
            physical[symbol] = s['physical_address'] + address - s['virtual_address']
        fmt = lambda values: {key: f'0x{v:016x}' for key, v in values.items()}
        require(value['schema'] == 1 and value['kind'] == 'elf-input-physical-address-map' and
                value['virtual_addresses'] == fmt(symbols) and
                value['physical_addresses'] == row['input_symbol_physical_addresses'] == fmt(physical),
                'physical input addresses inconsistent')
        argv = value['readelf_argv']
        require(len(argv) == 3 and Path(argv[0]).name == 'readelf' and argv[1] == '-lW' and
                Path(argv[2]).name == 'kernel.elf', 'readelf command identity')
        return physical, []
    except (ValueError, KeyError, OSError, TypeError) as error:
        return None, ['portable ELF facts: ' + str(error)]


# Only the excluded-binary boundary is adapted; causal input validation is reused.
FOUNDATION.input_address_map_errors = recorded_address_map


def memory_cpu_command_errors(row, segment, smp, require_irq_terminal=True):
    """Add structured mem/cpu records to the inherited command vocabulary."""
    payload = row.get('payload')
    if payload not in ('mem', 'cpu'):
        return INHERITED_COMMAND_ERRORS(row, segment, smp, require_irq_terminal)
    try:
        prefix = '[MEM][SUMMARY] PASS ' if payload == 'mem' else '[CPU][SUMMARY] PASS '
        lines = [line for line in segment if line.startswith(prefix)]
        require(len(lines) == 1 and row['handler_status'] == 0, payload + ': completion or summary missing')
        fields = dict(item.split('=', 1) for item in lines[0][len(prefix):].split())
        if payload == 'cpu':
            equal_numbers(fields, dict(logical_cpus=smp, long_mode=1), 'CPU summary')
        else:
            for arena in ('pmm', 'heap'):
                total, free, used = (number(fields, arena + '_' + k) for k in ('total', 'free', 'used'))
                require(total > 0 and 0 <= free <= total and total == free + used and
                        number(fields, arena + '_integrity') == 1, arena + ' summary arithmetic/integrity')
            require(number(fields, 'heap_free_blocks') <= number(fields, 'heap_blocks') and
                    number(fields, 'heap_largest_free') <= number(fields, 'heap_free'), 'heap summary bounds')
        return []
    except (ValueError, KeyError) as error:
        return [str(error)]


INHERITED_COMMAND_ERRORS = FOUNDATION.evidence_errors
FOUNDATION.evidence_errors = memory_cpu_command_errors


def records(lines, group, kind):
    prefix = f'[{group}][{kind}] '
    result = []
    for line in lines:
        if not line.startswith(prefix): continue
        fields = {}
        for item in line[len(prefix):].split():
            require('=' in item, f'{prefix} malformed field')
            k, v = item.split('=', 1)
            require(k not in fields and v, f'{prefix} duplicate/empty field')
            fields[k] = v
        require(fields.get('schema') == '1', f'{prefix} unsupported schema')
        result.append(fields)
    return result


def one(lines, group, kind):
    values = records(lines, group, kind)
    require(len(values) == 1, f'{group}/{kind}: expected one record, got {len(values)}')
    return values[0]


def equal_numbers(record, expected, context):
    for key, value in expected.items():
        require(number(record, key) == value, f'{context}: {key} differs from raw inputs')


def verify_cpu(lines, topology):
    roles = {slot: value[1] for slot, value in topology.items()}
    topology = {slot: value[0] for slot, value in topology.items()}
    summary = one(lines, 'CPU_CONTROL', 'SUMMARY')
    targets = records(lines, 'CPU_CONTROL', 'TARGET')
    cpus = records(lines, 'CPU_CONTROL', 'CPU')
    generation = number(summary, 'generation')
    require(generation > 0, 'CPU generation must be positive')
    for group in (targets, cpus):
        require(len(group) == len(topology), 'missing or duplicate CPU record')
        require({number(c, 'slot'): number(c, 'apic') for c in group} == topology,
                'CPU target identity differs from online boot topology')
        require(len({c['slot'] for c in group}) == len(group), 'duplicate CPU slot')
        require(all(number(c, 'generation') == generation for c in group), 'stale CPU generation')
    mask = sum(1 << slot for slot in topology)
    equal_numbers(summary, dict(target=mask, acknowledged=mask, errors=0, missing=0), 'CPU masks')
    require(summary['collection_complete'] == 'true' and summary['reason'] == 'collected',
            'CPU collection is incomplete')
    require(summary['apic_mode'] in ('xapic', 'x2apic'), 'invalid APIC mode')
    if summary['apic_mode'] == 'xapic': require(max(topology.values()) <= 255, 'xAPIC destination width')
    for c in cpus:
        require(c['role'] == roles[number(c, 'slot')], 'CPU role differs from independent boot topology')
        equal_numbers(c, dict(identity_valid=1, cr_valid=1), 'CPU validity')
        cpuid, ext = number(c, 'cpuid1_edx'), number(c, 'extended_edx')
        pat_supported = int(bool(cpuid & (1 << 16)))
        equal_numbers(c, dict(pat_supported=pat_supported), 'PAT capability')
        require(not number(c, 'pat_valid') or (pat_supported and cpuid & (1 << 5)),
                'unsupported PAT presented as valid')
        require(not number(c, 'efer_valid') or (ext & (1 << 29) and cpuid & (1 << 5)),
                'unsupported EFER presented as valid')
        require(number(c, 'efer_valid') == 1 and (not pat_supported or number(c, 'pat_valid')),
                'required CPU safe read failed')
        expected_width = number(c, 'address_eax') & 255 if number(c, 'address_eax') else 36
        equal_numbers(c, {'physical_bits': expected_width}, 'CPUID physical width')
        require(32 <= expected_width <= 52, 'unsupported physical width')
    require(sum(c['role'] == 'BSP' for c in cpus) == 1 and
            all(c['role'] in ('BSP', 'AP') for c in cpus), 'CPU role cardinality')
    uniform = {}
    for field, label in [('cr0', 'cr0'), ('cr3', 'cr3_raw'), ('cr4', 'cr4'), ('efer', 'efer'), ('pat', 'pat')]:
        valid = field not in ('efer', 'pat') or all(number(c, field + '_valid') for c in cpus)
        uniform[label] = str(int(len({number(c, field) for c in cpus}) == 1)) if valid else 'NA'
    uniform['cr3_root'] = str(int(len({number(c, 'cr3') & ADDRESS for c in cpus}) == 1))
    for key, value in uniform.items():
        require(summary['uniform_' + key] == value, f'falsely uniform CPU {key} summary')
    require(summary['controls_uniform'] == ('true' if all(v == '1' for v in uniform.values()) else 'false'),
            'overall CPU uniformity differs')
    return generation, summary, cpus


def verify_page(control, entries, result):
    va, cr0, cr3, cr4, efer = (number(control, k) for k in ('va', 'cr0', 'cr3', 'cr4', 'efer'))
    width = number(control, 'physical_bits')
    require(32 <= width <= 52, 'page physical width')
    canonical_bits = 57 if cr4 & (1 << 12) else 48
    canonical = va >> canonical_bits == ((1 << (64 - canonical_bits)) - 1 if va & (1 << (canonical_bits - 1)) else 0)
    if not canonical:
        require(result['status'] == 'NONCANONICAL' and not entries, 'noncanonical result')
        return
    if cr4 & (1 << 12):
        require(result['status'] == 'UNSUPPORTED_MODE' and not entries, 'LA57 must be unsupported')
        return
    require(number(control, 'efer_valid') and cr0 & (1 << 31) and cr4 & 32 and efer & 1024,
            'page active controls unavailable')
    physical_mask = ((1 << width) - 1) & ADDRESS
    root = cr3 & physical_mask
    require(cr3 & ~(physical_mask | 4095) == 0, 'CR3 reserved address bits')
    equal_numbers(result, dict(va=va, root=root, entries=len(entries)), 'page result')
    require(result['effective_type'] == 'NOT_COMPUTED', 'unqualified effective memory type')
    require(1 <= len(entries) <= 4, 'page entry count')
    table, rw, us, nx = root, True, True, False
    terminated = False
    for position, entry in enumerate(entries):
        level = 4 - position
        raw = number(entry, 'raw')
        present = bool(raw & 1)
        huge = level in (2, 3) and bool(raw & 128)
        leaf = level == 1 or huge
        equal_numbers(entry, dict(level=level, table=table, index=(va >> (12 + 9 * (level - 1))) & 511), 'walk link')
        decoded = dict.fromkeys(('P', 'RW', 'US', 'PWT', 'PCD', 'A', 'D', 'PS', 'PAT', 'G', 'NX'), 'NA')
        decoded['P'] = str(int(present))
        if present:
            for field, bit in [('RW', 1), ('US', 2), ('PWT', 3), ('PCD', 4), ('A', 5), ('NX', 63)]:
                decoded[field] = str((raw >> bit) & 1)
            if level in (2, 3): decoded['PS'] = str(int(huge))
            if leaf:
                for field, bit in [('D', 6), ('PAT', 12 if huge else 7), ('G', 8)]: decoded[field] = str((raw >> bit) & 1)
        require(all(entry[k] == v for k, v in decoded.items()), 'entry field decode differs from raw bits')
        equal_numbers(result if position == len(entries) - 1 else {'stopped': str(level)}, {'stopped': level}, 'stop level')
        if not present:
            require(position == len(entries) - 1 and result['status'] == 'NOT_PRESENT' and
                    number(result, 'leaf') == 0 and number(result, 'page_size') == 0,
                    'absent mapping falsely translated (including zero PA)')
            terminated = True
            break
        require(not (raw & ADDRESS & ~physical_mask), 'reserved physical address bits')
        require(not (raw & (1 << 63)) or (number(control, 'nx_supported') and efer & 2048), 'reserved NX state')
        require(level != 4 or not raw & 128, 'PML4 PS reserved')
        require(not (level == 3 and huge) or number(control, 'page1g_supported'), 'unsupported 1-GiB page')
        rw, us, nx = rw and bool(raw & 2), us and bool(raw & 4), nx or bool(raw & (1 << 63))
        if not leaf:
            table = raw & physical_mask
            continue
        size = 1 << (12 + 9 * (level - 1))
        require(not huge or not (raw & (size - 1) & ~0x1fff), 'misaligned huge leaf')
        base = raw & physical_mask & ~(size - 1)
        offset = va & (size - 1)
        index = (((raw >> (12 if huge else 7)) & 1) << 2) | ((raw >> 3) & 3)
        require(not (index & 4) or number(control, 'pat_supported'), 'reserved PAT selector')
        require(position == len(entries) - 1 and result['status'] == 'MAPPED', 'huge leaf was followed or mapping omitted')
        equal_numbers(result, dict(leaf=level, page_size=size, base=base, offset=offset,
            physical=base + offset, effective_rw=int(rw), effective_us=int(us), effective_nx=int(nx),
            supervisor_write_protected=int(not rw and bool(cr0 & (1 << 16))), pat_index=index), 'page translation')
        if number(control, 'pat_valid'):
            require(number(control, 'pat_supported'), 'unsupported PAT read claimed valid')
            type_value = (number(control, 'pat') >> (index * 8)) & 255
            equal_numbers(result, dict(pat_type_raw=type_value, pat_type_valid=int(type_value in TYPES)), 'PAT slot')
            require(result['pat_type'] == TYPES.get(type_value, 'RESERVED'), 'PAT slot type name')
        else:
            require(number(result, 'pat_type_valid') == 0 and result['pat_type'] == 'UNAVAILABLE', 'missing PAT safe read hidden')
        terminated = True
        break
    require(terminated, 'walk incomplete or unsafe access')


def verify_pages(lines, symbols=None):
    controls = records(lines, 'PAGE_TABLE', 'CONTROL')
    results = records(lines, 'PAGE_TABLE', 'RESULT')
    entries = records(lines, 'PAGE_TABLE', 'ENTRY')
    expected = {'kernel', 'ram', 'mmio', 'framebuffer', 'unmapped'}
    require(len(controls) == len(results) == 5 and {r['sample'] for r in results} == expected,
            'required page representatives missing or duplicated')
    ids = {r['id'] for r in controls + results + entries}
    require(len(ids) == 1, 'mixed page capture identities')
    for control in controls:
        sample = control['sample']
        matching = [r for r in results if r['sample'] == sample]
        require(len(matching) == 1, 'duplicate page result')
        result = matching[0]
        visit = [r for r in entries if r['sample'] == sample]
        verify_page(control, visit, result)
        require(result['status'] == ('NOT_PRESENT' if sample == 'unmapped' else 'MAPPED'), 'representative mapping status')
        if sample == 'mmio': require(visit[-1]['PCD'] == '1', 'missing existing PCD MMIO representative')
        if sample == 'ram':
            import low_current
            import direct_map_evidence
            require(number(result,'physical')==number(result,'root') and
                number(result,'va')==number(result,'root')+(direct_map_evidence.BASE if low_current.enabled() else 0),'RAM provenance')
        if sample == 'kernel':
            require(symbols and number(control, 'va') == symbols['_kernel_start'], 'kernel sample differs from ELF symbol')
            require(number(result, 'physical') == symbols['_kernel_start'] - 0xffffffff80000000, 'kernel image offset oracle')
    tables = one(lines, 'PAGE_TABLE', 'TABLES')
    require(tables['id'] in ids and tables['point'] == 'post_smp_command', 'table count observation point')
    equal_numbers(tables, dict(complete=1, consistent=1, static_frames=0), 'table traversal completeness')
    require(number(tables, 'epoch_before') == number(tables, 'epoch_after'), 'table mutation during traversal')
    require(0 < number(tables, 'reachable') <= number(tables, 'pmm_allocations') and
            number(tables, 'reachable') + number(tables, 'extra_roots') == number(tables, 'pmm_allocations') and
            0 < number(tables, 'initial_pmm_allocations') <= number(tables, 'pmm_allocations'), 'inconsistent table frame counts')
    return tables


def verify_memmap(lines):
    header = one(lines, 'MEMORY_MAP', 'HEADER')
    result = one(lines, 'MEMORY_MAP', 'RESULT')
    base, size, stride, count = (number(header, k) for k in ('base', 'bytes', 'stride', 'count'))
    require(base and 0 < size <= 1048576 and 40 <= stride <= 4096 and size % stride == 0 and
            count == size // stride and 0 < count <= 4096 and base + size <= MASK64,
            'invalid descriptor array shape/stride/count')
    require(header['valid'] == '1' and result['complete'] == '1' and
            header['descriptor_version'] == 'NOT_IN_HANDOFF', 'incomplete handoff')
    descriptors = records(lines, 'MEMORY_MAP', 'DESCRIPTOR')
    require([number(d, 'index') for d in descriptors] == list(range(count)), 'descriptor count or original order')
    for d in descriptors:
        types = ('Reserved', 'LoaderCode', 'LoaderData', 'BootServicesCode', 'BootServicesData',
            'RuntimeServicesCode', 'RuntimeServicesData', 'ConventionalMemory', 'UnusableMemory',
            'ACPIReclaimMemory', 'ACPIMemoryNVS', 'MemoryMappedIO', 'MemoryMappedIOPortSpace',
            'PalCode', 'PersistentMemory', 'UnacceptedMemory')
        dtype = number(d, 'type')
        require(d['name'] == (types[dtype] if dtype < len(types) else 'Unknown'), 'descriptor type label')
        start, pages = number(d, 'physical'), number(d, 'pages')
        require(start % 4096 == 0 and pages > 0 and start + pages * 4096 <= MASK64, 'descriptor overflow or alignment')
        equal_numbers(d, dict(end=start + pages * 4096, range_valid=1), 'descriptor interval')
    extensions = records(lines, 'MEMORY_MAP', 'EXTENSION')
    expected = [(i, offset, min(64, stride - offset)) for i in range(count) for offset in range(40, stride, 64)]
    require([(number(r, 'index'), number(r, 'offset'), number(r, 'bytes')) for r in extensions] == expected,
            'descriptor extensions omitted or reordered')
    require(all(len(bytes.fromhex(r['hex'])) == number(r, 'bytes') for r in extensions), 'descriptor extension truncated')
    mc = one(lines, 'MTRR', 'CONTROL')
    supported = bool(number(mc, 'cpuid1_edx') & (1 << 12) and number(mc, 'cpuid1_edx') & (1 << 5))
    equal_numbers(mc, {'supported': int(supported)}, 'MTRR support')
    require(supported or not (number(mc, 'capability_valid') or number(mc, 'defaults_valid')), 'unsupported MTRR valid read')
    cap, defaults, width = number(mc, 'capability'), number(mc, 'defaults'), number(mc, 'physical_bits')
    require(32 <= width <= 52, 'MTRR physical width')
    equal_numbers(mc, dict(count=(cap & 255) if number(mc, 'capability_valid') else 0,
        enabled=(defaults >> 11) & 1, fixed_enabled=(defaults >> 10) & 1, default_type=defaults & 255,
        fixed_supported=(cap >> 8) & 1, wc_supported=(cap >> 10) & 1), 'MTRR control bits')
    require(mc['fixed_coverage'] == 'NOT_COLLECTED', 'fixed range coverage falsely claimed')
    ranges = records(lines, 'MTRR', 'RANGE')
    require([number(r, 'index') for r in ranges] == list(range(number(mc, 'count'))), 'variable MTRR count')
    for r in ranges:
        rb, rm = number(r, 'base_raw'), number(r, 'mask_raw')
        valid_reads = supported and number(r, 'base_valid') and number(r, 'mask_valid')
        enabled = bool(rm & 2048)
        mask = ((1 << width) - 1) & ADDRESS
        length = ((~rm) & mask) + 4096
        start = rb & mask
        reserved_valid = not (rb & ~(mask | 255)) and not (rm & ~(mask | 2048)) and (rb & 255) in (0, 1, 4, 5, 6)
        well_formed = (reserved_valid and length & (length - 1) == 0 and
                       start % length == 0 and start + length <= (1 << width))
        valid = bool(valid_reads and reserved_valid and (not enabled or well_formed))
        equal_numbers(r, dict(enabled=int(enabled), valid=int(valid)), 'MTRR validity')
        if valid and enabled:
            equal_numbers(r, dict(base=start, size=length, end=start + length, type=rb & 255), 'MTRR interval')
    known = records(lines, 'MEMORY_MAP', 'RANGE')
    require(len({r['name'] for r in known}) == len(known), 'duplicate known range')
    names = {r['name'] for r in known}
    require({'framebuffer', 'lapic', 'APIC', 'FACP', 'DSDT', 'RSDP', 'XSDT', 'RSDT', 'HPET', 'MCFG'} <= names,
            'known firmware/device range missing')
    intersections = records(lines, 'MEMORY_MAP', 'INTERSECTION')
    correlations = records(lines, 'MEMORY_MAP', 'CORRELATION')
    mi = records(lines, 'MTRR', 'INTERSECTION')
    expected_di, expected_mi = [], []
    for r in known:
        start, length, end = (number(r, k) for k in ('base', 'bytes', 'end'))
        valid = bool(length and start + length <= MASK64)
        equal_numbers(r, dict(valid=int(valid), end=start + length if valid else 0), 'known range arithmetic')
        require(r.get('origin'), 'range provenance absent')
        n = 0
        if valid:
            for i, d in enumerate(descriptors):
                low, high = max(start, number(d, 'physical')), min(end, number(d, 'end'))
                if low < high:
                    expected_di.append((r['name'], i, low, high)); n += 1
            for i, m in enumerate(ranges):
                if m['enabled'] != '1' or m['valid'] != '1': continue
                low, high = max(start, number(m, 'base')), min(end, number(m, 'end'))
                if low < high: expected_mi.append((r['name'], i, low, high))
        summaries = [c for c in correlations if c['name'] == r['name']]
        require(len(summaries) == 1 and number(summaries[0], 'intersections') == n, 'map correlation summary')
    require([(r['name'], number(r, 'descriptor'), number(r, 'start'), number(r, 'end')) for r in intersections] == expected_di,
            'descriptor intersections differ from raw ranges')
    require([(r['name'], number(r, 'range'), number(r, 'start'), number(r, 'end')) for r in mi] == expected_mi,
            'fabricated or missing variable MTRR overlap')
    for group, kinds in [('MEMORY_MAP', ('HEADER', 'DESCRIPTOR', 'EXTENSION', 'RANGE', 'INTERSECTION', 'CORRELATION', 'RESULT')),
                         ('MTRR', ('CONTROL', 'RANGE', 'INTERSECTION'))]:
        for kind in kinds:
            for r in records(lines, group, kind):
                require(r['id'] == header['id'], 'mixed map capture identities')
                if 'effective_type' in r: require(r['effective_type'] == 'NOT_COMPUTED', 'unqualified effective cache type')
    return header, mc


def timing(directory, raw):
    events = [json.loads(line) for line in (directory / 'serial-observations.jsonl').read_text().splitlines()]
    offset, previous = 0, -1
    for e in events:
        data = base64.b64decode(e['bytes_base64'], validate=True)
        require(e['offset'] == offset and e['length'] == len(data) and data and
                e['monotonic_ns'] >= previous, 'nonmonotonic/truncated serial observations')
        require(raw[offset:offset + len(data)] == data, 'stream observation differs from raw serial')
        offset += len(data); previous = e['monotonic_ns']
    require(offset == len(raw), 'stream observation incomplete')
    samples = []
    for marker in (b'[CORE] Init Paging...', b'[CORE] Paging OK! Switched CR3.'):
        require(raw.count(marker) == 1, 'timing marker missing or duplicated')
        start = raw.index(marker); end = start + len(marker) - 1
        index = next(i for i, e in enumerate(events) if e['offset'] <= end < e['offset'] + e['length'])
        samples.append({'marker': marker.decode(), 'start_offset': start, 'last_byte_offset': end,
                        'event': index, 'monotonic_ns': events[index]['monotonic_ns']})
    require(samples[0]['start_offset'] < samples[1]['start_offset'] and samples[0]['event'] < samples[1]['event'] and
            samples[0]['monotonic_ns'] < samples[1]['monotonic_ns'], 'unresolved or reversed paging duration')
    return {'duration_ns': samples[1]['monotonic_ns'] - samples[0]['monotonic_ns'], 'markers': samples}


def verify_representatives(lines):
    controls = {r['sample']: r for r in records(lines, 'PAGE_TABLE', 'CONTROL')}
    known = {r['name']: r for r in records(lines, 'MEMORY_MAP', 'RANGE')}
    framebuffer = [re.search(r' FB=(0x[0-9A-Fa-f]+) Size=(0x[0-9A-Fa-f]+)$', l)
                   for l in lines if l.startswith('[UEFI][GOP] FINAL ')]
    require(len(framebuffer) == 1 and framebuffer[0], 'independent framebuffer handoff missing')
    base, size = (int(x, 16) for x in framebuffer[0].groups())
    managed=controls['framebuffer']['origin']=='owned_BootInfo_framebuffer'
    page_results={r['sample']:r for r in records(lines,'PAGE_TABLE','RESULT')}
    if managed:
        require(0xffffd00000000000<=number(controls['framebuffer'],'va')<0xffffd00040000000 and
                number(page_results['framebuffer'],'physical')==base,'owned framebuffer physical origin differs')
    else:
        require(controls['framebuffer']['origin'] == 'BootInfo_framebuffer' and
                number(controls['framebuffer'], 'va') == base, 'framebuffer representative differs from handoff')
    equal_numbers(known['framebuffer'], {'base': base, 'bytes': size}, 'BootInfo framebuffer range')
    require(known['framebuffer']['origin'] == 'BootInfo.BufferSize', 'framebuffer range origin')
    controllers = {}
    for line in lines:
        m = re.match(r'\[IRQ\]\[IOAPIC\] index=(\d+) id=\d+ base=([0-9A-Fa-f]+) ', line)
        if m:
            require(int(m[1]) not in controllers, 'duplicate independent controller')
            controllers[int(m[1])] = int(m[2], 16)
    mmio = controls['mmio']
    origins=('owned_madt_ioapic_base','owned_madt_lapic_base') if managed else ('madt_ioapic_base','madt_lapic_base')
    require(mmio['origin'] in origins,'MMIO representative origin')
    address=number(page_results['mmio'],'physical') if managed else number(mmio,'va')
    if managed:require(0xffffd00000000000<=number(mmio,'va')<0xffffd00040000000,'MMIO outside owned window')
    if mmio['origin']==origins[0]:
        require(0 in controllers and address==controllers[0],'MMIO representative differs from discovered controller')
    else:require(address==number(known['lapic'],'base'),'MMIO LAPIC range')
    require(controls['kernel']['origin'] == 'linker_kernel_start' and
            controls['ram']['origin'] == ('active_root_ram_view' if __import__('low_current').enabled() else 'active_root_pmm_frame') and
            controls['unmapped']['origin'] == 'bounded_walker_search', 'representative origin mismatch')
    for index, address in controllers.items():
        r = known['ioapic' + str(index)]
        equal_numbers(r, {'base': address, 'bytes': 4096}, 'discovered IOAPIC range')
        require(r['origin'] == 'MADT.page_extent', 'IOAPIC origin')
    require(known['lapic']['origin'] == 'MADT.page_extent', 'LAPIC origin')
    for line in lines:
        m = re.fullmatch(r'  \[\d+\] (\w{4}) addr=(0x[0-9A-Fa-f]+) len=(\d+) rev=\d+', line)
        if m and m[1] in known:
            equal_numbers(known[m[1]], {'base': int(m[2], 16), 'bytes': int(m[3])}, 'retained ACPI header')
    for name, r in known.items():
        if name in ('APIC', 'FACP', 'HPET', 'MCFG', 'WAET'):
            prefix = 'acpi_retained_table' if managed else 'acpi_find_table'
            require(r['origin'] == (prefix+'.header_length' if number(r, 'bytes') else
                    prefix+'.unavailable'), 'ACPI table range origin')
        elif name in ('RSDP', 'XSDT', 'RSDT'):
            require(r['origin'] == 'acpi_observe_root.retained_metadata', 'ACPI root range origin')
        elif name == 'DSDT': require(r['origin'] == ('acpi_retained_table.header_length' if managed else
                    'acpi_get_dsdt.header_length'), 'DSDT range origin')
        elif name == 'hpet': require(r['origin'] == 'HPET.GAS.page_extent', 'HPET range origin')
        elif name.startswith('ecam'): require(r['origin'] == 'MCFG.bus_span', 'MCFG range origin')


def verify_firmware_snapshot(directory, lines):
    meta = read_json(directory / 'table-snapshot.json')
    known = {r['name']: r for r in records(lines, 'MEMORY_MAP', 'RANGE')}
    boot = bytes.fromhex(meta['boot_metadata_hex'])
    base, size, stride, count, framebuffer, framebuffer_size, rsdp = struct.unpack_from('<7Q', boot)
    equal_numbers(one(lines, 'MEMORY_MAP', 'HEADER'),
                  dict(base=base, bytes=size, stride=stride, count=count), 'raw handoff metadata')
    equal_numbers(known['framebuffer'], dict(base=framebuffer, bytes=framebuffer_size), 'raw framebuffer metadata')
    equal_numbers(known['RSDP'], dict(base=rsdp), 'raw RSDP metadata')
    raw_map = bytes.fromhex(meta['memory_map_hex'])
    require(len(raw_map) == size and size == stride * count, 'raw descriptor dump length')
    for d in records(lines, 'MEMORY_MAP', 'DESCRIPTOR'):
        t, _, physical, virtual, pages, attributes = struct.unpack_from('<IIQQQQ', raw_map, number(d, 'index') * stride)
        equal_numbers(d, dict(type=t, physical=physical, virtual=virtual, pages=pages, attributes=attributes), 'raw UEFI descriptor')
    raw = {}
    for r in meta['acpi']:
        require(r['name'] not in raw, 'duplicate ACPI dump')
        data = bytes.fromhex(r['hex']); raw[r['name']] = data
        equal_numbers(known[r['name']], dict(base=r['base'], bytes=len(data)), 'ACPI raw table range')
        if r['name'] == 'RSDP':
            require(data[:8] == b'RSD PTR ' and sum(data[:20]) % 256 == 0 and
                    (len(data) == 20 or sum(data) % 256 == 0), 'RSDP signature/checksum')
        else:
            require(data[:4] == r['name'].encode() and struct.unpack_from('<I', data, 4)[0] == len(data)
                    and sum(data) % 256 == 0, 'ACPI table header/checksum')
    require({n for n in ('APIC', 'FACP', 'HPET', 'MCFG', 'WAET', 'DSDT', 'RSDP', 'XSDT', 'RSDT')
             if number(known[n], 'bytes')} == set(raw), 'missing ACPI raw provenance')
    rsdp_bytes = raw['RSDP']
    xsdt = struct.unpack_from('<Q', rsdp_bytes, 24)[0] if len(rsdp_bytes) > 20 else 0
    # init_acpi retains XSDT preferentially; RSDT is a fallback, not a second root.
    equal_numbers(known['RSDT'], {'base': 0 if xsdt else struct.unpack_from('<I', rsdp_bytes, 16)[0]}, 'retained RSDT pointer')
    equal_numbers(known['XSDT'], {'base': xsdt}, 'retained XSDT pointer')
    root_name, width = ('XSDT', 8) if 'XSDT' in raw else ('RSDT', 4)
    pointers = [x[0] for x in struct.iter_unpack('<Q' if width == 8 else '<I', raw[root_name][36:])]
    for n in ('APIC', 'FACP', 'HPET', 'MCFG', 'WAET'):
        if n in raw: require(number(known[n], 'base') in pointers, 'ACPI table not referenced by root')
    fadt = raw['FACP']; dsdt = struct.unpack_from('<Q', fadt, 140)[0] if len(fadt) >= 148 else 0
    if not dsdt: dsdt = struct.unpack_from('<I', fadt, 40)[0]
    equal_numbers(known['DSDT'], {'base': dsdt}, 'FADT DSDT pointer')
    madt = raw['APIC']; lapic = struct.unpack_from('<I', madt, 36)[0]; at = 44; io = 0
    while at < len(madt):
        kind, length = madt[at:at + 2]
        require(length >= 2 and at + length <= len(madt), 'MADT subtable bounds')
        if kind == 1:
            equal_numbers(known['ioapic' + str(io)], {'base': struct.unpack_from('<I', madt, at + 4)[0], 'bytes': 4096}, 'MADT IOAPIC')
            io += 1
        if kind == 5: lapic = struct.unpack_from('<Q', madt, at + 4)[0]
        at += length
    equal_numbers(known['lapic'], {'base': lapic, 'bytes': 4096}, 'MADT LAPIC')
    if 'HPET' in raw and raw['HPET'][40] == 0:
        equal_numbers(known['hpet'], {'base': struct.unpack_from('<Q', raw['HPET'], 44)[0], 'bytes': 4096}, 'HPET GAS')
    if 'MCFG' in raw:
        for i, (address, _, first, last, _) in enumerate(struct.iter_unpack('<QHBBI', raw['MCFG'][44:])):
            equal_numbers(known['ecam' + str(i)], {'base': address, 'bytes': (last - first + 1) << 20}, 'MCFG bus range')


def verify_readback(directory, lines, launch, candidate_id, observation):
    targets = read_json(directory / 'page-readback-targets.json')
    expected = [{'address': number(r, 'table') + number(r, 'index') * 8,
                 'sample': r['sample'], 'level': number(r, 'level'), 'observed_raw': number(r, 'raw')}
                for r in records(lines, 'PAGE_TABLE', 'ENTRY') if r['sample'] in ('kernel', 'mmio', 'framebuffer')]
    require(targets == expected and targets, 'readback target count/order/sample mismatch')
    command = (directory / 'page-readback.gdb').read_text().splitlines()
    require([l for l in command if l.startswith('x/')] == [f'x/gx 0x{r["address"]:x}' for r in targets], 'readback command targets')
    require(not any(re.search(r'^(call|set \$|set \{|restore|jump|continue|step)', l) for l in command), 'readback must be read-only')
    output = (directory / 'page-readback.stdout.log').read_text()
    (directory / 'page-readback.stderr.log').read_bytes()
    values = re.findall(r'^0x([0-9a-f]+)(?:\s+<[^>]+>)?:\s+0x([0-9a-f]+)', output, re.M)
    require(len(values) == len(targets), 'readback missing/truncated/duplicate values')
    for target, (address, value) in zip(targets, values):
        raw, level = target['observed_raw'], target['level']
        leaf = level == 1 or level in (2, 3) and raw & 128
        allowed = (0x20 | (0x40 if leaf else 0)) if raw & 1 else 0
        require(int(address, 16) == target['address'] and not (int(value, 16) ^ raw) & ~allowed,
                'readback address/order/value mismatch (only applicable hardware A/D may differ)')
    commands = [json.loads(l) for l in (directory / 'commands.jsonl').read_text().splitlines()]
    selected = [r for r in commands if r['name'] == 'page-readback']
    require(len(selected) == 1 and selected[0]['exit_status'] == 0 and
            Path(selected[0]['argv'][0]).name == 'gdb', 'readback command failed or missing')
    cfg = read_json(directory / 'capture-config.json')
    require(cfg['candidate_id'] == candidate_id and cfg['vm_id'] == launch['vm_id'], 'readback candidate/VM binding')
    argv = launch['argv']
    remote = argv[argv.index('-gdb') + 1].split(',')[0]
    require(remote.startswith('chardev:'), 'readback requires owned local GDB chardev')
    chardevs = [dict(token.split('=', 1) for token in argv[i + 1].split(',')[1:])
                for i, arg in enumerate(argv[:-1]) if arg == '-chardev' and argv[i + 1].startswith('socket,')]
    matching = [r for r in chardevs if r.get('id') == remote[8:]]
    require(len(matching) == 1 and matching[0].get('server') == 'on' and matching[0].get('wait') == 'off',
            'readback GDB socket binding')
    remote = matching[0]['path']
    capture_path = str(Path(cfg['output']) / 'capture.py')
    config_path = str(Path(cfg['output']) / 'capture-config.json')
    expected_command = ['set pagination off', 'set confirm off', 'target remote ' + remote]
    if cfg.get('direct_map'): expected_command += ['maintenance packet Qqemu.PhyMemMode:1']
    expected_command += [f'x/gx 0x{r["address"]:x}' for r in targets]
    if cfg.get('direct_map'): expected_command += ['maintenance packet Qqemu.PhyMemMode:0']
    expected_command += [f'python exec(compile(open({json.dumps(capture_path)}).read(), "memory-capture", "exec"), dict(MEMORY_CAPTURE_CONFIG={json.dumps(config_path)}))']
    if cfg.get('ram_layout'):
        import direct_map_evidence as RAM
        require(RAM.enabled() and cfg.get('direct_map'),'RAM owner capture outside its successor')
        ram_path=str(Path(cfg['output'])/'ram-capture.py')
        expected_command += [f'python exec(compile(open({json.dumps(ram_path)}).read(), "ram-owner-capture", "exec"), dict(MEMORY_CAPTURE_CONFIG={json.dumps(config_path)}))']
    if __import__('stack_current').enabled():
        require(cfg.get('stack_layout'),'stack readback owner layout missing')
        stack_path=str(Path(cfg['output'])/'stack-capture.py')
        expected_command += [f'python exec(compile(open({json.dumps(stack_path)}).read(), "stack-owner-capture", "exec"), dict(STACK_CAPTURE_CONFIG={json.dumps(config_path)}))']
    expected_command += ['detach','quit']
    require(command == expected_command, 'readback command/VM socket differs')
    require(selected[0]['argv'] == ['gdb', '-q', '-batch', '-x', str(Path(cfg['output']) / 'page-readback.gdb'), launch['elf']['path']],
            'readback invocation differs from bound candidate')
    symbols = {}
    for line in (directory / 'symbols.txt').read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in cfg['symbols']:
            if cfg.get('physical_readback') and fields[2]=='g_font':continue
            require(fields[2] not in symbols, 'ambiguous readback symbol')
            symbols[fields[2]] = int(fields[0], 16)
    if cfg.get('physical_readback'):
        proof_commands=[r for r in commands if r['name']=='physical-symbols']
        require(len(proof_commands)==1 and proof_commands[0]['exit_status']==0 and
                proof_commands[0]['argv']==['readelf','-sW',launch['elf']['path']],
                'physical ELF local-symbol command binding')
        (directory/'physical-symbols.stderr.log').read_bytes()
        owner=None;matches=[]
        for line in (directory/'physical-symbols.stdout.log').read_text().splitlines():
            fields=line.split()
            if len(fields)!=8 or not fields[0].endswith(':'):continue
            if fields[3]=='FILE':owner=fields[7]
            if owner=='boot_address.c' and fields[3]=='OBJECT' and fields[7]=='g_font':
                require(fields[2]=='16','physical adapted font ELF extent');matches.append(int(fields[1],16))
        require(len(matches)==1,'physical adapted font ELF identity');symbols['g_font']=matches[0]
    require(symbols == cfg['symbols'] and tuple(cfg['static_names']) == TABLE_PROOF.STATIC_NAMES,
            'readback symbol facts differ from candidate')
    control = records(lines, 'PAGE_TABLE', 'CONTROL')[0]
    require(cfg['reachable'] == number(observation, 'reachable') and cfg['physical_bits'] == number(control, 'physical_bits') and
            cfg['page1g'] == bool(number(control, 'page1g_supported')), 'readback geometry differs from inspector')
    require(digest(directory / 'capture.py') == digest(E.ROOT / 'scripts/memory-gdb-capture.py'), 'readback capture source differs')
    proof = TABLE_PROOF.replay(directory, candidate_id, launch['vm_id'], observation)
    require(len(re.findall(r'^MEMORY_CAPTURE_COMPLETE frames=\d+ cr3=0x[0-9a-f]+ epoch=\d+$', output, re.M)) == 1,
            'readback topology command incomplete')
    meta = read_json(directory / 'table-snapshot.json')
    require(f'MEMORY_CAPTURE_COMPLETE frames={len(meta["frame_addresses"])} cr3=0x{meta["cr3"]:x} epoch={meta["epoch_before"]}' in output.splitlines(),
            'readback topology completion contradicts raw snapshot')
    verify_firmware_snapshot(directory, lines)
    return proof


def production_absence_transaction(row, directory, transcript, byte_ends, profile, candidate):
    """Unknown production commands have no serial handler record; GDB observes 127.

    Require the actual dispatcher payload and return stop, and an empty serial
    interval. This path cannot accept a present command or replace SELFTEST END.
    """
    payload = row['payload']
    import low_current
    absent = ('ptinspect', 'cpustate', 'memmap', 'tlbtest', 'phystest', 'mappingtest', 'rammap', 'rammap high')
    if low_current.enabled(): absent += ('lowmap', 'lowmap crash')
    if WX.enabled(): absent += ('wxtest',)
    if __import__('stack_current').enabled(): absent += ('stacktest','stacktest overflow','stacktest timeout')
    require(payload in absent and row['handler_status'] == 127,
            'production command is present or did not return unknown')
    require(row['schema'] == 3 and row['kind'] == 'observed-command' and
            row['classification'] == 'PASS' and row['observer_status'] == 'COMPLETED', 'production observer failed')
    require(row['candidate_sha256'] == candidate and row['profile_id'] == profile['name'] and
            row['vm_id'] == profile['vm_id'], 'production observer identity')
    require(row['payload_crc32'] == f'{zlib.crc32(payload.encode()) & 0xffffffff:08x}', 'production payload checksum')
    start, end = row['start_line_count'], row['end_line_count']
    require(0 <= start == end <= len(transcript), 'unexpected serial output for unavailable command')
    offset = byte_ends[start - 1] if start else 0
    require(row['start_byte_count'] == row['end_byte_count'] == offset, 'production absence byte boundary')
    for name, hash_name in [('gdb_log', 'gdb_log_sha256'), ('gdb_command', 'gdb_command_sha256')]:
        require(Path(row[name]).name == row[name] and digest(directory / row[name]) == row[hash_name], 'production GDB artifact hash/path')
    log = (directory / row['gdb_log']).read_text().splitlines()
    for field in ('breakpoint_stop', 'payload_memory_response', 'handler_status_response', 'return_stop',
                  'return_pc_response', 'return_address_response', 'entry_breakpoint_response', 'return_breakpoint_response',
                  'entry_breakpoint_delete_response'):
        require(log.count(row[field]) == 1, 'missing/duplicate raw production GDB ' + field)
    require('func="shell_dispatch_command_line"' in row['breakpoint_stop'] and
            'reason="breakpoint-hit"' in row['breakpoint_stop'] and
            f'bkptno="{row["entry_breakpoint_number"]}"' in row['breakpoint_stop'], 'wrong dispatcher breakpoint')
    content = re.search(r'contents="([0-9a-f]+)"', row['payload_memory_response'])
    require(content is not None and bytes.fromhex(content[1]).split(b'\0', 1)[0] == payload.encode() and
            row['observed_payload'] == payload, 'production observed wrong payload')
    def value(field):
        match = re.fullmatch(r'[1-9][0-9]*\^done,value="(0x[0-9a-fA-F]+|[0-9]+)"', row[field])
        require(match is not None, 'malformed GDB value'); return int(match[1], 0)
    require(value('handler_status_response') == 127 and value('return_pc_response') ==
            value('return_address_response') == row['return_address'], 'production dispatcher return differs')
    require('reason="breakpoint-hit"' in row['return_stop'] and
            f'bkptno="{row["return_breakpoint_number"]}"' in row['return_stop'] and
            f'original-location="*0x{row["return_address"]:x}"' in row['return_breakpoint_response'], 'production return breakpoint')
    times = [row[k] for k in ('observer_armed_monotonic', 'trigger_monotonic', 'breakpoint_monotonic',
                              'handler_return_monotonic', 'completed_monotonic')]
    require(times == sorted(times) and row['gdb_return_code'] == 0, 'production observer lifecycle')
    require(Path(row['observer_armed_file']).name == row['observer_armed_file'] and
            (directory / 'qemu' / row['observer_armed_file']).is_file(), 'production observer arm receipt')


def verify_run(logfile, require_repeated=True, failure_experiment=None):
    require(failure_experiment in (None, 'context-failure', 'delayed-response', 'null-read',
        'stack-overflow', 'stack-timeout'), 'unknown failure experiment')
    directory = logfile.parent
    receipt = read_json(directory / 'launch.json')
    result = read_json(directory / 'result.json')
    candidate = read_json(directory / 'candidate.json')
    build_receipt = read_json(directory / 'build-receipt.json')
    candidate_id = E.verify_candidate(candidate, build_receipt)
    facts = read_json(directory / 'elf-facts.json')
    require(facts['elf_sha256'] == candidate['artifacts']['kernel.elf']['sha256'] and
            facts['symbols_sha256'] == digest(directory / 'symbols.txt') and
            facts['headers_sha256'] == digest(directory / 'elf.txt'), 'ELF facts binding')
    for item in build_receipt['commands']:
        for key in ('receipt', 'output'):
            E.safe(item[key]); require(digest(directory / item[key]) == item[key + '_sha256'], 'build raw proof hash')
        require(read_json(directory / item['receipt'])['exit_status'] == 0, 'build command failed')
    require(receipt['schema'] == 2 and receipt['candidate_id'] == result['candidate_id'] == candidate_id and
            receipt['source_sha256'] == candidate['source_sha256'] and
            receipt['build_receipt_sha256'] == candidate['build_receipt_sha256'], 'launch candidate/source binding')
    boot = read_json(directory / 'boot-receipt.json')
    for key in ('launch_id', 'vm_id', 'boot_id', 'candidate_id'):
        require(boot[key] == receipt[key] == result[key] and boot[key], 'independent launch/boot identity: ' + key)
    require(digest(directory / 'vm-identity.jsonl') == boot['qmp_sha256'], 'VM identity raw proof hash')
    qmp = [json.loads(l) for l in (directory / 'vm-identity.jsonl').read_text().splitlines()]
    requests = [r['message'] for r in qmp if r['direction'] == 'send' and r['message'].get('execute') == 'query-uuid']
    require(len(requests) == 1, 'missing/duplicate VM identity query')
    answers = [r['message'] for r in qmp if r['direction'] == 'receive' and r['message'].get('id') == requests[0]['id']]
    require(len(answers) == 1 and answers[0]['return']['UUID'] == receipt['vm_id'], 'actual VM identity differs')
    require(int(boot['process_stat'].split(' ', 1)[0]) == boot['pid'] and
            result['started_ns'] < receipt['monotonic_ns'] <= boot['observed_ns'] < result['finished_ns'], 'boot chronology/process identity')
    expected_state = 'FAIL' if failure_experiment in ('context-failure','null-read','stack-overflow','stack-timeout') else 'COLLECTED'
    require(result['status'] == expected_state and result['cleanup'], 'run not complete or not cleaned up')
    utc = [datetime.datetime.fromisoformat(value) for value in (result['started_utc'], receipt['utc'], boot['observed_utc'], result['finished_utc'])]
    require(all(value.tzinfo is not None for value in utc) and utc == sorted(utc), 'inconsistent UTC boot chronology')
    argv = receipt['argv']
    def arg(key):
        require(argv.count(key) == 1, f'launch requires exactly one {key}')
        return argv[argv.index(key) + 1]
    machine, accel, smp = arg('-machine').split(',')[0], arg('-accel').split(',')[0], int(arg('-smp').split(',')[0])
    profile = f'{machine}-{accel}-smp{smp}'
    cpu_model = 'host' if accel == 'kvm' else 'max'
    import direct_map_evidence as RAM
    ram_profile=result.get('ram_profile','normal')
    require(ram_profile in ('normal','high','high-fallback'),'unknown RAM layout profile')
    if ram_profile!='normal':
        require(RAM.enabled() and (machine,accel,smp)==('q35','kvm' if ram_profile=='high' else 'tcg',4) and
            result.get('control_profile','normal')=='normal' and result.get('device_profile','direct')=='direct' and
            not result['production'] and arg('-machine')=='q35,max-ram-below-4g=1G','RAM supplement launch identity')
        profile+='-high-ram'+('-fallback' if ram_profile=='high-fallback' else '')
        if ram_profile=='high-fallback':cpu_model='max,-pdpe1gb'
    else:require(arg('-machine')==machine,'undeclared machine property')
    if result['profile'] == 'q35-tcg-smp4-nx-disabled':
        require((machine, accel, smp) == ('q35', 'tcg', 4), 'NX-disabled profile launch mismatch')
        profile += '-nx-disabled'; cpu_model = 'max,-nx'
    if result['profile'] == 'q35-kvm-smp4-xapic':
        require((machine, accel, smp) == ('q35', 'kvm', 4), 'xAPIC profile launch mismatch')
        profile += '-xapic'; cpu_model = 'host,-x2apic'
    devices=[argv[i+1] for i,arg_value in enumerate(argv[:-1]) if arg_value=='-device']
    device_profile=result.get('device_profile','direct')
    require(device_profile in ('direct','hub-keyboard'),'unknown device topology')
    if device_profile=='hub-keyboard':
        require((machine,smp)==('q35',4) and result['profile']==profile+'-hub-keyboard' and
            not result['production'],'hub profile identity')
        require(devices==['nec-usb-xhci,id=xhci,msi=on,msix=off',
            'usb-hub,id=testhub,bus=xhci.0,port=1','usb-kbd,bus=xhci.0,port=1.1'],
            'hub keyboard launch topology')
        profile+='-hub-keyboard'
    else:
        require(devices==(['nec-usb-xhci,id=xhci,msi=on,msix=off','usb-kbd,bus=xhci.0'] if machine=='q35' else []),
                'direct keyboard launch topology')
    require(arg('-uuid') == receipt['vm_id'], 'launch UUID differs from independently observed VM')
    require(result['profile'] == profile, 'profile label differs from actual accelerator/topology')
    require(arg('-cpu') == cpu_model and arg('-m') == '2G', 'effective CPU/RAM differs')
    backend=result.get('serial_backend','socket')
    require(backend in ('socket','file'),'unknown serial observation backend')
    if backend=='file':
        require(arg('-serial')=='file:'+result['runtime_session']+'/r/qemu-serial.log' and
                result['timing_method']=='Host CLOCK_MONOTONIC observation after each file read(64), polling EOF at 0.5 ms; file buffering and host scheduling remain unquantified uncertainty, not guest execution time.',
                'serial file observation/launch mismatch')
    else: require(arg('-serial')=='chardev:observed_serial','serial socket observation/launch mismatch')
    require(accel in ('tcg', 'kvm') and machine in ('pc', 'q35'), 'unsupported launch profile')
    for kind, filename in [('elf', 'kernel.elf'), ('image', 'hobbyos.img')]:
        require(receipt[kind]['sha256'] == result['candidate_' + kind] == candidate['artifacts'][filename]['sha256'],
                'candidate image/ELF/manifest hash mismatch')
    require(candidate.get('source_sha256') and receipt.get('firmware') and receipt.get('qemu_version'), 'incomplete launch binding')
    fault = result['trace_enabled']
    require(receipt['fault_trace'] == fault and ('-no-reboot' in argv) == fault and
            (('int' in arg('-d').split(',') and 'cpu_reset' in arg('-d').split(',')) == fault), 'fault trace option mismatch')
    raw = logfile.read_bytes()
    require(raw.endswith(b'\n'), 'truncated serial line')
    transcript, byte_ends, offset = [], [], 0
    for line in raw.splitlines(keepends=True):
        offset += len(line); transcript.append(line.rstrip(b'\r\n').decode('ascii')); byte_ends.append(offset)
    topology_rows = []
    for line in transcript:
        match = re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)', line)
        if match: topology_rows.append((int(match[1]), (int(match[2]), match[3])))
    require(len(topology_rows) == smp and len(dict(topology_rows)) == smp and len(set(a[0] for _, a in topology_rows)) == smp,
            'independent online topology missing or duplicated')
    topology = dict(topology_rows)
    require(sum(v[1] == 'BSP' for v in topology.values()) == 1, 'independent boot role cardinality')
    commands = [json.loads(line) for line in (directory / 'framed.jsonl').read_text().splitlines()]
    require([r['sequence'] for r in commands] == list(range(1, len(commands) + 1)), 'command sequence duplicate/missing')
    require({r['vm_id'] for r in commands} == {receipt['vm_id']}, 'command VM differs from launch')
    if failure_experiment in ('stack-overflow','stack-timeout'):
        import stack_current as STACK
        import stack_guard_evidence as SG
        require(STACK.enabled() and profile in (('q35-tcg-smp1','q35-kvm-smp4') if
            failure_experiment=='stack-overflow' else ('q35-kvm-smp4',)) and fault and
            not result['production'] and not result['readback'],'isolated stack failure profile mismatch')
        proof=SG.destructive_records(directory,transcript,topology,commands,result,failure_experiment)
        timing(directory,raw)
        return {'status':'EXPECTED_FAILURE','candidate_id':candidate_id,'vm_id':receipt['vm_id'],
            'boot_id':receipt['boot_id'],'profile':profile,'experiment':failure_experiment,'proof':proof}
    if failure_experiment=='null-read':
        import low_address_evidence as LOW
        require(LOW.enabled() and profile in ('q35-tcg-smp1','q35-kvm-smp4') and fault and
            not result['production'] and not result['readback'],'isolated NULL failure profile mismatch')
        proof=LOW.destructive_records(directory,transcript,topology,commands,result)
        timing(directory,raw)
        return {'status':'EXPECTED_FAILURE','candidate_id':candidate_id,'vm_id':receipt['vm_id'],
            'boot_id':receipt['boot_id'],'profile':profile,'experiment':failure_experiment,'proof':proof}
    if failure_experiment:
        require(profile == 'q35-kvm-smp4' and fault and not result['production'] and not result['readback'],
                'isolated failure profile mismatch')
        spec = importlib.util.spec_from_file_location('tlb_failure_replay',Path(__file__).with_name('verify-tlb-shootdown.py'))
        tlb = importlib.util.module_from_spec(spec);spec.loader.exec_module(tlb)
        proof = tlb.failure_records(directory,transcript,byte_ends,topology,commands,failure_experiment,result)
        timing(directory,raw)
        return {'status':'EXPECTED_FAILURE','candidate_id':candidate_id,'vm_id':receipt['vm_id'],
                'boot_id':receipt['boot_id'],'profile':profile,'experiment':failure_experiment,'proof':proof}
    require(not FOUNDATION.boot_errors({'smp': str(smp)}, transcript), 'inherited boot contract failed')
    required_commands = {'mem', 'cpu', 'irq check', 'irq controllers', 'irq routes', 'irq boot'}
    if result.get('production'):
        absence = read_json(directory / 'absence.json')
        require(absence['status'] == 'PASS' and absence['elf_sha256'] == result['candidate_elf'], 'production binary absence receipt')
        symbols = (directory / 'symbols.txt').read_text()
        strings = (directory / 'compiled-strings.txt').read_text()
        require(digest(directory / 'compiled-strings.txt') == absence['compiled_strings_sha256'], 'production compiled string proof hash')
        for token in ('ptinspect', 'cpustate', 'memmap', '[CPU_CONTROL]', '[MEMORY_MAP]', '[MTRR]',
                      'cmd_ptinspect', 'cmd_cpustate', 'cmd_memmap', 'tlbtest', 'tlb_fixture', 'g_delayed_slot', 'g_slot_publication', 'g_active_generation',
                      'cpu_control_snapshot_irq', 'memory_observation_init', 'g_observer_tables', 'paging_inspect', 'page_table_decode'):
            require(token not in strings, 'production compiled diagnostic surface present: ' + token)
        for token in ('cmd_ptinspect', 'cmd_cpustate', 'cmd_memmap', 'cpu_control_snapshot_irq', 'g_slot_publication', 'paging_inspect'):
            require(token not in symbols, 'production helper/hook symbol present')
        require({'ptinspect', 'cpustate', 'memmap', 'mem', 'cpu', 'irq check'} <= {r['payload'] for r in commands}, 'production commands missing')
        stack_absent=('stacktest','stacktest overflow','stacktest timeout') if __import__('stack_current').enabled() else ()
        if stack_absent:
            require(set(stack_absent)|{'wxtest'} <= {r['payload'] for r in commands}, 'production stack/image rejection commands missing')
            for token in ('cmd_stacktest','guard_probe_fixup','guard_probe_read','guard_overflow_enter','stack_test_'):
                require(token not in symbols, 'production stack recovery/destructive symbol present: '+token)
        for row in commands:
            prof = dict(name=profile, smp=str(smp), vm_id=row['vm_id'])
            if row['payload'] in ('ptinspect', 'cpustate', 'memmap', 'tlbtest', 'phystest', 'mappingtest','rammap','rammap high','lowmap','lowmap crash')+ (('wxtest',) if WX.enabled() else ())+stack_absent:
                production_absence_transaction(row, directory, transcript, byte_ends, prof, result['candidate_elf'])
            else:
                errors = FOUNDATION.validate_production_transaction(row, directory, transcript, byte_ends, prof, result['candidate_elf'])
                require(not errors, 'production transport: ' + '; '.join(errors))
            expected = 127 if row['payload'] in ('ptinspect', 'cpustate', 'memmap', 'tlbtest', 'phystest', 'mappingtest','rammap','rammap high','lowmap','lowmap crash')+ (('wxtest',) if WX.enabled() else ())+stack_absent else 0
            require(row['handler_status'] == expected, 'production command absence/handler status')
        return {'profile': profile, 'candidate': result['candidate_elf'], 'production': True, 'commands': len(commands)}
    require(required_commands <= {r['payload'] for r in commands}, 'required inherited functional commands missing')
    generations, cpu_summaries, counts, maps, tlb_segments, mapping_segments = [], [], [], [], [], []
    for row in commands:
        prof = dict(name=profile, smp=str(smp), vm_id=row['vm_id'], input_profile='framed',
            command_input_observation='qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb')
        errors = FOUNDATION.validate_transaction(row, transcript, byte_ends, prof, result['candidate_elf'], directory)
        require(not errors, 'framed transport: ' + '; '.join(errors))
        require(row['handler_status'] == 0, 'handler failed: ' + row['payload'])
        segment = FOUNDATION.transaction_segment(transcript, row)
        if row['payload'] == 'cpustate':
            generation, summary, cpus = verify_cpu(segment, topology)
            generations.append(generation); cpu_summaries.append(summary)
        elif row['payload'] == 'ptinspect samples': counts.append(verify_pages(segment, candidate['symbols']))
        elif row['payload'] == 'memmap': maps.append(verify_memmap(segment))
        elif row['payload'] in ('tlbtest', 'tlbtest run'): tlb_segments.append(segment)
        elif row['payload']=='mappingtest':mapping_segments.append(segment)
        elif row['payload']=='mappingtest framebuffer-copy':
            spec=importlib.util.spec_from_file_location('device_copy_predicate',E.ROOT/'scripts/verify-device-mapping.py')
            device=importlib.util.module_from_spec(spec);spec.loader.exec_module(device)
            device.benchmark(segment,'FRAMEBUFFER_REFERENCE=1' in candidate['profile'])
        elif row['payload'] == 'phystest':
            spec=importlib.util.spec_from_file_location('physical_observation_predicate',E.ROOT/'scripts/verify-physical-translation.py')
            physical=importlib.util.module_from_spec(spec);spec.loader.exec_module(physical)
            pointer=physical.records(segment,'POINTER')
            require(pointer and pointer[0].get('name')=='active_root','physical root record absent')
            physical.fixture(segment,topology,physical.number(pointer[0],'physical'),candidate['symbols']['_kernel_start'],
                ram=RAM.activation(transcript) if RAM.enabled() else None)
        elif row['payload']=='wxtest':
            require(WX.enabled(),'image protection command requires its explicit successor contract')
        elif row['payload'] in ('stacktest','stacktest inventory','stacktest churn'):
            require(__import__('stack_current').enabled(),'stack command requires its explicit successor contract')
        elif row['payload']=='lowmap':
            import low_address_evidence
            require(low_address_evidence.enabled(),'NULL command requires its explicit successor contract')
            low_address_evidence.probe(segment,(directory/'symbols.txt').read_text(),topology)
        elif row['payload'] in ('rammap','rammap high'):
            require(RAM.enabled(),'RAM command requires its explicit successor contract')
        elif __import__('stack_current').enabled() and row['payload'] in (
                __import__('stack_guard_evidence').TASKMAN_PEAK_BEFORE+
                __import__('stack_guard_evidence').TASKMAN_PEAK_AFTER):
            __import__('stack_guard_evidence').taskman_command(row,segment)
        else:
            require(not FOUNDATION.evidence_errors(row, segment, smp), 'inherited command contract: ' + row['payload'])
    require(len(generations) >= (3 if require_repeated else 1) and all(a < b for a, b in zip(generations, generations[1:])),
            'repeated CPU generations missing/stale')
    require(len(counts) == len(maps) == 1, 'required commands missing/duplicated')
    if mapping_segments:
        import device_mapping_evidence
        blocks=device_mapping_evidence.split_traces((directory/'qemu-debugcon.log').read_bytes())['MAPPING']
        require(len(blocks)==len(mapping_segments),'mapping raw/transaction coverage')
        for segment,block in zip(mapping_segments,blocks):
            device_mapping_evidence.verify_mapping(block,segment,number(cpus[0],'cr3'),topology,
                'FRAMEBUFFER_REFERENCE=1' in candidate['profile'],usb_controller=machine=='q35',
                ram=RAM.activation(transcript) if RAM.enabled() else None)
    if tlb_segments:
        spec = importlib.util.spec_from_file_location('tlb_semantic_replay',
            Path(__file__).with_name('verify-tlb-shootdown.py'))
        tlb = importlib.util.module_from_spec(spec); spec.loader.exec_module(tlb)
        modes = [line for line in transcript if re.fullmatch(r'\[LAPIC\] System using X2?APIC', line)]
        require(len(modes) == 1, 'TLB actual APIC mode unavailable')
        previous = 0
        bound_trace = any(line.startswith('[TLB_TRACE]') for segment in tlb_segments for line in segment)
        blocks = tlb.trace_blocks(directory) if bound_trace else []
        if bound_trace: require(len(blocks) == len(tlb_segments), 'TLB command/trace cardinality differs')
        for index, segment in enumerate(tlb_segments):
            if bound_trace: segment = tlb.bind_trace(segment, blocks[index])
            proof = tlb.verify_tlb(segment, topology, number(cpus[0], 'cr3'),
                'X2APIC' in modes[0], previous)
            previous = proof['generation_last']
    verify_representatives(transcript)
    readback_files = any(directory.glob('page-readback*')) or (directory / 'table-snapshot.json').exists()
    require(bool(result['readback']) == bool(readback_files), 'readback claim contradicts collected artifacts')
    if result['readback']: verify_readback(directory, transcript, receipt, candidate_id, counts[0])
    observed = timing(directory, raw)
    return {'profile': profile, 'candidate': result['candidate_elf'], 'image': result['candidate_image'],
            'source': candidate['source_sha256'], 'commands': len(commands), 'generations': generations,
            'serial_backend':backend,'timing_method':result['timing_method'],
            'cpu_summaries': cpu_summaries, 'tables': counts[0], 'timing': observed,
            'trace': fault, 'readback': result['readback'], 'launch_id': receipt['launch_id'],
            'vm_id': receipt['vm_id'], 'boot_id': receipt['boot_id'], 'candidate_id': candidate_id,
            'started_ns': result['started_ns'], 'finished_ns': result['finished_ns'],
            'started_utc': result['started_utc'], 'finished_utc': result['finished_utc'],
            'process_identity': [receipt['host_boot_id'], boot['pid'], boot['process_stat'].rsplit(')', 1)[1].split()[19]],
            'firmware': [f['sha256'] for f in receipt['firmware']],
            'qemu_version': receipt['qemu_version'], 'logfile': str(logfile)}


MATRIX = {'q35-tcg-smp1': 3, 'q35-tcg-smp2': 1, 'q35-tcg-smp4': 3,
          'q35-kvm-smp4': 3, 'q35-kvm-smp8': 1, 'pc-tcg-smp4': 1, 'q35-kvm-smp24': 3}
BASELINE = {'q35-tcg-smp4': 3, 'q35-kvm-smp4': 3}
TLB_MATRIX = {profile: 3 for profile in MATRIX}
SERIES_SCOPES = {'full-matrix': MATRIX, 'baseline': BASELINE, 'nx-disabled': {'q35-tcg-smp4-nx-disabled': 3},
                 'tlb-matrix': TLB_MATRIX, 'xapic': {'q35-kvm-smp4-xapic': 3},
                 'hub-keyboard':{'q35-tcg-smp4-hub-keyboard':3,'q35-kvm-smp4-hub-keyboard':3},
                 'framebuffer-reference':BASELINE}
SERIES_SCOPES['high-ram']={'q35-kvm-smp4-high-ram':3}
SERIES_SCOPES['high-ram-fallback']={'q35-tcg-smp4-high-ram-fallback':3}


def attempt_clock(directory, start, finish):
    """Bind current attempt clocks to the recorded host boot, including a crash.

    Monotonic values from different host boots are never compared or rewritten.
    An externally sealed interruption has no observed completion timestamp.
    """
    if not WX.enabled():
        return {}
    launch = read_json(directory / 'launch.json')
    epoch = launch['host_boot_id']
    require(re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', epoch),
            'attempt host boot identity missing')
    require(launch['candidate_id'] == start['candidate_id'] and
            start.get('host_boot_id', epoch) == epoch and
            start['started_ns'] <= launch['monotonic_ns'], 'attempt host clock binding')
    clock = {'host_boot_id': epoch, 'started_utc': start['started_utc']}
    if 'interruption' in finish:
        ref = finish['interruption']; E.safe(ref['path'])
        proof = directory / ref['path']
        require(digest(proof) == ref['sha256'], 'interruption proof bytes changed')
        row = read_json(proof)
        require(finish['status'] == 'FAIL' and finish['finished_ns'] is None and
                row['status'] == 'INTERRUPTED_HOST_REBOOT' and row['classification'] == 'transport' and
                row['host_boot_id'] == epoch and row['recovered_host_boot_id'] != epoch and
                re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', row['recovered_host_boot_id']) and
                row['candidate_id'] == start['candidate_id'] and row['processes_alive'] is False,
                'interruption is not a failed prior-host attempt')
        names = set()
        for item in row['files']:
            E.safe(item['path']); path = directory / item['path']
            require(item['path'] not in names and path.is_file() and not path.is_symlink() and
                    path.stat().st_size == item['size'] and digest(path) == item['sha256'],
                    'interruption raw dependency changed')
            names.add(item['path'])
        require({'launch.json', 'boot-receipt.json', 'qemu-serial.log', 'qemu-debugcon.log',
                 'qemu-trace.log', 'serial-receipts.bin'} <= names, 'interruption raw closure incomplete')
        raw = read_json(directory / 'result.json')
        require(raw['status'] == 'FAIL' and raw['finished_ns'] is None and raw['cleanup'] is True and
                raw['candidate_id'] == start['candidate_id'] and raw['interruption'] == ref,
                'interruption result binding')
        clock.update(interrupted=True, recovered_host_boot_id=row['recovered_host_boot_id'],
                     recovered_utc=row['recovered_utc'])
    return {'clock': clock}


def attempt_order(attempts):
    """Check one monotonic domain per host boot and explicit cross-boot order."""
    previous_end = -1; previous_epoch = None; closed = set(); previous_utc = None
    interrupted = None
    for attempt in attempts:
        clock = attempt.get('clock')
        if clock is None:
            require(previous_epoch is None, 'attempt host clock coverage missing')
            require(attempt['started_ns'] > previous_end and
                    attempt['finished_ns'] > attempt['started_ns'], 'inconsistent attempt chronology')
            previous_end = attempt['finished_ns']; continue
        require(WX.enabled(), 'host clock successor used outside current contract')
        epoch = clock['host_boot_id']
        utc = datetime.datetime.strptime(clock['started_utc'], '%Y%m%dT%H%M%S.%fZ').replace(tzinfo=datetime.timezone.utc)
        if previous_epoch is not None and epoch != previous_epoch:
            closed.add(previous_epoch)
            require(epoch not in closed and utc > previous_utc, 'host boot epoch order changed')
            previous_end = -1
        if interrupted is not None:
            require(epoch == interrupted['recovered_host_boot_id'] and
                    utc >= datetime.datetime.fromisoformat(interrupted['recovered_utc']),
                    'interrupted attempt resumed in wrong host epoch')
        require(attempt['started_ns'] > previous_end, 'inconsistent attempt chronology')
        interrupted = clock if clock.get('interrupted') else None
        if interrupted:
            require(attempt['status'] == 'FAIL' and attempt['finished_ns'] is None,
                    'interruption cannot supply a successful completion')
            previous_end = attempt['started_ns']
        else:
            require(isinstance(attempt['finished_ns'], int) and
                    attempt['finished_ns'] > attempt['started_ns'], 'inconsistent attempt chronology')
            previous_end = attempt['finished_ns']
        previous_epoch = epoch; previous_utc = utc


def series_policy(attempts, scope):
    """Aggregate policy consumes validated run facts; hosted mocks test only this layer."""
    require(scope in SERIES_SCOPES, 'unknown series scope')
    required = SERIES_SCOPES[scope]
    require([r['sequence'] for r in attempts] == list(range(1, len(attempts) + 1)), 'missing/duplicate attempt sequence')
    paths, launches, boots, vms, processes = set(), set(), set(), set(), set()
    selected = {p: [] for p in required}
    attempt_order(attempts)
    candidates = set()
    for attempt in attempts:
        require(attempt['path'] not in paths, 'duplicate canonical run path')
        paths.add(attempt['path'])
        profile = attempt['profile']
        require(profile in selected, 'unexpected profile in functional series')
        candidates.add(attempt['candidate_id'])
        if attempt['status'] != 'PASS':
            selected[profile] = []
            continue
        r = attempt['validated']
        if 'clock' in attempt:
            require(r['process_identity'][0] == attempt['clock']['host_boot_id'], 'run host epoch differs')
        require(r['profile'] == profile and r['candidate_id'] == attempt['candidate_id'], 'attempt profile/candidate change')
        require(not r['trace'] and not r['readback'], 'diagnostic run cannot be a functional/baseline sample')
        for value, seen, label in [(r['launch_id'], launches, 'launch'), (r['vm_id'], vms, 'VM'),
                                   (r['boot_id'], boots, 'boot'), (tuple(r['process_identity']), processes, 'process')]:
            require(value and value not in seen, 'duplicate independent ' + label + ' identity')
            seen.add(value)
        require(attempt['started_ns'] <= r['started_ns'] < r['finished_ns'] <= attempt['finished_ns'],
                'run outside recorded attempt chronology')
        selected[profile].append(r)
    require(len(candidates) == 1, 'mixed candidates in frozen series')
    medians, runs = {}, []
    for profile, minimum in required.items():
        streak = selected[profile]
        require(len(streak) >= minimum, 'required consecutive profile/repetition missing: ' + profile)
        streak = streak[-minimum:]
        require(len({tuple(r['firmware']) for r in streak}) == 1 and len({r['qemu_version'] for r in streak}) == 1,
                'profile firmware/tool changed within series')
        runs.extend(r['path'] for r in streak)
        if profile in BASELINE:
            values = [r['timing']['duration_ns'] for r in streak]
            medians[profile] = {'samples_ns': values, 'median_ns': statistics.median(values)}
    return {'runs': runs, 'baseline': medians}


def verify_series(root, manifest=None):
    root = root.resolve()
    manifest = read_json(root / 'series.json') if manifest is None else manifest
    require(manifest['schema'] == 2, 'unsupported series schema')
    require(E.identity(manifest['verifier_sources']) == manifest['verifier_sha256'], 'collected verifier manifest digest')
    # A later oracle may replay old raw evidence. Identify both versions; final
    # acceptance separately requires fixtures for the actual replay version.
    replay_sources = E.manifest(harness=True)
    declared = manifest['attempts']
    actual = sorted(p.relative_to(root).as_posix() for p in (root / 'campaign-attempts').glob('*.json')
                    if not p.name.endswith('.result.json'))
    require(declared == actual and len(declared) == len(set(declared)), 'missing/unlisted campaign attempts')
    attempts = []
    canonical_paths = set()
    for name in declared:
        E.safe(name)
        start = read_json(root / name)
        finish = read_json((root / name).with_suffix('.result.json'))
        E.safe(start['path'])
        directory = root / start['path']
        path = str(directory.resolve())
        require(path not in canonical_paths and directory.resolve().is_relative_to(root), 'duplicate/unsafe canonical run path')
        canonical_paths.add(path)
        require(finish['start_sha256'] == digest(root / name), 'attempt receipt changed')
        r = dict(start, status=finish['status'], finished_ns=finish['finished_ns'],
                 **attempt_clock(directory, start, finish))
        if finish['status'] == 'PASS':
            validated = verify_run(directory / 'qemu-serial.log')
            validated['path'] = start['path']
            require(finish['result_sha256'] == digest(directory / 'result.json'), 'attempt result binding')
            r['validated'] = validated
        else:
            require((directory / 'result.json').is_file() or finish.get('reason'), 'failed attempt proof missing')
        attempts.append(r)
    observed_dirs = {p.parent.relative_to(root).as_posix() for p in (root / 'profiles').glob('*/result.json')}
    require(observed_dirs == {r['path'] for r in attempts}, 'run omitted from attempt inventory')
    derived = series_policy(attempts, manifest['scope'])
    require(manifest['runs'] == derived['runs'] and manifest['baseline'] == derived['baseline'],
            'series selection or baseline median/samples differs')
    require(len(manifest['runs']) == len(set(manifest['runs'])), 'repeated run in selection')
    if manifest['scope'] in ('full-matrix', 'tlb-matrix'):
        require(manifest['diagnostics'], 'independent topology/readback campaign missing')
        if 'diagnostic_attempts' in manifest:
            actual = [{'path': p.parent.relative_to(root).as_posix(), 'result_sha256': digest(p),
                       'status': read_json(p)['status']}
                      for p in sorted((root / 'diagnostics').glob('*/result.json'))]
            require(manifest['diagnostic_attempts'] == actual, 'missing/changed diagnostic attempt')
            require(set(manifest['diagnostics']) <= {a['path'] for a in actual if a['status'] == 'COLLECTED'},
                    'selected diagnostic did not complete')
        for name in manifest['diagnostics']:
            E.safe(name); r = verify_run(root / name / 'qemu-serial.log')
            require(r['readback'] and r['candidate_id'] == attempts[0]['candidate_id'], 'independent topology candidate mismatch')
    return {'status': 'PASS', 'scope': manifest['scope'], 'runs': len(derived['runs']), 'baseline': derived['baseline'],
            'collected_verifier_sha256': manifest['verifier_sha256'], 'replay_verifier_sha256': E.identity(replay_sources)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--logfile', type=Path, help='Raw serial file; receipts and framed artifacts must be alongside it.')
    group.add_argument('--series', type=Path, help='Directory with explicit series.json; never scans stale defaults.')
    args = parser.parse_args()
    try:
        result = verify_run(args.logfile) if args.logfile else verify_series(args.series)
        print(json.dumps({'status': 'PASS', 'result': result}, indent=2))
        return 0
    except (ValueError, KeyError, TypeError, OSError, StopIteration, IndexError) as error:
        print(json.dumps({'status': 'FAIL', 'reason': str(error)}))
        return 1


if __name__ == '__main__':
    sys.exit(main())
