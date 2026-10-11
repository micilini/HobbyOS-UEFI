"""Explicit successor scope rooted in the approved control-state commit."""
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
import memory_evidence as E
import cpu_current

ROOT = Path(__file__).resolve().parent.parent


def functions(source):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source, flags=re.S)
    result = {}
    pattern = r'(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s*]*?\b(\w+)\s*\([^;{}]*\)\s*\{'
    for match in re.finditer(pattern, masked):
        depth = 1; end = match.end()
        while depth and end < len(masked):
            depth += (masked[end] == '{') - (masked[end] == '}'); end += 1
        E.require(not depth, 'unbalanced C function')
        result[match[1]] = source[match.start():end]
    return result


def instruction_sites(root=ROOT):
    result = cpu_current.instruction_sites(root)
    for path in sorted((root/'kernel').rglob('*')):
        if path.suffix not in ('.c', '.h'): continue
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
        ordinal = {}
        for block in re.findall(r'\b(?:__asm__|__asm|asm)\s*(?:(?:volatile|__volatile__|__volatile)\s*)?\((?:"(?:\\.|[^"\\])*"|[^;])*?\);', source, re.S):
            if not re.search(r'\b(mfence|lfence)\b', block): continue
            if re.search(r'\b(cpuid|wrmsr|rdmsr|wbinvd|invd|invlpg)\b|\bcr[034]\b', block): continue
            text = ' '.join(block.split()); ordinal[text] = ordinal.get(text, 0) + 1
            result.append({'path':path.relative_to(root).as_posix(), 'label':'',
                           'instruction':text, 'occurrence':ordinal[text]})
    return result


def region_identity(path, text):
    if path == 'kernel/src/apic/lapic.c':
        for old, new in (
            ('lapic_lvt_mask(LAPIC_LVT_ERROR, 0xFEu)', 'lapic_lvt_mask(LAPIC_LVT_ERROR, INT_VECTOR_LAPIC_ERROR)'),
            ('lapic_write(LAPIC_SPURIOUS, 0xFFu |', 'lapic_write(LAPIC_SPURIOUS, INT_VECTOR_SPURIOUS |'),
            ('state.programmed_vector == 34u', 'state.programmed_vector == INT_VECTOR_LAPIC_TIMER')):
            text = text.replace(new, old)
    return E.identity(text)


def contract(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.contract(root)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'physical-translation':
        import physical_current
        return physical_current.contract(root)
    value = json.loads((root/'scripts/tlb-coherency-scope.json').read_text())
    E.require(value['base_commit'] == '18e64098519e3787e5f5ced2328d60ef8084964c' and
              value['base_tree'] == 'e07550842e79512a0dc770728bb0e2b919c71ad6', 'invalidation scope baseline')
    for name, digest in value['historical_contracts'].items():
        E.require(E.sha(root/name) == digest, 'historical scope modified: '+name)
    for name, digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root/name) == digest, 'invalidation protected anchor drift: '+name)
    E.require(E.sha(root/'kernel/src/core/interrupts.c') == value['interrupt_dispatch_sha256'],
              'invalidation protected anchor drift: dedicated interrupt dispatch')
    lapic_header_contract((root/'kernel/src/apic/lapic.h').read_text())
    for name, expected in value['protected_functions'].items():
        actual = functions((root/name).read_text())
        for symbol, digest in expected.items():
            E.require(symbol in actual and region_identity(name, actual[symbol]) == digest,
                      'protected function drift: '+name+':'+symbol)
    return value


def lapic_header_contract(source):
    """Only the three checked send declarations may change the approved ABI."""
    checked = '''#include "lapic_delivery.h"
lapic_delivery_result_t lapic_send_ipi(uint32_t apic_id, uint32_t vector);
lapic_delivery_result_t lapic_send_init(uint32_t apic_id);
lapic_delivery_result_t lapic_send_sipi(uint32_t apic_id, uint32_t trampoline_page);'''
    original = '''void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_send_init(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint32_t trampoline_page);'''
    E.require(source.count(checked) == 1 and
              hashlib.sha256(source.replace(checked, original).encode()).hexdigest() ==
              '19f4bc74f4dceb328b6de4c7f0ca2ccae5b904f42a75ac36f63b3f1a105f753d',
              'LAPIC declarations exceed the checked-send contract')


def audit_sites(root=ROOT):
    value = contract(root)
    actual = instruction_sites(root)
    E.require(actual == value['instruction_sites'], 'unauthorized architectural instruction site')
    return {'sites':len(actual), 'sha256':E.identity(actual)}


def dispatch_semantics(source):
    body=functions(source)['irq_external_dispatch']
    start=body.index('else if (vector == INT_VECTOR_CPU_REQUEST)')
    end=body.index('else if (vector == INT_VECTOR_SPURIOUS)',start)
    expected='''else if (vector == INT_VECTOR_CPU_REQUEST)
    {
        irq_stats_record(vector);
        cpu_request_irq();
        lapic_eoi();
    }
    '''
    E.require(body[start:end]==expected,'request dispatch EOI/order mismatch')
    E.require(body.count('cpu_request_irq();')==1 and body.index('interrupt_context_enter(vector);')<start and
              body.rindex('interrupt_context_exit(vector);')>end,'request dispatch context accounting mismatch')
    return {'vector':225,'service_calls':1,'eoi_calls_in_request_branch':1,'context_accounting':'PRESERVED'}


def caller_semantics(root=ROOT):
    boot=functions((root/'kernel/src/smp/smp_boot.c').read_text())['smp_boot_aps']
    branches=re.findall(r'if \(delivery != LAPIC_DELIVERY_SUBMITTED\) \{(.*?)\n\s*\}',boot,re.S)
    E.require(len(branches)==2 and all('code=AP_DELIVERY' in branch and
              'kpanic("AP checked IPI delivery failed");' in branch for branch in branches),
              'checked AP send failure can be erased by late ONLINE publication')
    observer=(root/'kernel/src/cpu/cpu_control_snapshot.c').read_text()
    E.require('!= LAPIC_DELIVERY_SUBMITTED)\n                g_capture.error_mask |= 1u << slot;' in observer,
              'observer checked send failure discarded')
    wake=functions((root/'kernel/src/core/irq_bootstrap.c').read_text())['irq_bootstrap_wake_missing_cpus']
    E.require('result != LAPIC_DELIVERY_SUBMITTED' in wake and
              'irq_bootstrap_runtime_abort(IRQ_RUNTIME_ABORT_HARD_FAILURE);' in wake,
              'rendezvous checked send failure discarded')
    return {'boot_delivery_errors':'NONRETURNING_EXISTING_PANIC','observer':'RETAINED_ERROR_MASK','rendezvous':'HARD_FAILURE'}


def scope(family):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.scope(family)
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') == 'physical-translation':
        import physical_current
        return physical_current.scope(family)
    value = contract()
    names = subprocess.check_output(['git','diff','--name-only','-z',value['base_commit']],cwd=ROOT).decode().split('\0')
    names += subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
    changed = {n for n in names if n and Path(n).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed <= set(value['allowed_paths']), 'unrelated invalidation source edits: '+', '.join(sorted(changed-set(value['allowed_paths']))))
    return {'schema':1,'status':'PASS','family':family,'contract':value['name'],
            'base_commit':value['base_commit'],'changed_paths':sorted(changed),
            'scope_contract_sha256':E.sha(ROOT/'scripts/tlb-coherency-scope.json'),
            'protected':value['immutable_reviewed_anchors']}
