"""Bounded raw ELF fragments for portable structural replay.

The full binary stays local. Its measured identity is retained, but replay only
rechecks the captured headers, symbols, bootstrap/trampoline and fixup opcodes.
Uncaptured code is not reconstructed or claimed to have been executed.
"""
import hashlib
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import memory_evidence as E

spec = importlib.util.spec_from_file_location('memory_layout_native', Path(__file__).with_name('verify-section-layout.py'))
V = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = V
spec.loader.exec_module(V)


def ranges(elf):
    h = elf.header
    selected = [(0, elf.EHDR.size), (h['phoff'], h['phnum'] * h['phentsize']),
                (h['shoff'], h['shnum'] * h['shentsize'])]
    for section in elf.sections:
        if section['type'] in (V.SHT_SYMTAB, V.SHT_STRTAB) or section['name'] in (
                '.boot_text', '.trampoline', '.cpu_msr_fixup', '.layout_probe'):
            selected.append((section['offset'], section['size']))
    text = elf.section('.text')
    fixup = elf.section_data(elf.section('.cpu_msr_fixup'))
    for offset in range(0, len(fixup), 16):
        site, target = struct.unpack_from('<QQ', fixup, offset)
        for address in (site, target):
            E.require(text['addr'] <= address < text['addr'] + text['size'], 'ELF fixup fragment outside text')
            selected.append((text['offset'] + address - text['addr'], min(32, text['addr'] + text['size'] - address)))
    if __import__('os').environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        from kernel_image_evidence import fixed_probe_ranges
        selected.extend(fixed_probe_ranges(elf))
    if __import__('os').environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
        from stack_guard_evidence import fixed_probe_ranges as stack_ranges
        selected.extend(stack_ranges(elf))
        for name in ('scheduler_reap_zombies','kernel_stack_retire'):
            symbol=elf.symbol(name)
            E.require(symbol['shndx']==text['index'] and 0<symbol['size']<16384,
                'ELF stack reaper fragment extent')
            selected.append((text['offset']+symbol['value']-text['addr'],symbol['size']))
    merged = []
    for offset, size in sorted(selected):
        if not size: continue
        E.require(0 <= offset < len(elf.data) and size <= len(elf.data) - offset, 'ELF fragment bounds')
        end = offset + size
        if merged and offset <= merged[-1][1]: merged[-1][1] = max(end, merged[-1][1])
        else: merged.append([offset, end])
    return merged


def capture(path, *, orphan_payload=None):
    elf = V.ElfFile.from_path(path)
    if orphan_payload is None:
        layout = V.validate_layout(elf)
        del layout['elf_sha256']
    else:
        V.validate_orphan_binary(elf, orphan_payload)
        layout = {'orphan_payload_hex': orphan_payload.hex()}
    chunks = []
    for start, end in ranges(elf):
        raw = elf.data[start:end]
        chunks.append({'offset': start, 'size': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(), 'hex': raw.hex()})
    result = {'schema': 1, 'kind': 'bounded-raw-ELF-structure', 'elf_sha256': E.sha(path),
              'elf_size': len(elf.data), 'chunks': chunks, 'layout': layout,
              'limitation': 'Uncaptured code and the full binary hash cannot be recomputed from these fragments.'}
    replay(result)
    return result


def replay(proof):
    E.require(proof.get('schema') == 1 and proof.get('kind') == 'bounded-raw-ELF-structure', 'ELF fragment schema')
    size = proof['elf_size']
    E.require(isinstance(size, int) and 64 <= size <= 32 * 1024 * 1024, 'ELF fragment size bound')
    data = bytearray(size)
    observed = []
    for row in proof['chunks']:
        start = row['offset']; raw = bytes.fromhex(row['hex']); end = start + len(raw)
        E.require(len(raw) == row['size'] and hashlib.sha256(raw).hexdigest() == row['sha256'], 'ELF fragment bytes/hash')
        E.require(0 <= start < end <= size and (not observed or start > observed[-1][1]), 'ELF fragment overlap/order')
        data[start:end] = raw; observed.append([start, end])
    elf = V.ElfFile(bytes(data), 'bounded structural proof; uncaptured bytes are placeholders')
    E.require(ranges(elf) == observed, 'ELF required fragment coverage')
    if 'orphan_payload_hex' in proof['layout']:
        V.validate_orphan_binary(elf, bytes.fromhex(proof['layout']['orphan_payload_hex']))
        result = {'orphan_payload_hex': proof['layout']['orphan_payload_hex']}
    else:
        result = V.validate_layout(elf)
        del result['elf_sha256']
    E.require(result == proof['layout'], 'ELF structural result differs')
    return elf


