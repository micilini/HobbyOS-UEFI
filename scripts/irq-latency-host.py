#!/usr/bin/env python3
"""Run the actual interrupt-latency C implementation with sanitizer receipts."""
import argparse
import importlib.util
import json
import re
from pathlib import Path
import memory_evidence as E

SUBJECTS = {
    'spinlock-release': ['scripts/spinlock-release-host.c'],
    'pvclock': ['scripts/pvclock-host.c','kernel/src/core/pvclock.c'],
    'clocksource': ['scripts/clocksource-host.c','kernel/src/core/pvclock.c'],
    'cpu-coverage': ['scripts/irq-latency-cpu-coverage-host.c'],
    'stack-coverage': ['scripts/irq-latency-stack-coverage-host.c'],
    'coverage-admission': ['scripts/irq-latency-coverage-admission-host.c'],
    'activation-report': ['scripts/irq-latency-activation-report-host.c'],
    'tlb-proof': ['scripts/tlb-proof-host.c','kernel/src/memory/tlb_proof.c'],
    'tlb-proof-emitter': ['scripts/tlb-proof-emitter-host.c','kernel/src/memory/tlb_proof.c'],
    'request-cycle-contract': ['scripts/request-telemetry-contract-host.c'],
    'details-enabled': ['scripts/irq-latency-detail-host.c','kernel/src/core/irq_latency_record.c'],
    'details-disabled': ['scripts/irq-latency-detail-host.c','kernel/src/core/irq_latency_record.c'],
    'heap-stages': ['scripts/irq-latency-heap-host.c','kernel/src/core/irq_latency_heap.c',
                    'kernel/src/core/irq_latency_interior.c','kernel/src/core/irq_latency_time.c'],
    'heap-integration': ['scripts/irq-latency-heap-integration-host.c'],
    'writeback-clock': ['scripts/irq-latency-writeback-clock-host.c', 'kernel/src/core/irq_latency_writeback_clock.c',
                       'kernel/src/core/irq_latency_interior.c', 'kernel/src/core/irq_latency_time.c'],
    'writeback': ['scripts/irq-latency-writeback-host.c', 'kernel/src/core/irq_latency_interior.c',
                  'kernel/src/core/irq_latency_time.c', 'kernel/src/core/irq_latency_arch.S'],
    'tlb-preparation': ['scripts/tlb-preparation-host.c'],
    'heap-preparation': ['scripts/heap-preparation-host.c'],
    'warning-report': ['scripts/warning-report-host.c'],
    'warning-emitter': ['scripts/warning-report-emitter-host.c'],
    'pmm-observation': ['scripts/pmm-observation-host.c','kernel/src/utils/bitmap.c'],
    'pmm-concurrent': ['scripts/pmm-observation-thread-host.c','kernel/src/utils/bitmap.c'],
    'stack-report': ['scripts/stack-report-host.c'],
    'stack-emitter': ['scripts/stack-report-emitter-host.c'],
    'bitmap-load': ['scripts/irq-latency-bitmap-load-host.c'],
    'interior': ['scripts/irq-latency-interior-host.c', 'kernel/src/core/irq_latency_interior.c', 'kernel/src/core/irq_latency_time.c'],
    'terminal': ['scripts/irq-latency-terminal-host.c'],
    'entry': ['scripts/irq-latency-entry-host.c', 'scripts/irq-latency-entry-host.S', 'kernel/src/core/interrupt_stubs.S'],
    'collector': ['scripts/irq-latency-collector-host.c', 'kernel/src/core/irq_latency_pairs.c',
                  'kernel/src/core/irq_latency_time.c'],
    'pairing': ['scripts/irq-latency-pairing-host.c', 'kernel/src/core/irq_latency_pairs.c',
                'kernel/src/core/irq_latency_time.c'],
    'controller': ['scripts/irq-latency-controller-host.c', 'kernel/src/core/irq_latency_record.c',
                   'kernel/src/core/switch.S',
                   'kernel/src/core/irq_latency_scope.c', 'kernel/src/core/irq_latency_component.c',
                   'kernel/src/core/irq_latency_time.c'],
    'time': ['scripts/irq-latency-time-host.c', 'kernel/src/core/irq_latency_time.c'],
    'record': ['scripts/irq-latency-record-host.c', 'kernel/src/core/irq_latency_record.c'],
    'component': ['scripts/irq-latency-component-host.c', 'kernel/src/core/irq_latency_component.c'],
    'scope': ['scripts/irq-latency-scope-host.c', 'kernel/src/core/irq_latency_scope.c'],
    'boundary': ['scripts/irq-latency-boundary-host.c', 'scripts/irq-latency-boundary-probe.S',
                 'kernel/src/core/irq_latency_arch.S', 'kernel/src/core/irq_latency_record.c'],
    'calibration': ['scripts/irq-latency-calibration-host.c', 'kernel/src/core/irq_latency_calibration.c',
                    'kernel/src/core/irq_latency_time.c'],
    'stress-admission': ['scripts/irq-latency-stress-host.c'],
}
SUBJECTS['interior-controller'] = [*SUBJECTS['controller'], 'kernel/src/core/irq_latency_interior.c']
SUBJECTS['heap-controller'] = ['scripts/irq-latency-heap-controller-host.c', *SUBJECTS['interior-controller'][1:],
                              'kernel/src/core/irq_latency_heap.c']
