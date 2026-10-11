#!/usr/bin/env python3
"""Reproducible semantic controls and mutations of the actual compiled policy."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import struct
import memory_evidence as E
import cpu_current
if os.environ.get("HOBBYOS_VALIDATION_CONTRACT") in ("tlb-coherency","physical-translation","owned-device-mapping",'verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
    import tlb_current as cpu_current

ROOT=Path(__file__).resolve().parent.parent
REQUIRED_NEGATIVES = {
    'historical-complete-is-not-normalized','ap-wp-clear','ap-cd-set','ap-nw-set','ap-ne-clear',
    'ap-cr0-residue','ap-cr4-different','supported-nx-clear','wrong-common-pat','framebuffer-slot-four-changed',
    'prohibited-cr4-everywhere','nxe-with-nx-absent','roles-swapped','stale-generation','duplicate-readiness',
    'missing-readiness','online-before-validation','ordinary-model-relabeled-nx-off',
    'production-initializer-compiled-out','production-observer-linked',
    'desired-ready-constant-disagrees-with-raw-debugger','unused-slot-claim-contradicted-by-decoded-leaf',
    'unrelated-protected-edit','wrong-ap-handoff','lost-exception-recovery','unauthorized-instruction-site',
    'missing-first-cache-flush','pat-before-validation','missing-final-tlb-flush','wrong-final-cache-mode',
    'wb-zero-treated-as-failure','large-leaf-pat-confused-with-ps',
    'missing-trampoline-header-dependency',
    'portable-log-path-misdirected','portable-control-value-changed',
    'harness-reverification-linked-input-drift',
    'native-summary-nonzero-exit','native-summary-missing-family','native-summary-wrong-run-count',
    'native-summary-trailing-garbage','native-summary-reported-failure',
}


def verify_receipt(value):
    cases=value['cases'];names=[r['name'] for r in cases]
    E.require(len(names)==len(set(names)) and set(names)==REQUIRED_NEGATIVES,'control negative requirement omitted/duplicated')
    E.require(value['positive_controls']==['new-complete-and-normalized','previous-complete-nonuniform',
              'unchanged-protected-scope','exact-instruction-sites','portable-log-relocation',
              'harness-reverification-preserves-receipts','native-output-final-summary','previous-full-observation-replay'],
              'control positive baseline replay omitted')
    for case in cases:
        E.require(case['synthetic'] is True and case['expected_exit']==case['actual_exit']==1 and
                  (case['expected_boundary'] in case['actual_reason'] or
                   case['expected_boundary']=='actual C independent oracle' and 'check failed at line' in case['actual_reason']),
                  'control fixture rejected at unrelated boundary')


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    v=importlib.util.module_from_spec(spec);sys.modules[spec.name]=v;spec.loader.exec_module(v);return v


V=module('verify-cpu-control-state');O=V.observation()


def first_collection(directory):
    lines=(directory/'qemu-serial.log').read_text().splitlines();topology={}
    for line in lines:
        m=re.fullmatch(r'\[SMP\]\[CPU\] ONLINE slot=(\d+) apic=(\d+) role=(BSP|AP)',line)
        if m:topology[int(m[1])]=(int(m[2]),m[3])
    row=next(json.loads(l) for l in (directory/'framed.jsonl').read_text().splitlines() if json.loads(l)['payload']=='cpustate')
    return O.FOUNDATION.transaction_segment(lines,row),topology


def mutate_collection(lines, changes):
    """Synthetic semantic input: recompute uniformity to reach the policy checker."""
    result=list(lines)
    for index,line in enumerate(result):
        if line.startswith('[CPU_CONTROL][CPU] '):
            fields=dict(item.split('=',1) for item in line.split()[1:]);slot=int(fields['slot'])
            for key,value in changes.get(slot,{}).items():fields[key]=str(value)
            result[index]='[CPU_CONTROL][CPU] '+' '.join(k+'='+v for k,v in fields.items())
    cpus=O.records(result,'CPU_CONTROL','CPU')
    for index,line in enumerate(result):
        if not line.startswith('[CPU_CONTROL][SUMMARY] '):continue
        fields=dict(item.split('=',1) for item in line.split()[1:]);uniform=[]
        for key,label in [('cr0','cr0'),('cr3','cr3_raw'),('cr4','cr4'),('efer','efer'),('pat','pat')]:
            same=len({V.number(c,key) for c in cpus})==1;fields['uniform_'+label]=str(int(same));uniform.append(same)
        fields['uniform_cr3_root']=str(int(len({V.number(c,'cr3')&O.ADDRESS for c in cpus})==1))
        fields['controls_uniform']='true' if all(uniform) and fields['uniform_cr3_root']=='1' else 'false'
        result[index]='[CPU_CONTROL][SUMMARY] '+' '.join(k+'='+v for k,v in fields.items())
    return result


def run(evidence,output,baseline_source=None,compiled=True):
    output.mkdir(parents=True,exist_ok=False);cases=[]
    def reject(name,boundary,operation,recipe=None):
        try:operation()
        except Exception as error:
            E.require(boundary in str(error),'wrong control fixture boundary: '+name+': '+str(error))
            cases.append({'name':name,'synthetic':True,'expected_exit':1,'actual_exit':1,
                          'expected_boundary':boundary,'actual_reason':str(error),'recipe':recipe})
        else:raise ValueError('control negative accepted: '+name)
    current=evidence/'profiles/q35-kvm-smp4-1';old=evidence/'previous-observation'
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('tlb-coherency','physical-translation','owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks') and not current.is_dir():
        eligible=sorted(evidence.glob('diagnostics/q35-kvm-smp4-readback-*/result.json'))
        E.require(eligible,'current diagnostic capture required for control fixtures')
        current=eligible[-1].parent
    base,topology=first_collection(current);previous,old_topology=first_collection(old)
    V.verify_run(current)
    E.require(O.verify_cpu(previous,old_topology)[1]['controls_uniform']=='false','previous baseline unexpectedly normalized')
    V.policy_observation(base,topology)
    reject('historical-complete-is-not-normalized','policy cr4 readback mismatch',lambda:V.policy_observation(previous,old_topology),{'immutable_base':'previous-observation'})
    if baseline_source:
        proc=subprocess.run([sys.executable,'-B',str(baseline_source/'scripts/verify-memory-observation.py'),'--logfile',str(old/'qemu-serial.log')],text=True,capture_output=True)
        (output/'previous-observation.stdout.log').write_text(proc.stdout);(output/'previous-observation.stderr.log').write_text(proc.stderr)
        E.require(proc.returncode==0,'previous full observation replay failed: '+proc.stdout+proc.stderr)
    cpu=O.records(base,'CPU_CONTROL','CPU');ap=next(r for r in cpu if r['role']=='AP');slot=int(ap['slot'])
    changes=[('ap-wp-clear','cr0',0x80000033,'cr0'),('ap-cd-set','cr0',0xc0010033,'cr0'),
             ('ap-nw-set','cr0',0xa0010033,'cr0'),('ap-ne-clear','cr0',0x80010013,'cr0'),
             ('ap-cr0-residue','cr0',0x8001003b,'cr0'),('ap-cr4-different','cr4',0x668,'cr4'),
             ('supported-nx-clear','efer',0x500,'EFER/NX'),('wrong-common-pat','pat',0x0007040600070406,'pat'),
             ('framebuffer-slot-four-changed','pat',0x0007010100070406,'pat')]
    for name,key,value,reason in changes:
        change={slot:{key:hex(value)}}
        if name=='wrong-common-pat':change={s:{key:hex(value)} for s in topology}
        lines=mutate_collection(base,change)
        O.verify_cpu(lines,topology)
        reject(name,'policy '+reason+' readback mismatch',lambda lines=lines:V.policy_observation(lines,topology),change)
    change={s:{'cr4':hex(0x20|(1<<20))} for s in topology}
    reject('prohibited-cr4-everywhere','policy cr4 readback mismatch',lambda:V.policy_observation(mutate_collection(base,change),topology),change)
    change={int(r['slot']):{'extended_edx':hex(V.number(r,'extended_edx')&~(1<<20))} for r in cpu}
    reject('nxe-with-nx-absent','policy EFER/NX readback mismatch',lambda:V.policy_observation(mutate_collection(base,change),topology,True),change)
    bsp=next(s for s,(_,role) in topology.items() if role=='BSP')
    change={bsp:{'role':'AP'},slot:{'role':'BSP'}}
    reject('roles-swapped','CPU role differs',lambda:V.policy_observation(mutate_collection(base,change),topology),change)
    change={slot:{'generation':'99'}}
    reject('stale-generation','stale CPU generation',lambda:V.policy_observation(mutate_collection(base,change),topology),change)
    raw=(current/'qemu-serial.log').read_text().splitlines();V.readiness(raw,topology)
    ready=next(l for l in raw if l.startswith('[CPU_STATE][READY] slot='+str(slot)+' '))
    reject('duplicate-readiness','readiness missing/duplicate',lambda:V.readiness(raw+[ready],topology),{'append':ready})
    reject('missing-readiness','readiness missing/duplicate',lambda:V.readiness([l for l in raw if l!=ready],topology),{'remove':ready})
    online=f'[SMP][CPU] ONLINE slot={slot} apic={topology[slot][0]} role=AP'
    late=[l for l in raw if l!=ready];late.insert(late.index(online)+1,ready)
    reject('online-before-validation','ONLINE precedes local validation',lambda:V.readiness(late,topology),{'move_after':online,'record':ready})
    reject('ordinary-model-relabeled-nx-off','NX capability differs',lambda:V.policy_observation(base,topology,True),{'profile':'nx-disabled'})
    G=module('cpu-control-state-gate')
    recorded=V.verify_run(current);relocated=copy.deepcopy(recorded)
    target=Path('/synthetic-extraction/qemu-serial.log')
    relocated['observation']['logfile']=str(target)
    class FixtureTransport:
        def __init__(self, mapped): self.target=mapped
        def mapped(self, name):
            E.require(name==recorded['observation']['logfile'],'synthetic original path differs')
            return self.target
    G.verify_mode_run(recorded,copy.deepcopy(relocated),FixtureTransport(target))
    reject('portable-log-path-misdirected','control serial path transport binding differs',
           lambda:G.verify_mode_run(recorded,copy.deepcopy(relocated),FixtureTransport(Path('/synthetic-other/qemu-serial.log'))),
           {'scope':'isolated portable path policy with real validated run fields'})
    changed=copy.deepcopy(relocated);changed['policy']['cr0']=0
    reject('portable-control-value-changed','completed mode raw replay differs',
           lambda:G.verify_mode_run(recorded,changed,FixtureTransport(target)),
           {'scope':'synthetic measured-value contradiction survives correct path relocation'})
    with tempfile.TemporaryDirectory(prefix='cpu-verification-revision-') as temporary:
        root=Path(temporary);receipt=root/'control-acceptance.json'
        synthetic={'status':'PASS','synthetic':True,'build_input_sha256':E.identity(E.manifest()),'harness_sha256':'0'*64}
        E.write(receipt,synthetic);original=receipt.read_bytes()
        archived=G.begin_harness_reverification(root)
        E.require(archived and not receipt.exists() and (archived/receipt.name).read_bytes()==original,
                  'verification revision lost original receipt')
        synthetic['build_input_sha256']='f'*64;E.write(receipt,synthetic)
        reject('harness-reverification-linked-input-drift','harness re-verification cannot reuse changed build inputs',
               lambda:G.begin_harness_reverification(root),{'scope':'synthetic revision policy; no runtime acceptance'})
    P=module('cpu-control-portable-replay')
    families={r['key']:{} for r in json.loads((ROOT/'scripts/memory-current-requirements.json').read_text())['requirements']}
    summary={'package_content_replay':'PASS','current_acceptance':'PASS','native':families,'series_runs':15}
    def native_process(value, code=0, suffix=''):
        return subprocess.CompletedProcess(['synthetic-native-output'],code,
            'Synthetic native progress; no runtime claim.\n'+json.dumps(value)+'\n'+suffix,'')
    E.require(P.native_output(native_process(summary),15)==summary,'native progress/summary positive control')
    native_scope={'scope':'isolated synthetic output parser; not a native run or boot repetition'}
    reject('native-summary-nonzero-exit','native portable acceptance failed',
           lambda:P.native_output(native_process(summary,1),15),native_scope)
    omitted=copy.deepcopy(summary);del omitted['native']['section-layout']
    reject('native-summary-missing-family','native portable family inventory differs',
           lambda:P.native_output(native_process(omitted),15),native_scope)
    wrong=copy.deepcopy(summary);wrong['series_runs']=1
    reject('native-summary-wrong-run-count','native portable series count differs',
           lambda:P.native_output(native_process(wrong),15),native_scope)
    reject('native-summary-trailing-garbage','native portable final summary malformed',
           lambda:P.native_output(native_process(summary,suffix='unexpected trailing output\n'),15),native_scope)
    failed=copy.deepcopy(summary);failed['current_acceptance']='FAIL'
    reject('native-summary-reported-failure','native portable final summary status differs',
           lambda:P.native_output(native_process(failed),15),native_scope)
    makefile=(ROOT/'makefile').read_text()
    dependency='$(KERNEL_DIR)/src/smp/trampoline.o: $(KERNEL_DIR)/src/cpu/control_policy_bits.h'
    E.require(makefile.count(dependency)==1,'trampoline header prerequisite fixture anchor')
    G.shared_header_dependency(makefile)
    reject('missing-trampoline-header-dependency','shared policy header does not rebuild trampoline',
           lambda:G.shared_header_dependency(makefile.replace(dependency,'')),
           {'path':'makefile','remove':dependency,'boundary':'actual Make dependency graph; dry run only'})
    symbols=(evidence/'candidates/production/symbols.txt').read_text();G.verify_symbols(symbols,True)
    reject('production-initializer-compiled-out','initializer compiled out',
           lambda:G.verify_symbols(re.sub(r'^.*\bcpu_policy_init_bsp$','',symbols,flags=re.M),True),{'remove_symbol':'cpu_policy_init_bsp'})
    reject('production-observer-linked','production observer payload present',
           lambda:G.verify_symbols(symbols+'\n0000000000000001 T cmd_cpustate\n',True),{'append_symbol':'cmd_cpustate'})
    if (evidence/'series.json').exists() or json.loads((current/'result.json').read_text()).get('readback'):
        selected=(json.loads((evidence/'series.json').read_text())['diagnostics'][0]
                  if (evidence/'series.json').exists() else current.relative_to(evidence).as_posix())
        diagnostic=evidence/selected;result=json.loads((diagnostic/'result.json').read_text())
        lines=(diagnostic/'qemu-serial.log').read_text().splitlines()
        ready=V.readiness(lines,topology);V.control_readback(diagnostic,ready,result);V.cache_snapshot(diagnostic)
        with tempfile.TemporaryDirectory(prefix='cpu-readback-fixtures-') as temp:
            directory=Path(temp)
            names=('capture-config.json','control-readback.json','control-threads.log','vm-identity.jsonl','page-readback.stdout.log')
            for name in names:shutil.copy2(diagnostic/name,directory/name)
            proof=json.loads((directory/'control-readback.json').read_text())
            original=copy.deepcopy(proof['cpus'][1]);proof['cpus'][1]['registers']['cr0']&=~(1<<16)
            output_raw=(directory/'page-readback.stdout.log').read_text()
            original_line='CPU_STATE_GDB '+json.dumps(original,sort_keys=True,separators=(',',':'))
            changed_line='CPU_STATE_GDB '+json.dumps(proof['cpus'][1],sort_keys=True,separators=(',',':'))
            E.require(output_raw.count(original_line)==1,'control readback fixture anchor')
            (directory/'page-readback.stdout.log').write_text(output_raw.replace(original_line,changed_line))
            (directory/'control-readback.json').write_text(json.dumps(proof))
            reject('desired-ready-constant-disagrees-with-raw-debugger','debugger/raw local disagreement: cr0',
                   lambda:V.control_readback(directory,ready,result),{'base':selected,'cpu_index':1,'cr0_clear':65536,'recompute_outer':True})
        with tempfile.TemporaryDirectory(prefix='cpu-cache-fixtures-') as temp:
            directory=Path(temp)
            meta=json.loads((diagnostic/'table-snapshot.json').read_text());data=bytearray((diagnostic/'table-frames.bin').read_bytes())
            frames={a:bytes(data[i*4096:(i+1)*4096]) for i,a in enumerate(meta['frame_addresses'])}
            levels=V.T.traverse(meta['active_root'],frames,meta['physical_bits'],meta['page1g'])
            target=next((a,i,entry) for a,level in levels.items() if level==1
                        for i,(entry,) in enumerate(struct.iter_unpack('<Q',frames[a])) if entry&1 and entry&0x98==0)
            address,index,entry=target;offset=meta['frame_addresses'].index(address)*4096+index*8
            struct.pack_into('<Q',data,offset,entry|0x88)
            (directory/'table-frames.bin').write_bytes(data);meta['frames_sha256']=E.sha(directory/'table-frames.bin')
            (directory/'table-snapshot.json').write_text(json.dumps(meta))
            owned=os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks')
            if owned:shutil.copy2(diagnostic/'qemu-serial.log',directory/'qemu-serial.log')
            reject('unused-slot-claim-contradicted-by-decoded-leaf','WC outside owned framebuffer' if owned else 'cache spare slot used',lambda:V.cache_snapshot(directory),
                   {'base':selected,'table':address,'index':index,'old_entry':entry,'new_entry':entry|0x88,
                    'materialized_frames_sha256':meta['frames_sha256'],'recompute_outer':True})
    # These source mutations are disposable and never modify packaged originals.
    with tempfile.TemporaryDirectory(prefix='cpu-scope-fixtures-') as temp:
        root=Path(temp)
        contract=cpu_current.contract()
        names=set(contract['immutable_reviewed_anchors'])|{'scripts/cpu-control-scope.json','scripts/memory-current-scope.json'}
        if 'protected_functions' in contract:
            names|=set(contract['protected_functions'])|{'scripts/tlb-coherency-scope.json','kernel/src/apic/lapic.h'}
        if 'promoted_sources' in contract:
            names|=set(contract['promoted_sources'])|{'scripts/physical-translation-scope.json'}
        if contract['name']=='kernel-image-protection':names|={'scripts/kernel-image-scope.json'}
        if contract['name']=='guarded-kernel-stacks':names|={'scripts/stack-guard-scope.json'}
        if contract['name']=='low-address-isolation':names|={'scripts/low-address-scope.json'}
        if contract['name']=='verified-ram-direct-map':
            names|={'scripts/direct-map-scope.json'}
        if contract['name']=='owned-device-mapping':
            names|={'scripts/device-mapping-scope.json',*contract['historical_contracts']}
        for name in names:
            target=root/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target)
        cpu_current.contract(root)
        for name,label in [('kernel/src/graphics/graphics.c','unrelated-protected-edit'),('kernel/src/smp/ap_entry.S','wrong-ap-handoff'),('kernel/src/core/interrupts.c','lost-exception-recovery')]:
            path=root/name;original=path.read_bytes();path.write_bytes(original+b'\n/* Synthetic unauthorized change. */\n')
            boundary=('RAM candidate anchor drift' if name in contract['promoted_sources'] else 'RAM protected file drift') if contract['name']=='verified-ram-direct-map' else ('candidate anchor drift' if contract['name']=='owned-device-mapping' and name in contract['promoted_sources'] else 'protected anchor drift')
            if contract['name']=='low-address-isolation':boundary='low-domain promoted source differs' if name in contract['promoted_sources'] else 'low-domain protected file drift'
            if contract['name']=='kernel-image-protection':boundary='image protection promoted source differs' if name in contract['promoted_sources'] else 'image protection protected file drift'
            if contract['name']=='guarded-kernel-stacks':boundary='stack promoted source differs' if name in contract['promoted_sources'] else 'stack protected file drift'
            reject(label,boundary,lambda:cpu_current.contract(root),{'path':name,'append':'comment'})
            path.write_bytes(original)
        for source in (ROOT/'kernel').rglob('*'):
            if source.suffix in ('.c','.h','.S') and not (root/source.relative_to(ROOT)).exists():
                target=root/source.relative_to(ROOT);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        cpu_current.audit_sites(root)
        instruction_path='kernel/src/smp/cpu_request.c' if 'protected_functions' in contract else 'kernel/src/cpu/control_policy.c'
        if 'promoted_sources' in contract:
            # Reach the instruction inventory without first violating a fixed
            # whole-file anchor for the unchanged request implementation.
            instruction_path='kernel/src/cpu/synthetic_instruction.c'
        path=root/instruction_path
        path.write_text((path.read_text() if path.exists() else '')+'\nvoid injected(void) { __asm__ volatile("wrmsr"); }\n')
        reject('unauthorized-instruction-site','unauthorized architectural instruction site',lambda:cpu_current.audit_sites(root),{'path':instruction_path,'append':'raw wrmsr'})
    if compiled: compiled_mutations(output,cases)
    result={'schema':1,'status':'PASS','scope':'semantic fixtures and isolated compiled source mutations; not VM boot repetitions',
            'build_input_sha256':E.identity(E.manifest()),'harness_sha256':E.identity(E.manifest(harness=True)),
            'base_serial_sha256':E.sha(current/'qemu-serial.log'),'previous_serial_sha256':E.sha(old/'qemu-serial.log'),
            'positive_controls':['new-complete-and-normalized','previous-complete-nonuniform','unchanged-protected-scope','exact-instruction-sites',
                'portable-log-relocation','harness-reverification-preserves-receipts','native-output-final-summary']+
                (['previous-full-observation-replay'] if baseline_source else []),
            'cases':cases}
    E.write(output/'results.json',result)
    # JSON object keys are strings; replay compares the persisted wire format.
    return json.loads((output/'results.json').read_text())


def compiled_mutations(output,cases):
    files=['scripts/cpu-control-policy-host.c','kernel/src/cpu/control_policy.c','kernel/src/cpu/control_policy.h',
           'kernel/src/cpu/control_policy_bits.h','kernel/src/memory/cache_attributes.c','kernel/src/memory/cache_attributes.h',
           'kernel/src/memory/page_table_decode.c','kernel/src/memory/page_table_decode.h']
    recipes=[
        ('missing-first-cache-flush','kernel/src/cpu/control_policy.c','    flush_cache();\n','    /* Synthetic omitted flush. */\n',1),
        ('pat-before-validation','kernel/src/cpu/control_policy.c','    uint64_t flags = save_if();','    uint64_t flags = save_if();\n    if (p) (void)write_msr(CPU_MSR_PAT, p->pat);',1),
        ('missing-final-tlb-flush','kernel/src/cpu/control_policy.c','    flush_cache();\n    write_cr(3, before.cr3);','    flush_cache();',1),
        ('wrong-final-cache-mode','kernel/src/cpu/control_policy.c','    write_cr(0, p->cr0_final);','    write_cr(0, p->cr0_final | CPU_CR0_CD);',1),
        ('wb-zero-treated-as-failure','kernel/src/memory/cache_attributes.c','    return true;','    return *out != 0;',1),
        ('large-leaf-pat-confused-with-ps','kernel/src/memory/cache_attributes.c','leaf == CACHE_LEAF_4K ? 7 : 12','leaf == CACHE_LEAF_4K ? 7 : 7',1),
    ]
    for name,file,old,new,count in recipes:
        destination=output/name;destination.mkdir()
        with tempfile.TemporaryDirectory(prefix='cpu-compiled-fixture-') as temp:
            root=Path(temp)
            for source in files:
                path=root/source;path.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/source,path)
            path=root/file;before=path.read_text();E.require(old in before,'source mutation anchor missing');path.write_text(before.replace(old,new,count))
            argv=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-DHOBBYOS_CONTROL_POLICY_HOST_TEST=1',
                  'scripts/cpu-control-policy-host.c','kernel/src/cpu/control_policy.c','kernel/src/memory/cache_attributes.c','kernel/src/memory/page_table_decode.c','-o','fixture']
            build=subprocess.run(argv,cwd=root,text=True,capture_output=True)
            (destination/'compile.stdout.log').write_text(build.stdout);(destination/'compile.stderr.log').write_text(build.stderr)
            E.require(build.returncode==0,'synthetic policy failed compilation: '+name+build.stderr)
            result=subprocess.run([str(root/'fixture')],cwd=root,text=True,capture_output=True)
            (destination/'run.stdout.log').write_text(result.stdout);(destination/'run.stderr.log').write_text(result.stderr)
            E.require(result.returncode==1 and 'check failed at line' in result.stderr,'compiled mutation escaped actual C tests: '+name)
            cases.append({'name':name,'synthetic':True,'expected_exit':1,'actual_exit':result.returncode,
                'expected_boundary':'actual C independent oracle','actual_reason':result.stderr.strip(),
                'recipe':{'path':file,'old':old,'new':new,'count':count,'base_sha256':E.sha(ROOT/file),'materialized_sha256':E.sha(path)},
                'compile_argv':argv,'compile_exit':build.returncode})


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--baseline-source',type=Path);p.add_argument('--no-compiled',action='store_true');a=p.parse_args()
    try:
        result=run(a.evidence.resolve(),a.output.resolve(),a.baseline_source,not a.no_compiled)
        print(json.dumps({'status':result['status'],'negative_controls':len(result['cases'])}))
    except Exception as error:p.exit(1,str(error)+'\n')