def fixture_controls(proof):
    """Exercise transport and semantic failures in the real structural parser."""
    replay(proof)
    cases = []
    def reject(name, value, boundary):
        try: replay(value)
        except (ValueError, V.ValidationError) as error:
            E.require(boundary in str(error), 'ELF fixture rejected at another boundary: ' + str(error))
            cases.append({'name': name, 'synthetic': True, 'expected_exit': 1, 'actual_exit': 1,
                          'expected_boundary': boundary, 'reason': str(error), 'materialized_sha256': E.identity(value)})
        else: raise ValueError('ELF fixture accepted: ' + name)
    value = copy.deepcopy(proof); value['chunks'].pop()
    reject('missing-section-table-bytes', value, 'ELF_HEADER_INVALID')
    value = copy.deepcopy(proof); value['chunks'].insert(1, value['chunks'][0])
    reject('duplicated-fragment', value, 'ELF fragment overlap/order')
    value = copy.deepcopy(proof); value['chunks'][0]['hex'] = value['chunks'][0]['hex'][:-2]
    reject('truncated-fragment', value, 'ELF fragment bytes/hash')
    elf = replay(proof)
    def patch(name, offset, replacement, boundary):
        value = copy.deepcopy(proof)
        found = 0
        for row in value['chunks']:
            if row['offset'] <= offset and offset + len(replacement) <= row['offset'] + row['size']:
                data = bytearray.fromhex(row['hex']); start = offset - row['offset']
                data[start:start + len(replacement)] = replacement
                row['hex'] = data.hex(); row['sha256'] = hashlib.sha256(data).hexdigest(); found += 1
        E.require(found == 1, 'semantic ELF mutation is outside captured bytes')
        reject(name, value, boundary)
    patch('missing-boundary-symbol', elf.symbol('_text_start')['string_file_offset'], b'X', 'SYMBOL_MISSING')
    fixup = elf.section_data(elf.section('.cpu_msr_fixup'))
    site = struct.unpack_from('<Q', fixup)[0]; text = elf.section('.text')
    patch('wrong-protected-opcode', text['offset'] + site - text['addr'], b'\xff', 'MSR_FIXUP_SITE_OPCODE_INVALID')
    return {'status': 'PASS', 'base_sha256': E.identity(proof), 'cases': cases}


def replay_native_fixtures(evidence, normal, orphan, *, expected_result=None):
    """Materialize explicitly synthetic structural ELFs in a disposable tree.

    Uncaptured code is zero-filled and is never executed. The native fixture
    producer mutates the same captured headers/symbols/opcodes as its local run.
    Original full-binary fixture identities remain in the local artifact index.
    """
    campaign = json.loads((evidence / 'campaign.json').read_text())
    pair = json.loads((evidence / campaign['pairs']['debug-off']).read_text())
    orphan_row = json.loads((evidence / campaign['orphan_probe']).read_text())
    with tempfile.TemporaryDirectory(prefix='memory-structural-fixtures-') as temp:
        root = Path(temp)
        files = { 'campaign.json': (evidence / 'campaign.json').read_bytes(),
                  campaign['pairs']['debug-off']: E.canonical(pair),
                  campaign['orphan_probe']: E.canonical(orphan_row),
                  pair['objects']: (evidence / pair['objects']).read_bytes(),
                  pair['after']['map']: (evidence / pair['after']['map']).read_bytes(),
                  pair['before']['elf']: normal.data, pair['after']['elf']: normal.data,
                  orphan_row['elf']: orphan.data }
        for name, data in files.items():
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        result = V.run_fixtures(root, root / 'generated')
        original = expected_result if expected_result is not None else json.loads((evidence / 'fixtures/results.json').read_text())
        fields = ('id', 'expected', 'observed', 'status')
        E.require([{k: r[k] for k in fields} for r in result['records']] ==
                  [{k: r[k] for k in fields} for r in original['records']], 'native structural fixture boundaries differ')
        return {'kind': 'synthetic-structural-subset-replay', 'positive': result['valid_controls'],
                'negative': result['negative_controls'], 'executed_guest_code': False}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=False)
    proof = capture(args.elf); E.write(args.output / 'proof.json', proof)
    result = fixture_controls(proof); E.write(args.output / 'result.json', result)
    print(json.dumps({'status': 'PASS', 'negative': len(result['cases'])}))