SUBJECTS['clock-body'] = ['scripts/irq-latency-clock-body-host.c', *SUBJECTS['interior-controller'][1:]]
SUBJECTS['tick-body'] = ['scripts/irq-latency-tick-host.c', *SUBJECTS['interior-controller'][1:]]
SUBJECTS['boundary-local-order'] = SUBJECTS['boundary']
SUBJECTS['calibration-local-order'] = SUBJECTS['calibration']
SUBJECTS['marker-attribution'] = ['scripts/irq-latency-marker-attribution-host.c', *SUBJECTS['heap-controller'][1:]]
SUBJECTS['heap-operation'] = ['scripts/irq-latency-heap-operation-host.c', *SUBJECTS['heap-controller'][1:]]
SUBJECTS['mandatory-controller'] = SUBJECTS['controller']
SUBJECTS['tlb-proof-controller'] = ['scripts/tlb-proof-controller-host.c', *SUBJECTS['controller'][1:],
    'kernel/src/memory/tlb_proof.c']
SUBJECTS['activation-envelope'] = ['scripts/irq-latency-activation-envelope-host.c', *SUBJECTS['controller'][1:]]
SUBJECTS['nested-controller'] = ['scripts/irq-latency-nested-host.c', *SUBJECTS['controller'][1:]]
SUBJECTS['precheck-assembly'] = ['scripts/irq-latency-precheck-assembly-host.c', *SUBJECTS['controller'][1:],
    'kernel/src/core/irq_latency_arch.S', 'scripts/irq-latency-boundary-probe.S']
SUBJECTS['mmio-identity'] = ['scripts/irq-latency-mmio-identity-host.c', *SUBJECTS['controller'][1:]]
SUBJECTS['writeback-clock-integration'] = ['scripts/irq-latency-writeback-clock-integration-host.c',
    *SUBJECTS['controller'][1:], 'kernel/src/core/irq_latency_interior.c', 'kernel/src/core/irq_latency_writeback_clock.c']
