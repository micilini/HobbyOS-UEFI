#!/usr/bin/env python3
"""Exact successor of inline PCI ordering for the owned-mapping worker."""
import argparse
import os
from pathlib import Path
import re
import memory_evidence as E
from tlb_current import functions

STACK_SEAL='if (!kernel_stack_close_bootstrap()) kpanic("Stack preparation is not sealed before AP startup");'
STACK_RUNTIME='if (!kernel_stack_runtime_enable()) kpanic("Stack runtime admission precedes CPU IRQ readiness");'

def guarded_stacks():
    return os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks'


def compact(text):
    return re.sub(r'\s+', '', re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S))


def validate(source):
    bodies = functions(source)
    worker = '''static void initialize_runtime_devices(void *unused) {
        (void)unused;
        task_handle_t owner;
        if (!scheduler_current_task_handle(&owner)) kpanic("Device initialization owner unavailable");
        char line[128];
        int length=ksnprintf(line,sizeof(line),"\\n[BOOT][DEVICE_OWNER] id=%llu generation=%llu\\n",
            (unsigned long long)owner.id,(unsigned long long)owner.lifecycle_generation);
        if (length<0 || (size_t)length>=sizeof(line)) kpanic("Device initialization owner format");
        serial_write_all(line);
        console_begin_batch(); pci_init(); console_end_batch();
        runtime_ready_mark_pci_scan_complete();
        __atomic_store_n(&g_device_initialization_complete, 1, __ATOMIC_RELEASE);
    }'''
    E.require(compact(bodies.get('initialize_runtime_devices', '')) == compact(worker),
              'device worker ordering/body')
    E.require(source.count('pci_init();') == 1, 'PCI initialization outside owned worker')
    boot = bodies['init_system_core']
    start = boot.index('if (!irq_bootstrap_wait_all_runtime_ready(10000000u))')
    tail = '''if (!irq_bootstrap_wait_all_runtime_ready(10000000u))
        kpanic("CPU interrupt runtime readiness failed");
    serial_write_all("[BOOTTRACE][BSP] 5 before-pci-init\\n");
    if (!cpu_request_mapping_enable()) kpanic("Device mapping admission precedes CPU IRQ readiness");
    kinit_progress("PCI_BEGIN");
    if (!thread_create_named_with_class_flags(initialize_runtime_devices, NULL,
        TASK_CLASS_NORMAL, "device-initialization", TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED))
        kpanic("Device initialization worker creation failed");
    uint64_t device_deadline = clock_monotonic_ns() + UINT64_C(120000000000);
    while (!__atomic_load_n(&g_device_initialization_complete, __ATOMIC_ACQUIRE)) {
        if (clock_monotonic_ns() >= device_deadline) kpanic("Device initialization worker deadline");
        __asm__ volatile("hlt" ::: "memory");
    }
    kinit_progress("PCI_SCAN_COMPLETE");
    kinit_progress("CORE_COMPLETE");
    kinit_debug("[CORE] System Core Initialization Complete.\\n");
    }'''
    if guarded_stacks():
        mapping='if (!cpu_request_mapping_enable()) kpanic("Device mapping admission precedes CPU IRQ readiness");'
        tail=tail.replace(mapping,mapping+'\n'+STACK_RUNTIME)
        E.require(boot.count(STACK_SEAL)==boot.count(STACK_RUNTIME)==1 and
            boot.index('owned_mapping_close_bootstrap();')<boot.index(STACK_SEAL)<
            boot.index('kernel_tlb_close_bootstrap();')<boot.index('smp_boot_aps();')<
            boot.index('kernel_tlb_runtime_init()')<boot.index('irq_enable();')<
            boot.index(mapping)<boot.index(STACK_RUNTIME),
            'stack early preparation/seal/runtime order')
    E.require(compact(boot[start:]) == compact(tail), 'device worker admission/completion ordering')
    E.require(boot.index('irq_enable();') < start, 'device worker before IF release')
    return {'status': 'PASS', 'contract': 'owned-device-worker-order',
            'base_commit': 'ff6eaf609611b3d4a401e739f1f45dd7451346f3',
            'retained': 'PCI after all CPU IRQ readiness; console batching; completion before core release'}


def fixtures(source):
    validate(source)
    cases = [
        ('PCI-before-console', source.replace('console_begin_batch();\n    pci_init();', 'pci_init();\n    console_begin_batch();')),
        ('missing-readiness', source.replace('if (!irq_bootstrap_wait_all_runtime_ready(10000000u))', 'if (0)')),
        ('completion-before-PCI', source.replace('    pci_init();', '    runtime_ready_mark_pci_scan_complete();\n    pci_init();')),
        ('IF-disabled-wait', source.replace('    uint64_t device_deadline =', '    irq_disable();\n    uint64_t device_deadline =')),
        ('missing-acquire', source.replace('__ATOMIC_ACQUIRE)) {', '__ATOMIC_RELAXED)) {')),
        ('duplicate-inline-PCI', source.replace('    kinit_progress("CORE_COMPLETE");', '    pci_init();\n    kinit_progress("CORE_COMPLETE");'))]
    if guarded_stacks():
        cases += [
            ('stack-missing-seal',source.replace(STACK_SEAL,'')),
            ('stack-seal-after-AP-launch',source.replace(STACK_SEAL,'').replace('smp_boot_aps();','smp_boot_aps();\n'+STACK_SEAL)),
            ('stack-runtime-before-IF',source.replace(STACK_RUNTIME,'').replace('irq_enable();',STACK_RUNTIME+'\nirq_enable();')),
            ('stack-worker-before-seal',source.replace('kernel_tlb_runtime_init()','0').replace(STACK_SEAL,'kernel_tlb_runtime_init();\n'+STACK_SEAL)),
            ('stack-runtime-not-enabled',source.replace(STACK_RUNTIME,''))]
    outcomes = []
    for name, changed in cases:
        E.require(changed != source, 'fixture did not mutate: ' + name)
        try:
            validate(changed)
        except (ValueError, KeyError) as error:
            outcomes.append({'case': name, 'synthetic': True, 'result': 'REJECTED', 'reason': str(error)})
        else:
            raise ValueError('ordering mutation accepted: ' + name)
    return {'positive': validate(source), 'negative': outcomes}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=E.ROOT / 'kernel/src/core/kernel_init.c')
    parser.add_argument('--fixtures', action='store_true')
    args = parser.parse_args()
    print(E.canonical((fixtures if args.fixtures else validate)(args.source.read_text())).decode(), end='')
