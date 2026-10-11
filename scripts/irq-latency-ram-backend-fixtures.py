#!/usr/bin/env python3
"""Actual RAM plan/property/role controls using synthetic VM interfaces."""
import copy,json,importlib.util
from unittest.mock import patch
import memory_evidence as E
import irq_latency_ram_backend as R
import irq_latency_authority as A

def module(name):
    s=importlib.util.spec_from_file_location(name,E.ROOT/'scripts'/(name+'.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def run(output):
    output.mkdir(parents=True,exist_ok=False);X=module('irq-latency-pairing-experiment');cases=[]
    candidate=output/'candidate';candidate.mkdir()
    info=dict(profile='irq-entry-diagnostic',source_sha256=E.identity(E.manifest()),
        artifacts={'kernel.elf':{'sha256':'1'*64},'hobbyos.img':{'sha256':'2'*64}})
    command=dict(argv=['make','kernel-check','image','SELFTEST=1','IRQ_LATENCY=1','IRQ_ENTRY_DIAGNOSTIC=1','IRQ_LATENCY_DETAILS=0'],
        exit_status=0,build_input_sha256=info['source_sha256'])
    receipt=dict(commands=[dict(receipt='build.json',receipt_sha256=E.identity(command))],status='PASS',
        artifacts=info['artifacts'],inputs_before=info['source_sha256'],build_input_sha256=info['source_sha256'])
    E.write(candidate/'candidate.json',dict(info,build_receipt_sha256=E.identity(receipt)))
    E.write(candidate/'build-receipt.json',receipt)
    E.write(candidate/'build.json',command)
    original=X.module
    def load(name):
        value=original(name)
        if name=='memory-observation-gate':value.verify_local_candidate=lambda p:None
        return value
    plans=[];launches=[];backends=[]
    with patch.object(X,'module',side_effect=load),patch.object(X.subprocess,'check_output',return_value=b''):
        for mode in ('off','on'):
            plan=X.prepare(output/mode,candidate,entry_diagnostic=True,ram_prealloc=mode);plans.append(plan)
            argv=['qemu','-machine','q35,memory-backend=diagnostic.ram','-object',f'memory-backend-ram,id=diagnostic.ram,size=2G,prealloc={mode}',
                '-m','2G','-accel','kvm','-smp','8,sockets=1,cores=8,threads=1','-uuid',plan['vm_ids'][0]]
            launched=dict(argv=argv,qemu_version='synthetic installed QEMU',firmware=[dict(size=1,sha256='firmware')]);launches.append(launched)
            props=dict(uuid={'UUID':plan['vm_ids'][0]},backend='/objects/diagnostic.ram',size=2*1024**3,prealloc=mode=='on',
                memory={'base-memory':2*1024**3,'plugged-memory':0},share=False,merge=True,dump=True,reserve=True,
                **{'host-nodes':[],'policy':'default','prealloc-threads':1,'capabilities':{}})
            row=dict(begin_ns=1,end_ns=2,values={k:dict(status='AVAILABLE',value=v) for k,v in props.items()},
                transcript=[dict(receive={'id':'ram-'+k,'return':v}) for k,v in props.items()]);backends.append(row)
    assert R.controlled_pair(plans,launches,backends)['timing_successes']==0
    cases.append(dict(name='actual-plan-two-conditions',status='PASS',vm_launched=False))
    def reject(name,change):
        ps,ls,bs=copy.deepcopy((plans,launches,backends));change(ps,ls,bs)
        try:R.controlled_pair(ps,ls,bs)
        except (ValueError,KeyError):pass
        else:raise AssertionError(name)
        cases.append(dict(name=name,status='REJECTED'))
    reject('different-proof-policy',lambda ps,ls,bs:ps[1]['proof_output'].update(policy='immediate'))
    reject('missing-proof-policy',lambda ps,ls,bs:ps[1].pop('proof_output'))
    reject('different-work-after-setup',lambda ps,ls,bs:ps[1]['commands'].insert(2,'mem'))
    for key,value in [('candidate_id','other'),('detail_policy','bounded-details-v1'),('observer_contract_sha256','other'),
                      ('heap_operation_contract','old'),('build_identity','other'),('harness_identity','other')]:
        reject('pair-changed-'+key,lambda ps,ls,bs,k=key,v=value:ps[1].update({k:v}))
    reject('prealloc-promoted-to-timing',lambda ps,ls,bs:ps[1].update(role='CURRENT_TIMING_QUALIFICATION'))
    reject('wrong-guest-RAM',lambda ps,ls,bs:ls[0]['argv'].__setitem__(6,'3G'))
    reject('unverified-prealloc-property',lambda ps,ls,bs:bs[0]['values']['prealloc'].update(value=True))
    reject('missing-backend',lambda ps,ls,bs:bs[0]['values'].pop('backend'))
    reject('different-VM-property',lambda ps,ls,bs:bs[0]['values']['uuid'].update(value={'UUID':'other'}))
    reject('ordinary-argv-claimed-explicit',lambda ps,ls,bs:ls[0]['argv'].__setitem__(2,'q35'))
    reject('different-firmware',lambda ps,ls,bs:ls[1]['firmware'][0].update(sha256='other'))
    reject('different-executor',lambda ps,ls,bs:ls[1].update(qemu_version='other'))
    for missing in ({},dict(meminfo='MemAvailable: 1 kB',cgroup={'ancestors':[]})):
        try:R.admit(missing)
        except (ValueError,KeyError):pass
        else:raise AssertionError('missing memory admitted')
        cases.append(dict(name='resident-headroom-rejected-'+str(len(cases)),status='REJECTED'))
    assert R.admit(dict(meminfo='MemAvailable: 16000000 kB',cgroup={'ancestors':[]}))['status']=='ADMITTED'
    try:R.host([])
    except ValueError:cases.append(dict(name='missing-host-phase-not-zero',status='REJECTED'))
    else:raise AssertionError('missing host accepted')
    for role in ('CURRENT_TIMING_QUALIFICATION','CURRENT_FUNCTIONAL_STRESS'):
        try:A.verify_profile_series([],{},role,8,diagnostic_backend=True)
        except ValueError:cases.append(dict(name='backend-forbidden-'+role,status='REJECTED'))
        else:raise AssertionError('diagnostic profile promoted')
    receipt=dict(status='PASS',authority='HOSTED_ORCHESTRATION_ONLY',cases=cases,vm_launched=False)
    E.write(output/'result.json',receipt);return receipt