EXPECTED = {
    'spinlock-release': 'Ticket release actual x86: PASS FIFO=8 acquisitions=160000 publication=CHECKED wrap=CHECKED flags=EXACT\n',
    'pvclock': 'KVM pvclock actual ABI/arithmetic/snapshot: PASS bounded_retry=4 independent_vectors=CHECKED\n',
    'clocksource': 'KVM clocksource actual registration/clock controller: PASS cases=42 fallback=BOOT_ONLY lifetime=RETAINED\n',
    'cpu-coverage': 'IRQ coverage actual scheduler selection: PASS target_identity=CHECKED ordinary_FIFO=RETAINED\n',
    'stack-coverage': 'IRQ coverage actual lifecycle controller: PASS CPU1_to_32=HOSTED ownership_and_refusals=CHECKED\n',
    'coverage-admission': 'STACK_SCHEDULER_HOST status=PASS source_adapters=2 cases=26 capacity=16384 canary=512 privileged_boundaries=HOSTED\nIRQ coverage actual task admission: PASS offline_and_masked=REJECTED lifecycle=RETAINED\n',
    'activation-report': 'IRQ activation actual formatter: PASS bounded_record=CHECKED refusal=RETAINED\n',
    'tlb-proof': 'TLB proof actual-C bounded whole records and immutable ownership: PASS\n',
    'tlb-proof-emitter': 'TLB proof actual emitter/export: PASS capture_port_bytes=0 flags=EXACT export_IF=1\n',
    'request-cycle-contract': 'Request local-cycle typed identity/order contract: PASS functional_deadline=SEPARATE\n',
    'details-enabled': 'IRQ latency actual-C compulsory maxima independent of optional detail/backlog: PASS minimum_only=1\n',
    'details-disabled': 'IRQ latency actual-C compulsory maxima independent of optional detail/backlog: PASS minimum_only=1\n',
    'heap-stages': 'IRQ latency actual-C bounded heap stages and continuity: PASS minimum_only=1\n',
    'writeback-clock': 'IRQ latency actual-C writeback clock calls and bounded raw outcomes: PASS hardware=HOSTED minimum_only=1\n',
    'writeback': 'IRQ latency actual-C writeback continuity and assembly timestamps: PASS wbinvd=HOSTED_NOP minimum_only=1\n',
    'tlb-preparation': 'Paging actual-C private table preparation, recheck and publication: PASS\n',
    'heap-preparation': 'Heap actual-C private preparation, first-fit recheck and competing growth: PASS\n',
    'warning-report': 'Memory warning actual-C bounded publication and drain: PASS\n',
    'warning-emitter': 'Memory warning actual-C emitter preserves context and defers UART: PASS\n',
    'pmm-observation': 'PMM actual-C coherent incremental observation: PASS\n',
    'pmm-concurrent': 'PMM actual-C concurrent writers and complete observations: PASS\n',
    'stack-report': 'Stack proof actual-C bounded publication and drain: PASS\n',
    'stack-emitter': 'Stack proof actual-C emitter defers exact lifetime rows: PASS\n',
    'bitmap-load': 'IRQ latency actual-C full bitmap word/tail and high corruption: PASS\n',
    'interior': 'IRQ latency actual-C interior continuity and downward conversion: PASS diagnostic=MINIMUM_ONLY\n',
    'terminal': 'IRQ latency actual-C terminal audit and bounded refusal: PASS services=HOSTED\n',
    'entry': 'IRQ latency actual-assembly entry frame, GPR and DF: PASS iret=HOSTED\n',
    'collector': 'IRQ latency actual-C completed collector and reports: PASS clocks=HOSTED\n',
    'pairing': 'IRQ latency actual-C deterministic CPU-local pairs: PASS clocks=HOSTED\n',
    'controller': 'IRQ latency actual-C boundary controller and identity lifetime: PASS hardware=HOSTED\n',
    'time': 'IRQ latency actual-C arithmetic and calibration decisions: PASS\n',
    'record': 'IRQ latency actual-C nesting, maxima and bounded publication: PASS\n',
    'component': 'IRQ latency actual-C callback and request maximum witnesses: PASS\n',
    'scope': 'IRQ latency actual-C stack-owned scope attribution: PASS\n',
    'boundary': 'IRQ latency actual-assembly flags and ordered envelopes: PASS privileged_IF=HOSTED\n',
    'calibration': 'IRQ latency actual-C capability and HPET acquisition: PASS clocks=HOSTED\n',
    'stress-admission': 'IRQ latency actual-C bounded stress admission and quarantine: PASS services=HOSTED\n',
}
EXPECTED['interior-controller'] = EXPECTED['controller']
EXPECTED['mandatory-controller'] = EXPECTED['controller']
EXPECTED['tlb-proof-controller'] = 'TLB actual emitter/controller: PASS capture_wait_copy_in_outer_maximum=1 exact_flags=1\n'
EXPECTED['activation-envelope'] = 'EARLY_WITNESS [110,300,1,1,0,18446744071562072064,32,3,0,0,1,0,0,0,40,110,0,300,0,0,1,0,0,4294967295,0,40,0,0,0]\n'
EXPECTED['heap-controller'] = ('IRQ latency actual-C heap controller identity and ownership: PASS minimum_only=1\n'
    + EXPECTED['controller'])
