#!/usr/bin/env python3
"""Run real USB ownership, discovery admission and exact probe decision tests."""
import argparse
import importlib.util
import re
from pathlib import Path
import sys
import memory_evidence as E
import kernel_image_current as WX
ROOT=Path(__file__).resolve().parent.parent
SUMMARIES={
    'usb-ownership':'USB_OWNERSHIP_HOST checks=1758 status=PASS\n',
    'usb-hotplug':'USB_HOTPLUG_HOST checks=382 status=PASS\n',
    'hub-buffer':'USB_HUB_BUFFER_HOST wire_sizes=9,12 bounded_output=PASS\n',
    'hub-ownership':'USB_HUB_OWNERSHIP_HOST checks=63 status=PASS\n',
    'probe':'MAPPING_PROBE_HOST exact_site_and_fault_boundaries=PASS\n',
    'origin':'DEVICE_ORIGIN_HOST checks=97 status=PASS\n'}

if WX.enabled():SUMMARIES['origin']='DEVICE_ORIGIN_HOST checks=100 status=PASS\n'

def verify_summary(subject,output):
    if subject=='usb-control':
        # The two real callers can observe a different number of busy polls;
        # terminal events, wraps and retirement cases are fixed independently.
        match=re.fullmatch(r'USB_CONTROL_HOST checks=(\d+) terminal_events=302 concurrent_callers=2 contention_observed=1 wraps=2 retained_timeout=1 cpu_bus_separate=1 status=PASS\n',output)
        E.require(match and int(match[1])>=6355,'actual concurrent USB C coverage differs: '+repr(output[:512]))
    else:E.require(output==SUMMARIES[subject],'actual USB/ownership C coverage differs: '+repr(output[:512]))

def summary_cases():
    positive='USB_CONTROL_HOST checks=6355 terminal_events=302 concurrent_callers=2 contention_observed=1 wraps=2 retained_timeout=1 cpu_bus_separate=1 status=PASS\n'
    verify_summary('usb-control',positive)
    cases=[]
    for before,after in [('checks=6355','checks=6354'),('terminal_events=302','terminal_events=301'),
                         ('concurrent_callers=2','concurrent_callers=1'),('contention_observed=1','contention_observed=0'),
                         ('contention_observed=1 ',''),('wraps=2','wraps=1'),
                         ('retained_timeout=1','retained_timeout=0'),('cpu_bus_separate=1','cpu_bus_separate=0'),
                         ('status=PASS\n','status=PASS\nextra\n')]:
        try:verify_summary('usb-control',positive.replace(before,after))
        except ValueError as error:cases.append({'before':before,'after':after,'reason':str(error)})
        else:raise ValueError('USB hosted summary mutation accepted: '+before)
    verify_summary('usb-control',positive)
    return {'status':'PASS','positive_controls':2,'rejections':cases}

def run(output):
    spec=importlib.util.spec_from_file_location('gate',ROOT/'scripts/memory-observation-gate.py')
    gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
    output.mkdir(parents=True,exist_ok=False)
    subjects={
        'usb-control':(['scripts/usb-control-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c',
            'kernel/src/drivers/usb/xhci/xhci_dma.c','kernel/src/memory/dma_buffer.c','kernel/src/memory/mapping_core.c'],['-pthread']),
        'usb-ownership':(['scripts/usb-ownership-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c'],[]),
        'usb-hotplug':(['scripts/usb-hotplug-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c'],[]),
        'hub-buffer':(['scripts/usb-hub-buffer-host.c','kernel/src/drivers/usb/xhci/usb_hub.c'],[]),
        'hub-ownership':(['scripts/usb-hub-ownership-host.c','kernel/src/drivers/usb/xhci/usb_ownership.c'],[]),
        'probe':(['scripts/mapping-probe-host.c','kernel/src/memory/mapping_probe_match.c'],[]),
        'origin':(['scripts/device-origin-host.c','kernel/src/memory/physical_contract.c','kernel/src/memory/ram_map_plan.c','kernel/src/memory/mapping_core.c',
                   'kernel/src/utils/bitmap.c','kernel/src/cpu/control_policy.c','kernel/src/memory/cache_attributes.c'],
                  ['-DHOBBYOS_CONTROL_POLICY_HOST_TEST'])}
    decisions=summary_cases();results=[]
    for mode,flags in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        for name,(sources,extra) in subjects.items():
            binary=output/(name+'-'+mode)
            build=gate.run(output,binary.name+'-build',['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
                '-fno-builtin','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                *flags,*extra,*sources,'-o',str(binary)])
            result=gate.run(output,binary.name,[str(binary)],{'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            summary=(result/'stdout.log').read_text();verify_summary(name,summary)
            results.append({'subject':name,'mode':mode,'binary':E.sha(binary),'summary':summary,'build':str(build.relative_to(output)),
                            'execution':str(result.relative_to(output))})
    receipt={'schema':1,'status':'PASS','authority':'hosted actual C; no guest runtime authority',
             'inputs':E.identity(E.manifest()),'harness':E.identity(E.manifest(harness=True)),
             'summary_cases':decisions,'results':results}
    E.write(output/'ownership-host.json',receipt);return receipt
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    print(E.canonical(run(args.output.resolve())).decode())
