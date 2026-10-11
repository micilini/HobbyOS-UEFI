#!/usr/bin/env python3
"""Control-policy gate using the accepted memory transport and native regressions."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import memory_evidence as E
import validation_candidates as candidates
import cpu_current

ROOT=Path(__file__).resolve().parent.parent
os.environ.setdefault('HOBBYOS_VALIDATION_CONTRACT','cpu-control-state')
os.environ['PYTHONDONTWRITEBYTECODE']='1'


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value


G=module('memory-observation-gate')
V=module('verify-cpu-control-state')


def receipt(evidence, name, result):
    E.write(evidence/(name+'.json'),{'schema':1,'status':'PASS',
        'build_input_sha256':E.identity(E.manifest()),'harness_sha256':E.identity(E.manifest(harness=True)),
        'result':result})


def shared_header_dependency(makefile=None):
    """Ask Make about the real rule, using synthetic timestamps and no build."""
    header='kernel/src/cpu/control_policy_bits.h'
    source='kernel/src/smp/trampoline.S'
    target='kernel/src/smp/trampoline.o'
    with tempfile.TemporaryDirectory(prefix='cpu-header-dependency-') as temporary:
        root=Path(temporary)
        (root/'makefile').write_text((ROOT/'makefile').read_text() if makefile is None else makefile)
        for name in (header,'kernel/src/memory/paging_boot_bits.h',source,target):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'')
            os.utime(path,(1000000100 if name==target else 1000000000,)*2)
        argv=['make','--no-print-directory','-n','-W',header,target]
        command=subprocess.run(argv,cwd=root,text=True,capture_output=True)
        E.require(command.returncode==0 and '-c '+source+' -o '+target in command.stdout,
                  'shared policy header does not rebuild trampoline')
        return {'header':header,'target':target,'command':argv,'exit_status':command.returncode,
                'recipe':command.stdout.strip(),'execution':'DRY_RUN_SYNTHETIC_TIMESTAMPS'}


def control_semantics():
    current = cpu_current
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('tlb-coherency','physical-translation','owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import tlb_current
        current = tlb_current
    current.contract()
    audit=current.audit_sites()
    source=(ROOT/'kernel/src/cpu/control_policy.c').read_text()
    E.require('cpu_write_msr(' not in source and 'wrmsr' not in source and 'cpuid"' not in source,
              'control normalizer bypasses checked CPU APIs')
    paging=(ROOT/'kernel/src/memory/paging.c').read_text()
    E.require('cpu_write_msr' not in paging and 'wbinvd' not in paging, 'stale paging PAT writer')
    if current is cpu_current:
        E.require(E.sha(ROOT/'kernel/src/memory/paging.c')==cpu_current.contract()['mapping_source_sha256'],
                  'mapping producer changed beyond removal of PAT writer')
    # The invalidation successor pins every existing paging function separately.
    else: current.contract()
    init=(ROOT/'kernel/src/core/kernel_init.c').read_text()
    E.require(init.index('init_idt();')<init.index('cpu_policy_init_bsp();')<init.index('init_pmm('), 'early BSP policy ordering')
    ap=(ROOT/'kernel/src/smp/smp_boot.c').read_text()
    E.require(ap.index('cpu_policy_confirm_local(')<ap.index('init_lapic_ap();')<ap.index('smp_mark_cpu_online('), 'AP readiness ordering')
    E.require('current_cr4' not in ap and 'current_efer' not in ap, 'firmware snapshot policy copied to AP')
    for instruction in ('cpuid','wrmsr','rdmsr'):
        E.require(not re.search(r'__asm__[^;]*\b'+instruction+r'\b',init+ap), 'unauthorized raw CPU instruction site')
    return {'instruction_audit':audit,'mapping_source_sha256':E.sha(ROOT/'kernel/src/memory/paging.c'),
            'early_bsp_and_ap_readiness_order':'PASS','checked_C_CPU_APIs':'PASS',
            'assembly_header_dependency':shared_header_dependency()}


def static(evidence):
    scope=cpu_current.scope('control-policy')
    scope.update(control_semantics())
    G.static(evidence)
    receipt(evidence,'control-static',scope)


def host(evidence, inherited=True):
    directory=evidence/'control-host'/G.stamp();directory.mkdir(parents=True)
    result=[]
    for name,flags in [('normal',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all']),
                       ('asan',['-fsanitize=address','-fno-omit-frame-pointer'])]:
        binary=directory/name
        compile=G.run(directory,'compile-'+name,['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O2','-fno-pie','-no-pie',
            '-DHOBBYOS_CONTROL_POLICY_HOST_TEST=1',*flags,'scripts/cpu-control-policy-host.c','kernel/src/cpu/control_policy.c',
            'kernel/src/memory/cache_attributes.c','kernel/src/memory/page_table_decode.c','-o',str(binary)])
        execute=G.run(directory,'execute-'+name,[str(binary)],{'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
        raw=(execute/'stdout.log').read_text()
        E.require(re.fullmatch(r'CPU_POLICY_HOST status=PASS checks=[1-9][0-9]* privileged_operations=SUBSTITUTED\n',raw), 'actual C policy host summary')
        result.append({'mode':name,'compile':str(compile.relative_to(evidence)),'execute':str(execute.relative_to(evidence)),
                       'summary':raw.strip(),'binary_sha256':E.sha(binary)})
    if inherited:
        (directory/'inherited').mkdir()
        G.host(directory/'inherited')
    receipt(evidence,'control-host',result)


def build(evidence):
    G.build(evidence)
    facts=[]
    L=module('verify-section-layout')
    for name in ('selftest','debug','production'):
        directory=evidence/'candidates'/name
        G.verify_local_candidate(directory)
        for target,command in [('policy-disassembly.txt',['objdump','-d','-Mintel']),
                               ('policy-relocations.txt',['readelf','-Wr'])]:
            (directory/target).write_bytes(subprocess.check_output(command+[str(directory/'kernel.elf')]))
        elf=L.ElfFile.from_path(directory/'kernel.elf');layout=L.validate_layout(elf)
        E.require(layout['trampoline']['sha256']==cpu_current.contract()['trampoline_abi']['sha256'], 'copied bootstrap blob differs from audited candidate')
        nm=(directory/'symbols.txt').read_text()
        for symbol in ('cpu_policy_init_bsp','cpu_policy_apply','cpu_policy_confirm_local','cache_attribute_encode'):
            E.require(re.search(r'\b'+symbol+r'$',nm,re.M),'policy initializer/encoder absent from '+name)
        facts.append({'profile':name,'layout':layout,'disassembly_sha256':E.sha(directory/'policy-disassembly.txt')})
    receipt(evidence,'control-build',facts)


def nx_runtime(evidence):
    directory=evidence/'supplemental';directory.mkdir()
    G.runtime(directory,nx_disabled=True,candidate=evidence/'candidates/selftest',validation=V.verify_run)


def runtime(evidence):
    G.runtime(evidence,validation=V.verify_run)
    series=json.loads((evidence/'series.json').read_text())
    results=[V.verify_run(evidence/name) for name in series['runs']+series['diagnostics']]
    nx_runtime(evidence)
    results += [V.verify_run(evidence/'supplemental'/name) for name in json.loads((evidence/'supplemental/series.json').read_text())['runs']]
    receipt(evidence,'control-runtime',results)


def production(evidence):
    if not (evidence/'production.json').exists(): G.production(evidence)
    G.verify_local_candidate(evidence/'candidates/production')
    runs=json.loads((evidence/'production.json').read_text())['runs']
    receipt(evidence,'control-production',[V.verify_run(evidence/name) for name in runs])


def fixtures(evidence):
    if (evidence/'acceptance.json').exists():
        accepted=json.loads((evidence/'acceptance.json').read_text())['fixtures']
        E.require(accepted['harness_sha256']==E.identity(E.manifest(harness=True)), 'observation fixtures need current verifier replay')
    else:
        accepted=G.fixtures(evidence)
    directory=evidence/'control-fixtures'/G.stamp()
    baseline=next((p/'before/source' for p in evidence.parents if (p/'before/source').is_dir()),None)
    E.require(baseline is not None,'approved baseline source export missing')
    G.run(evidence,'control-negative-fixtures',['python3','scripts/cpu-control-fixtures.py','--evidence',str(evidence),
        '--output',str(directory),'--baseline-source',str(baseline)])
    # Recompile the actual policy with the final oracle sources. The initial
    # host receipt remains immutable when unrelated collector sources evolve.
    host(directory,inherited=False)
    receipt(evidence,'control-fixtures',{'path':directory.relative_to(evidence).as_posix(),'sha256':E.sha(directory/'results.json'),
                                       'observation_fixtures':accepted})


def verify_command(evidence, relative):
    E.safe(relative); directory=evidence/relative
    E.require(directory.resolve().is_relative_to(evidence.resolve()),'unsafe mode command path')
    row=json.loads((directory/'command.json').read_text())
    E.require(row['exit_status']==0 and row['build_input_sha256']==E.identity(E.manifest()) and
              row['started_ns']<row['finished_ns'] and E.sha(directory/'stdout.log')==row['stdout_sha256'],
              'mode command raw receipt mismatch')
    return row,(directory/'stdout.log').read_text()


def verify_host(evidence, current_harness=False):
    receipt=json.loads((evidence/'control-host.json').read_text())
    if current_harness:
        E.require(receipt['harness_sha256']==E.identity(E.manifest(harness=True)),
                  'final actual C host verifier sources changed')
    rows=receipt['result']
    E.require([r['mode'] for r in rows]==['normal','ubsan','asan'],'control host profile coverage')
    for row in rows:
        built,_=verify_command(evidence,row['compile']);ran,output=verify_command(evidence,row['execute'])
        E.require(built['harness_sha256']==ran['harness_sha256']==receipt['harness_sha256'],
                  'policy host sources changed between compilation and execution')
        argv=built['argv'];mode=row['mode']
        flags={'normal':[],'ubsan':['-fsanitize=undefined','-fno-sanitize-recover=all'],
               'asan':['-fsanitize=address','-fno-omit-frame-pointer']}[mode]
        expected=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O2','-fno-pie','-no-pie',
            '-DHOBBYOS_CONTROL_POLICY_HOST_TEST=1',*flags,'scripts/cpu-control-policy-host.c',
            'kernel/src/cpu/control_policy.c','kernel/src/memory/cache_attributes.c','kernel/src/memory/page_table_decode.c','-o']
        E.require(argv[:-1]==expected and ran['argv']==[argv[-1]] and Path(argv[-1]).name==mode,
                  'actual policy C compile/execute contract differs')
        E.require(output.strip()==row['summary'] and re.fullmatch(
            r'CPU_POLICY_HOST status=PASS checks=8100 privileged_operations=SUBSTITUTED\n',output),
            'actual policy host coverage/output differs')
    return {'profiles':[r['mode'] for r in rows],'checks_per_profile':8100,'privileged_operations':'SUBSTITUTED'}


def verify_build(evidence, transport=None):
    rows=json.loads((evidence/'control-build.json').read_text())['result']
    E.require([r['profile'] for r in rows]==['selftest','debug','production'],'control build profile coverage')
    layouts=module('verify-section-layout');result=[]
    for row in rows:
        directory=evidence/'candidates'/row['profile']
        candidate=json.loads((directory/'candidate.json').read_text())
        build=json.loads((directory/'build-receipt.json').read_text())
        identity=E.verify_candidate(candidate,build)
        if transport is None:
            E.require(G.verify_local_candidate(directory)==identity,'control local candidate identity')
            layout=layouts.validate_layout(layouts.ElfFile.from_path(candidates.artifact(directory/'kernel.elf')))
        else:
            for name,item in candidate['artifacts'].items():transport.binary(directory/name,item['sha256'],item['size'])
            layout=layouts.validate_layout(transport.elf(directory/'kernel.elf'))
        E.require({k:v for k,v in layout.items() if k!='elf_sha256'}==
                  {k:v for k,v in row['layout'].items() if k!='elf_sha256'} and
                  row['layout']['elf_sha256']==candidate['artifacts']['kernel.elf']['sha256'] and
                  layout['trampoline']['sha256']==cpu_current.contract()['trampoline_abi']['sha256'],
                  'control candidate bootstrap/layout facts differ')
        E.require(E.sha(directory/'policy-disassembly.txt')==row['disassembly_sha256'],'control disassembly raw hash differs')
        symbols=(directory/'symbols.txt').read_text()
        verify_symbols(symbols,row['profile']=='production')
        result.append({'profile':row['profile'],'candidate_id':identity,'elf_sha256':candidate['artifacts']['kernel.elf']['sha256']})
    return result


def verify_symbols(symbols, production):
    for symbol in ('cpu_policy_init_bsp','cpu_policy_apply','cpu_policy_confirm_local','cache_attribute_encode'):
        E.require(re.search(r'\b'+symbol+r'$',symbols,re.M),'production/control initializer compiled out')
    E.require('cpu_policy_test_' not in symbols,'substituted privileged primitives leaked into kernel')
    if production:
        for symbol in G.SURFACE:E.require(not re.search(r'\b'+symbol+r'$',symbols,re.M),'production observer payload present')


def verify_mode_run(recorded, observed, transport=None):
    if transport is not None and 'logfile' in observed.get('observation', {}):
        original = recorded.get('observation', {}).get('logfile')
        actual = Path(observed['observation']['logfile']).resolve()
        E.require(isinstance(original, str) and transport.mapped(original).resolve() == actual,
                  'control serial path transport binding differs')
        # Retain the original provenance label only after binding it to verified
        # extracted bytes. No measurement or identity is normalized.
        observed['observation']['logfile'] = original
    E.require(all(key in recorded for key in ('status','candidate_id','profile','vm_id','boot_id','ready_cpus','policy')) and
              all(key in observed and observed[key] == value for key,value in recorded.items()),
              'completed mode raw replay differs')


def begin_harness_reverification(evidence):
    """Start a fresh verification attempt while preserving all capture facts."""
    accepted = evidence / 'control-acceptance.json'
    generic = evidence / 'acceptance.json'
    if not accepted.exists() and not generic.exists(): return None
    previous = json.loads((accepted if accepted.exists() else generic).read_text())
    source = previous['build_input_sha256'] if accepted.exists() else previous['fixtures']['source_sha256']
    harness = previous['harness_sha256'] if accepted.exists() else previous['fixtures']['harness_sha256']
    E.require(source == E.identity(E.manifest()), 'harness re-verification cannot reuse changed build inputs')
    current = E.identity(E.manifest(harness=True))
    if harness == current: return None
    directory = evidence / 'attempts' / (G.stamp() + '-harness-reverification')
    directory.mkdir(parents=True, exist_ok=False)
    names = [name for name in ('acceptance.json','control-acceptance.json','control-fixtures.json')
             if (evidence / name).exists()]
    rows = [{'path':name,'sha256':E.sha(evidence/name),'size':(evidence/name).stat().st_size} for name in names]
    E.write(directory / 'start.json', {'schema':1,'status':'REPLAY_PENDING','build_input_sha256':source,
        'previous_harness_sha256':harness,'harness_sha256':current,'archived_receipts':rows,
        'raw_capture_policy':'reuse only after independent replay; no new boot claimed'})
    for row in rows:
        original = evidence / row['path']
        E.require(E.sha(original) == row['sha256'], 'acceptance receipt changed during preservation')
        original.rename(directory / row['path'])
    return directory


def verify(evidence, transport=None):
    if transport is None: cpu_current.scope('control-policy-replay')
    if transport is None: G.verify_acceptance(evidence)
    G.verify_static(evidence)
    semantics=control_semantics()
    for name in ('control-static','control-host','control-build','control-runtime','control-production','control-fixtures'):
        value=json.loads((evidence/(name+'.json')).read_text())
        E.require(value['status']=='PASS' and value['build_input_sha256']==E.identity(E.manifest()),'missing/stale required mode: '+name)
    V.observation().verify_series(evidence)
    series=json.loads((evidence/'series.json').read_text())
    current=[V.verify_run(evidence/path) for path in series['runs']+series['diagnostics']]
    V.observation().verify_series(evidence/'supplemental')
    supplemental=json.loads((evidence/'supplemental/series.json').read_text())
    current += [V.verify_run(evidence/'supplemental'/path) for path in supplemental['runs']]
    production=json.loads((evidence/'production.json').read_text())
    production_runs=[V.verify_run(evidence/path) for path in production['runs']]
    for mode,derived in [('control-runtime',current),('control-production',production_runs)]:
        recorded=json.loads((evidence/(mode+'.json')).read_text())['result']
        E.require(len(recorded)==len(derived),'completed mode run inventory differs: '+mode)
        for old,new in zip(recorded,derived):
            verify_mode_run(old,new,transport)
    ledger=(module('memory-current-regression').verify_ledger(evidence,E.identity(E.manifest())) if transport is None
            else module('memory-portable-replay').regression_ledger(evidence,transport))
    ledger['historical']=json.loads((evidence/'regression-ledger.json').read_text())['historical_unavailable']
    fixtures=json.loads((evidence/'control-fixtures.json').read_text())['result'];E.safe(fixtures['path'])
    raw=json.loads((evidence/fixtures['path']/'results.json').read_text())
    E.require(E.sha(evidence/fixtures['path']/'results.json')==fixtures['sha256'] and raw['status']=='PASS' and
              raw['build_input_sha256']==E.identity(E.manifest()) and raw['harness_sha256']==E.identity(E.manifest(harness=True)),
              'control negative fixture raw/source/verifier binding')
    module('cpu-control-fixtures').verify_receipt(raw)
    final_host=verify_host(evidence/fixtures['path'],current_harness=True)
    E.require(final_host==verify_host(evidence),'initial/final actual C coverage differs')
    proof=V.cache_comparison(evidence/'previous-observation',evidence/series['diagnostics'][0])
    return {'status':'PASS','contract':'cpu-control-state','normal_boots':len(series['runs']),
            'nx_disabled_boots':len(supplemental['runs']),'production_boots':len(production['runs']),
            'runs':current,'inherited':ledger,'static':semantics,'host':final_host,
            'production':production_runs,
            'builds':verify_build(evidence,transport),'cache_invariance':proof,
            'baseline':V.observation().verify_series(evidence)['baseline'],
            'negative_fixture_sha256':fixtures['sha256'],'hardware':'DEFERRED_TO_FINAL_CLOSURE'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=('static','host','build','runtime','production','fixtures','verify','all'))
    p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence.resolve()
    e.relative_to(ROOT/'backups');e.mkdir(parents=True,exist_ok=True)
    if a.mode=='all':
        begin_harness_reverification(e)
        for name,marker in [('static','control-static'),('host','control-host'),('build','control-build'),
                            ('runtime','control-runtime'),('production','control-production')]:
            if not (e/(marker+'.json')).exists():globals()[name](e)
        if not (e/'acceptance.json').exists():G.all_modes(e)
        else:G.verify_acceptance(e)
        if not (e/'control-fixtures.json').exists():fixtures(e)
        result=verify(e);receipt(e,'control-acceptance',result);print(json.dumps(result,sort_keys=True))
    elif a.mode=='verify': print(json.dumps(verify(e),sort_keys=True))
    else:globals()[a.mode](e)


if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