EXPECTED['heap-integration'] = EXPECTED['heap-preparation'] + 'Heap actual-C operation checkpoints and node counts: PASS clocks=HOSTED\n'
EXPECTED['nested-controller'] = ('IRQ latency actual-C nested no-op equivalence and required publication: PASS hardware=HOSTED\n'
    + EXPECTED['controller'])
EXPECTED['mmio-identity'] = ('IRQ latency actual-C MMIO same-mask identity and fallback: PASS hardware=HOSTED\n'
    + EXPECTED['controller'])
EXPECTED['writeback-clock-integration'] = (
    'IRQ latency actual-C HPET/MMIO nesting: PASS legacy_calls=2 legacy_mmio_reads=6 pending_delta=12 empty_delta=0 attributed=CHECKED minimum_only=1 hardware=HOSTED\n'
    + EXPECTED['controller'])
EXPECTED['marker-attribution'] = 'ACTUAL_MARKER_ATTRIBUTION baseline=10 observer_tail_injected=500010 delta=500000 metadata_instructions=0\n'
EXPECTED['precheck-assembly'] = 'IRQ latency actual controller/assembly nested precheck: PASS flags=EXACT outer=RETAINED privileged_IF=HOSTED\n'
EXPECTED['heap-operation'] = 'Heap actual call-site and controller: PASS observer_tail_excluded=500000 parent_retained=1 minimum_only=1\n'
EXPECTED['clock-body'] = 'IRQ latency actual clock/controller: PASS stalled=RETAINED regression=CLASSIFIED mask=EXACT minimum_only=1\n'
EXPECTED['tick-body'] = 'LAPIC actual handler/controller: PASS phases=SAME_EVENT continuity=CHECKED EOI=ONCE minimum_only=1\n'
EXPECTED['boundary-local-order'] = EXPECTED['boundary']
EXPECTED['calibration-local-order'] = EXPECTED['calibration']
MODES = {
    'normal': [],
    'ubsan': ['-fsanitize=undefined', '-fno-sanitize-recover=all'],
    'asan': ['-fsanitize=address', '-fno-omit-frame-pointer'],
}


