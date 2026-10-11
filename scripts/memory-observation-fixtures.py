#!/usr/bin/env python3
"""Hosted verifier controls and intentional rejected mutations; never runtime evidence."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import sys

SPEC = importlib.util.spec_from_file_location('memory_verifier', Path(__file__).with_name('verify-memory-observation.py'))
V = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(V)


def record(group, kind, **fields):
    return f'[{group}][{kind}] schema=1 ' + ' '.join(f'{k}={v}' for k, v in fields.items())


def cpu_fixture():
    lines = []
    for slot in range(2):
        lines.append(record('CPU_CONTROL', 'TARGET', generation=1, slot=slot, apic=slot * 2))
        lines.append(record('CPU_CONTROL', 'CPU', generation=1, slot=slot, apic=slot * 2,
            role='AP' if slot else 'BSP', identity_valid=1, cr_valid=1,
            cr0='0x80000011' if slot else '0x80010033', cr3='0x1000', cr4='0x20',
            efer='0xd00', efer_valid=1, pat='0x0007040600070406', pat_supported=1, pat_valid=1,
            cpuid1_edx='0x10020', extended_edx='0x20000000', address_eax='0x24', physical_bits=36))
    lines.append(record('CPU_CONTROL', 'SUMMARY', generation=1, target='0x3', acknowledged='0x3', errors=0,
        missing=0, collection_complete='true', controls_uniform='false', uniform_cr0=0, uniform_cr3_raw=1,
        uniform_cr4=1, uniform_efer=1, uniform_pat=1, uniform_cr3_root=1, apic_mode='xapic', reason='collected'))
    return lines


def page_fixture():
    c = dict(va='0', cr0='0x80010000', cr3='0x1000', cr4='0x20', efer='0xc00', efer_valid='1',
        physical_bits='36', nx_supported='1', page1g_supported='1', pat_supported='1', pat_valid='1', pat='0x7040600070406')
    entries = []
    for level, table, raw in [(4, 0x1000, 0x2003), (3, 0x2000, 0x3003), (2, 0x3000, 0x4003), (1, 0x4000, 3)]:
        e = dict(level=str(level), table=hex(table), raw=hex(raw), index='0', P='1', RW='1', US='0',
            PWT='0', PCD='0', A='0', D='0' if level == 1 else 'NA', PS='0' if level in (2, 3) else 'NA',
            PAT='0' if level == 1 else 'NA', G='0' if level == 1 else 'NA', NX='0')
        entries.append(e)
    r = dict(va='0', root='0x1000', entries='4', effective_type='NOT_COMPUTED', status='MAPPED', stopped='1',
             leaf='1', page_size='4096', base='0', offset='0', physical='0', effective_rw='1', effective_us='0',
             effective_nx='0', supervisor_write_protected='0', pat_index='0', pat_type_raw='6', pat_type_valid='1', pat_type='WB')
    return c, entries, r


def state_fixture():
    # A bounded model of the documented CAS states, including terminal quarantine.
    generation, active, owner, quarantine = 1, 1, True, False
    words = {0: 5, 2: 5}
    def claim(slot, observed):
        if slot not in words or words[slot] != ((observed << 2) | 1): return False
        words[slot] = (observed << 2) | 2
        return True
    assert owner and active == generation
    assert claim(2, 1) and not claim(2, 1)  # duplicate cannot claim an owned slot
    words[2] = 7
    assert not claim(0, 0) and not claim(4, 1)
    assert claim(0, 1)
    quarantine, active = True, 0  # requester deadline before slot 0 completion
    assert quarantine and active == 0
    words[0] = 7  # late writer has permanent static storage, never a newer request
    assert quarantine and owner  # no buffer reuse while formatter owns the result
    owner = False
    assert quarantine and not owner  # a later request is rejected until reboot
    return {'completion_order': [2, 0], 'duplicate_claim': False, 'invalid_identity_claim': False,
            'stale_generation_claim': False, 'late_after_timeout': 'storage_retained',
            'next_request': 'quarantined', 'concurrent_request': 'busy'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, required=True)
    a = p.parse_args()
    a.evidence.mkdir(parents=True, exist_ok=False)
    ledger = []
    def case(name, data, function, reject=False):
        (a.evidence / (name + '.json')).write_text(json.dumps(data, indent=2) + '\n')
        error = None
        try: function(data)
        except (ValueError, KeyError, TypeError, IndexError) as e: error = str(e)
        actual = 1 if error else 0
        expected = 1 if reject else 0
        ledger.append(dict(name=name, expected_exit=expected, actual_exit=actual, error=error, source='hosted_fixture'))
        if actual != expected: raise ValueError(f'{name}: expected {expected}, got {actual}: {error}')
    cpu = cpu_fixture()
    check_cpu = lambda lines: V.verify_cpu(lines, {0: (0, 'BSP'), 1: (2, 'AP')})
    case('nonuniform-complete', cpu, check_cpu)
    out_of_order = cpu[2:4] + cpu[:2] + cpu[4:]
    case('out-of-order-complete', out_of_order, check_cpu)
    for name, mutated in [
        ('swapped-cpu-roles', [line.replace('role=BSP', 'role=TEMP').replace('role=AP', 'role=BSP').replace('role=TEMP', 'role=AP') for line in cpu]),
        ('missing-cpu', cpu[:3] + cpu[4:]),
        ('duplicate-cpu', cpu[:4] + [cpu[3]] + cpu[4:]),
        ('invalid-identity', [line.replace('slot=1 apic=2', 'slot=1 apic=7') if '[CPU]' in line else line for line in cpu]),
        ('stale-generation', [line.replace('generation=1', 'generation=2') if '[CPU]' in line else line for line in cpu]),
        ('false-uniform', [line.replace('controls_uniform=false', 'controls_uniform=true').replace('uniform_cr0=0', 'uniform_cr0=1') for line in cpu]),
        ('unsupported-valid-msr', [line.replace('cpuid1_edx=0x10020', 'cpuid1_edx=0x20') for line in cpu]),
        ('false-completeness', [line.replace('acknowledged=0x3', 'acknowledged=0x1') for line in cpu]),
        ('timeout', [line.replace('collection_complete=true', 'collection_complete=false').replace('reason=collected', 'reason=missing_cpu_response') for line in cpu]),
        ('busy', [record('CPU_CONTROL', 'ERROR', reason='busy')])]:
        case(name, mutated, check_cpu, True)
    optional = [line.replace('cpuid1_edx=0x10020', 'cpuid1_edx=0x20').replace('pat_supported=1', 'pat_supported=0').replace('pat_valid=1', 'pat_valid=0').replace('uniform_pat=1', 'uniform_pat=NA') for line in cpu]
    case('unsupported-optional-pat', optional, check_cpu)
    page = page_fixture()
    check_page = lambda data: V.verify_page(*data)
    case('physical-zero-mapped', page, check_page)
    unmapped = copy.deepcopy(page)
    unmapped[1][-1].update(raw='0', **dict.fromkeys(('RW', 'US', 'PWT', 'PCD', 'A', 'D', 'PAT', 'G', 'NX'), 'NA'), P='0')
    unmapped[2].update(status='NOT_PRESENT', leaf='0', page_size='0')
    case('intentional-unmapped', unmapped, check_page)
    for name, change in [
        ('wrong-translation', lambda d: d[2].update(physical='1')),
        ('hidden-ancestor-rw', lambda d: d[1][0].update(raw='0x2001', RW='0')),
        ('hidden-ancestor-us', lambda d: d[2].update(effective_us='1')),
        ('bad-pat-slot', lambda d: d[2].update(pat_index='4')),
        ('missing-pat-read', lambda d: d[0].update(pat_valid='0')),
        ('nx-without-nxe', lambda d: d[1][0].update(raw='0x8000000000002003', NX='1')),
        ('effective-type-claim', lambda d: d[2].update(effective_type='WB')),
        ('invalid-ps', lambda d: d[1][0].update(raw='0x2083')),
        ('huge-misalignment', lambda d: d[1][1].update(raw='0x3083', PS='1'))]:
        mutated = copy.deepcopy(page); change(mutated); case(name, mutated, check_page, True)
    false_zero = copy.deepcopy(unmapped); false_zero[2].update(status='MAPPED', leaf='1', page_size='4096')
    case('absent-falsely-zero-mapped', false_zero, check_page, True)
    state = state_fixture()
    (a.evidence / 'mailbox-state-model.json').write_text(json.dumps(state, indent=2) + '\n')
    result = {'status': 'PASS', 'source': 'hosted_fixtures', 'checks': len(ledger),
              'negative': sum(r['expected_exit'] for r in ledger), 'cases': ledger,
              'limitation': 'Mailbox model checks transitions, not a replacement for real CPU concurrency runs.'}
    (a.evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__': main()