def subject_flags(subject):
    if subject=='spinlock-release':
        return ['-pthread','-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=0']
    if subject in ('pvclock','clocksource'):
        return ['-DIRQ_LATENCY_HOSTED_CLOCK=1','-DHOBBYOS_IRQ_LATENCY=1','-DHOBBYOS_DEBUG_ASSERT=1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    if subject in ('cpu-coverage','stack-coverage','coverage-admission'):
        return ['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=1',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    if subject=='tlb-proof-controller':
        return [*subject_flags('controller'),'-DHOBBYOS_SELFTEST=1','-DHOBBYOS_DEBUG_ASSERT=0']
    if subject=='activation-report':
        return ['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_IRQ_LATENCY=1','-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1',
                '-DHOBBYOS_IRQ_LOCAL_TSC_ORDER=1','-DHOBBYOS_IRQ_LATENCY_DETAILS=0',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    if subject in ('tlb-proof','tlb-proof-emitter'):
        return ['-DHOBBYOS_SELFTEST=1','-DHOBBYOS_DEBUG_ASSERT=0','-DHOBBYOS_IRQ_LATENCY=0',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread']
    if subject.endswith('-local-order'):
        return [*subject_flags(subject.removesuffix('-local-order')),'-DHOBBYOS_IRQ_LOCAL_TSC_ORDER=1']
    if subject=='request-cycle-contract':return ['-DHOBBYOS_SELFTEST=1']
    if subject == 'precheck-assembly':
        return [*subject_flags('controller'), '-DIRQ_LATENCY_HOSTED_BOUNDARY=1']
    if subject in ('details-enabled','details-disabled'):
        return ['-DHOBBYOS_IRQ_LATENCY_DETAILS='+str(int(subject=='details-enabled'))]
    if subject == 'mandatory-controller':
        return [*subject_flags('controller'),'-DHOBBYOS_IRQ_LATENCY_DETAILS=0']
    if subject == 'heap-operation':
        return [*subject_flags('heap-controller'),'-DHOBBYOS_SELFTEST=1','-DHOBBYOS_DEBUG_ASSERT=0']
    if subject == 'marker-attribution':
        return subject_flags('heap-controller')
    if subject == 'heap-integration':
        return [*subject_flags('heap-preparation'),'-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1','-DIRQ_LATENCY_HOSTED_CLOCK=1']
    if subject == 'mmio-identity':
        return [*subject_flags('controller'), '-DHOBBYOS_ARCH_HOST_TEST=1', '-DHOBBYOS_DEBUG_ASSERT=1']
    if subject == 'writeback-clock-integration':
        return [*subject_flags('interior-controller'), '-DHOBBYOS_ARCH_HOST_TEST=1',
                '-DHOBBYOS_DEBUG_ASSERT=1', '-DIRQ_LATENCY_HOSTED_WRITEBACK_CLOCK=1']
    if subject == 'writeback-clock':
        return ['-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1', '-DIRQ_LATENCY_HOSTED_CLOCK=1', '-DIRQ_LATENCY_HOSTED_WRITEBACK_CLOCK=1']
    if subject == 'writeback':
        return ['-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1','-DIRQ_LATENCY_HOSTED_WRITEBACK=1',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    if subject in ('heap-preparation', 'tlb-preparation'):
        return ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread',
                '-DHOBBYOS_SELFTEST=1','-DHOBBYOS_DEBUG_ASSERT=0','-DHOBBYOS_IRQ_LATENCY=0']
    if subject == 'warning-emitter':
        return ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DHOBBYOS_DEBUG_ASSERT=1','-DHOBBYOS_IRQ_LATENCY=1']
    if subject in ('warning-report','pmm-observation','pmm-concurrent','stack-report','stack-emitter'):
        flags=['-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread']
        return flags+(['-DHOBBYOS_DEBUG_ASSERT=0'] if subject.startswith('pmm-') else ['-DHOBBYOS_IRQ_LATENCY=1','-DHOBBYOS_SELFTEST=1'])
    if subject == 'entry':
        return ['-DHOBBYOS_IRQ_LATENCY=1', '-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1', '-DHOBBYOS_DEBUG_ASSERT=1', '-DIRQ_ENTRY_HOSTED=1']
    if subject in ('interior-controller', 'heap-controller', 'clock-body', 'tick-body'):
        return [*subject_flags('controller'), '-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1']
    if subject in ('controller', 'nested-controller', 'activation-envelope'):
        return ['-DHOBBYOS_IRQ_LATENCY=1', '-DIRQ_LATENCY_HOSTED_CONTROLLER=1',
                '-DIRQ_LATENCY_HOSTED_CLOCK=1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
    if subject == 'boundary':
        return ['-DIRQ_LATENCY_HOSTED_BOUNDARY=1']
    if subject == 'calibration':
        return ['-DIRQ_LATENCY_HOSTED_CLOCK=1', '-DHOBBYOS_IRQ_LATENCY=1']
    if subject in ('stress-admission', 'collector', 'terminal'):
        return ['-DHOBBYOS_SELFTEST=1', '-DHOBBYOS_IRQ_LATENCY=1',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
    return []


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'),
                                                 E.ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def timestamp_codegen(output):
    output.mkdir()
    probe=output/'timestamp.c'
    probe.write_text('#include "core/irq_latency_tsc.h"\n'
        'uint64_t irq_timestamp_codegen_probe(volatile uint64_t *p) {\n'
        '  *p=1; uint64_t t=irq_latency_read_tsc(); *p=2; return t;\n}\n')
    gate=module('memory-observation-gate');rows=[]
    sources=[str(probe),'kernel/src/core/irq_latency_arch.S','kernel/src/core/interrupt_stubs.S']
    for order in (0,1):
        for index,source in enumerate(sources):
            name=f'order-{order}-unit-{index}';obj=output/(name+'.o')
            argv=['gcc','-O2','-ffreestanding','-mno-red-zone','-mgeneral-regs-only',
                '-DHOBBYOS_IRQ_LATENCY=1','-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1',
                '-DHOBBYOS_IRQ_LOCAL_TSC_ORDER='+str(order),'-Ikernel/src','-c',source,'-o',str(obj)]
            compiled=gate.run(output,name+'-compile',argv)
            emitted=gate.run(output,name+'-disassembly',['objdump','-d','--no-show-raw-insn',str(obj)])
            rows.append(dict(order=order,unit=index,compile=compiled.relative_to(output).as_posix(),
                disassembly=emitted.relative_to(output).as_posix(),
                object_sha256=E.sha(obj)))
    E.write(output/'result.json',dict(rows=rows,probe_sha256=E.sha(probe)))
    return verify_timestamp_codegen(output)


def verify_timestamp_codegen(output):
    gate=module('cpu-control-state-gate');receipt=json.loads((output/'result.json').read_text())
    E.require(receipt['probe_sha256']==E.sha(output/'timestamp.c'),'timestamp codegen probe identity')
    sequences={};counts={}
    for row in receipt['rows']:
        order,unit=row['order'],row['unit'];key=(order,unit)
        E.require(key not in sequences and order in (0,1) and unit in (0,1,2),'timestamp object inventory')
        compiled,_=gate.verify_command(output,row['compile'])
        emitted,stdout=gate.verify_command(output,row['disassembly'])
        source=str(output/'timestamp.c') if unit==0 else ('kernel/src/core/irq_latency_arch.S' if unit==1 else 'kernel/src/core/interrupt_stubs.S')
        expected=['gcc','-O2','-ffreestanding','-mno-red-zone','-mgeneral-regs-only',
            '-DHOBBYOS_IRQ_LATENCY=1','-DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=1',
            '-DHOBBYOS_IRQ_LOCAL_TSC_ORDER='+str(order),'-Ikernel/src','-c',source,'-o',compiled['argv'][-1]]
        E.require(compiled['argv']==expected and emitted['argv']==['objdump','-d','--no-show-raw-insn',compiled['argv'][-1]],
                  'timestamp exact compile/disassembly recipe')
        instructions=[m[1] for line in stdout.splitlines() if (m:=re.match(r'^\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)',line))]
        stamps=[i for i,v in enumerate(instructions) if v=='rdtsc']
        E.require(stamps and all(i>0 and instructions[i-1]=='lfence' and instructions[i+1]=='lfence' for i in stamps),
                  'timestamp lost local ordering')
        E.require(instructions.count('mfence')==(len(stamps) if order==0 else 0),
                  'timestamp global-store drain differs from declared primitive')
        if not order:E.require(all(i>1 and instructions[i-2]=='mfence' for i in stamps),'original timestamp sequence')
        sequences[key]=[v for v in instructions if v not in ('mfence','nop','nopl','nopw','data16')]
        counts[str(key)]=len(stamps)
        if unit==0:
            E.require(re.search(r'movq?\s+\$0x1,\(%rdi\)',stdout) and re.search(r'movq?\s+\$0x2,\(%rdi\)',stdout) and
                stdout.index('$0x1')<stdout.index('rdtsc')<stdout.index('$0x2'),'compiler moved the call-site stores')
    E.require(set(sequences)=={(o,u) for o in (0,1) for u in range(3)},'timestamp object coverage')
    for unit in range(3):
        E.require(sequences[(0,unit)]==sequences[(1,unit)],'timestamp contrast changed an instruction beyond the declared fence')
    return dict(status='PASS',ordered_timestamp_sites=counts,execution_authority='Emitted code only')


def tick_handler_source():
    source=(E.ROOT/'kernel/src/core/interrupts.c').read_text()
    first=source.index('uint64_t irq_lapic_timer_handler_inner(void)')
    last=source.index('\nstatic void irq_xhci_handler_inner',first)
    body=source[first:last]
    E.require(body.count('__asm__ volatile("cli; hlt");')==1,'LAPIC hosted privileged seam')
    return body.replace('__asm__ volatile("cli; hlt");','CHECK(false);')


def tick_flags(output, subject):
    if subject in ('cpu-coverage','coverage-admission'): return ['-I'+str(output)]
    if subject=='terminal': return ['-DIRQ_FINISH_SOURCE="'+str(output/'irq-finish.host.c')+'"']
    if subject!='tick-body': return []
    return ['-DIRQ_TICK_HANDLER_SOURCE="'+str(output/'lapic-handler.host.c')+'"']


def finish_source():
    source=(E.ROOT/'kernel/src/memory/irq_latency_test.c').read_text()
    body=source[source.index('int irq_latency_test_finish(void)'):source.index('int irq_latency_test_audit(void)')]
    for name in ('irq_latency_test_finish','calibration_collect','calibration_select','calibration_report','terminal_freeze','terminal_report'):
        E.require(body.count(name+'(')==1,'finish hosted call seam: '+name)
        body=body.replace(name+'(','host_'+name+'(')
    return body


def scheduler_selector_source():
    import stack_current
    return stack_current.functions((E.ROOT/'kernel/src/core/scheduler.c').read_text())['runqueue_take_next_locked']+'\n'


def run(output):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    gate = module('memory-observation-gate')
    (output/'lapic-handler.host.c').write_text(tick_handler_source())
    (output/'irq-finish.host.c').write_text(finish_source())
    (output/'scheduler-selector.host.c').write_text(scheduler_selector_source())
    (output/'stack-scheduler-extracted.h').write_text(module('stack-guard-host').scheduler_source()[0])
    rows = []
    for mode, extra in MODES.items():
        for subject, sources in SUBJECTS.items():
            binary = output / (subject + '-' + mode)
            argv = ['gcc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fno-pie', '-no-pie', *extra,
                    *subject_flags(subject), *tick_flags(output,subject),
                    *sources, '-o', str(binary)]
            compiled = gate.run(output, binary.name + '-compile', argv)
            executed = gate.run(output, binary.name, [str(binary)],
                                {'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
                                 'UBSAN_OPTIONS': 'halt_on_error=1'})
            summary = (executed / 'stdout.log').read_text()
            E.require(summary == EXPECTED[subject], 'latency hosted semantic result: ' + subject)
            rows.append({'subject': subject, 'mode': mode,
                         'compile': compiled.relative_to(output).as_posix(),
                         'execute': executed.relative_to(output).as_posix(),
                         'summary': summary, 'binary_sha256': E.sha(binary),
                         'stdout_sha256': E.sha(executed / 'stdout.log')})
    module('request-telemetry-host').run(output/'request-service')
    module('request-telemetry-host').run(output/'request-clock',actual_clock=True)
    module('request-telemetry-host').run(output/'request-pvclock',actual_pvclock=True)
    module('request-telemetry-host').run(output/'request-service-local-order',local_order=True)
    module('request-telemetry-fixtures').run(output/'request-fixtures')
    module('irq-latency-stack-coverage-fixtures').run(output/'stack-coverage-fixtures')
    module('clocksource-fixtures').run(output/'clocksource-normal',output/'clocksource-fixtures')
    timestamp_codegen(output/'timestamp-codegen')
    result = {'schema': 1, 'status': 'PASS', 'build_input_sha256': E.identity(E.manifest()),
              'harness_sha256': E.identity(E.manifest(harness=True)),
              'contract_sha256': E.sha(E.ROOT / 'scripts/irq-latency-contract.json'),
              'executions': rows, 'authority': 'Actual C decisions; injected scalar endpoints. No runtime IRQ latency claim.'}
    E.write(output / 'irq-latency-host.json', result)
    verify(output)
    return result


def verify(directory):
    gate = module('cpu-control-state-gate')
    record = json.loads((directory / 'irq-latency-host.json').read_text())
    E.require(record['schema'] == 1 and record['status'] == 'PASS' and
              record['build_input_sha256'] == E.identity(E.manifest()) and
              record['harness_sha256'] == E.identity(E.manifest(harness=True)) and
              record['contract_sha256'] == E.sha(E.ROOT / 'scripts/irq-latency-contract.json'),
              'latency hosted source/harness/contract binding')
    E.require((directory/'lapic-handler.host.c').read_text()==tick_handler_source(),'actual LAPIC function source differs')
    E.require((directory/'irq-finish.host.c').read_text()==finish_source(),'actual finish function source differs')
    E.require((directory/'scheduler-selector.host.c').read_text()==scheduler_selector_source(),'actual scheduler selection source differs')
    E.require((directory/'stack-scheduler-extracted.h').read_text()==module('stack-guard-host').scheduler_source()[0],
              'actual targeted task admission source differs')
    coverage=json.loads((directory/'stack-coverage-fixtures/result.json').read_text())
    E.require(coverage['status']=='PASS' and len(coverage['cases'])==26 and
        sum(c['result']=='REJECT' for c in coverage['cases'])==25,'coverage verifier controls incomplete')
    seen = set()
    for row in record['executions']:
        subject, mode = row['subject'], row['mode']
        E.require(subject in SUBJECTS and mode in MODES and (subject, mode) not in seen,
                  'latency hosted unknown or repeated execution')
        seen.add((subject, mode))
        compiled, _ = gate.verify_command(directory, row['compile'])
        executed, stdout = gate.verify_command(directory, row['execute'])
        argv = compiled['argv']
        expected = ['gcc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fno-pie', '-no-pie', *MODES[mode],
                    *subject_flags(subject), *tick_flags(directory,subject),
                    *SUBJECTS[subject], '-o', argv[-1]]
        E.require(argv == expected and executed['argv'] == [argv[-1]] and
                  compiled['build_input_sha256'] == executed['build_input_sha256'] == record['build_input_sha256'] and
                  stdout == row['summary'] == EXPECTED[subject] and
                  E.sha(directory / row['execute'] / 'stdout.log') == row['stdout_sha256'],
                  'latency actual compile/execution evidence differs')
    E.require(seen == {(s, m) for s in SUBJECTS for m in MODES},
              'latency actual hosted implementation coverage missing')
    module('request-telemetry-host').verify(directory/'request-service')
    module('request-telemetry-host').verify(directory/'request-clock',actual_clock=True)
    module('request-telemetry-host').verify(directory/'request-pvclock',actual_pvclock=True)
    module('request-telemetry-host').verify(directory/'request-service-local-order',local_order=True)
    module('request-telemetry-fixtures').verify(directory/'request-fixtures')
    verify_timestamp_codegen(directory/'timestamp-codegen')
    module('clocksource-fixtures').verify(directory/'clocksource-fixtures')
    return {'status': 'PASS', 'executions': len(seen)+12, 'runtime_measurement': 'NOT_HOSTED'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    print(E.canonical(verify(args.output) if args.verify else run(args.output)).decode())
