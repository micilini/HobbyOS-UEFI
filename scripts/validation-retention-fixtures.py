#!/usr/bin/env python3
"""Actual host retention executor tests; no guest success is inferred."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import importlib.util
import sys
from unittest.mock import patch
import memory_evidence as E
import validation_retention as R
import validation_resources as resources
import validation_candidates as candidates

CASES = set('obsolete-original-removed-without-copy idempotent-resume protected-or-prefix-source.c '
    'protected-or-prefix-untracked.c protected-or-prefix-../escape protected-or-prefix-backups-other/image '
    'retained-raw-only duplicate-with-authoritative-original changed-bytes replaced-inode symlink-directory-escape '
    'symlink-file unexpected-directory-child real-open-reader needed-backing-chain hardlink-source-alias '
    'interrupted-before resumed-before interrupted-after resumed-after lossless-pack-with-consuming-parser '
    'corrupt-required-pack missing-durable-replacement stale-kept-pointer foreign-root launch-maintenance-exclusion '
    'nested-scratch-propagation same-device-overlap-accounting genuine-working-set-shortage '
    'candidate-role-reuse-without-binary-copy stale-candidate-reference '
    'unsealed-run-stream sealed-run-working-copy-retired retired-run-idempotent '
    'firmware-state-sealed firmware-seal-resumed firmware-unknown-file firmware-payload-change '
    'firmware-staged-change firmware-image-change journal-source-binding journal-missing-intent '
    'journal-duplicate-outcome journal-missing-terminal journal-block-substitution '
    'retirement-report-replay retirement-report-count-substitution retirement-report-proof-substitution '
    'campaign-copy-index-not-tree-dependency formatting-resume-index-not-tree-dependency '
    'selected-native-copy-index-not-tree-dependency unknown-reuse-file-remains-dependency '
    'raw-receipt-remains-dependency reused-static-commands-not-imported '
    'reused-build-and-host-commands-retained observation-import-wrong-identity '
    'observation-import-duplicate-member observation-import-missing-member '
    'observation-import-altered-bytes observation-import-forged-candidate '
    'discarded-command-not-tree-dependency discarded-outcome-not-tree-dependency '
    'selected-command-remains-dependency causal-child-receipt-remains-dependency '
    'discarded-missing-terminal discarded-invalid-terminal discarded-command-substitution'.split())

def expected_cases():
    return (CASES | ({'required-bootstrap-prelaunch', 'required-null-candidate'}
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else set()) |
        ({'required-historical-stream','required-image-table-bytes','required-protection-candidate',
          'lsof-unrelated-namespace','lsof-target-device-refused','lsof-target-path-refused','lsof-unknown-mount-preserved',
          'sealed-static-helper-retained','static-helper-contract-boundary','static-helper-byte-substitution',
          'static-helper-missing-seal-member','static-helper-duplicate-seal-member','static-helper-format',
          'static-helper-size-bound','required-static-helper','static-helper-missing-consumer-input',
          'static-helper-restored-consumer-input'}
         if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks') else set()) |
        ({'required-stack-owners','required-stack-retirement-bytes','required-stack-candidate'}
         if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks' else set()))


def verify_receipt(result):
    rows=result['cases']
    E.require(result['schema']==1 and result['status']=='PASS' and result['kind']=='actual-host-retention' and
              result['count']==len(rows)==len(expected_cases()) and {r['name'] for r in rows}==expected_cases() and
              all(r['status']=='PASS' and r['expected'] in ('ACCEPT','REJECT') and
                  (r['expected']=='ACCEPT' or r.get('reason')) for r in rows),'retention fixture coverage/boundary')
    return {'status':'PASS','cases':len(rows)}


def run(output):
    cases = []
    @contextlib.contextmanager
    def scene():
        with tempfile.TemporaryDirectory(prefix='retention-fixture-') as temp:
            root = Path(temp); (root/'backups').mkdir()
            (root/'source.c').write_text('required source\n')
            yield root
    def create(root, name='backups/retired/working.img', data=b'obsolete generated original\0'*512):
        p=root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_bytes(data); p.chmod(0o644); return p
    def keep(root, *names):
        return R.kept_manifest(root,[{'path':n,'consumers':['fixture-native-validator']} for n in ('source.c',*names)])
    def entries(*names):
        return [{'path':n,'category':'run-working-copy','reason':'Stopped disposable run; required candidate retained',
                 'consumers':[]} for n in names]
    def reject(name, reason, operation):
        try: operation()
        except (ValueError,OSError) as error:
            E.require(reason in str(error),name+': wrong boundary: '+str(error))
            cases.append({'name':name,'status':'PASS','expected':'REJECT','reason':reason})
        else: raise ValueError(name+': unexpectedly accepted')
    def positive(name): cases.append({'name':name,'status':'PASS','expected':'ACCEPT'})
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        with scene() as root:
            path=create(root);dev=path.stat().st_dev;other='0:99999'
            line=f'100 90 {other} / /run/snapd/ns rw - tmpfs tmpfs rw\n'
            command,excluded=R.open_reference_command([path],line)
            E.require(command==['lsof','-e','/run/snapd/ns','-nP','-Fpn','--',str(path)] and
                len(excluded)==1,'unrelated namespace exclusion command differs')
            positive('lsof-unrelated-namespace')
            reject('lsof-target-device-refused','open-reference exclusion overlaps artifact filesystem',
                lambda:R.open_reference_command([path],line.replace(other,f'{os.major(dev)}:{os.minor(dev)}')))
            with patch.object(Path,'resolve',return_value=Path('/run/snapd/ns/target')):
                reject('lsof-target-path-refused','open-reference exclusion overlaps artifact filesystem',
                    lambda:R.open_reference_command([path],line))
            command,excluded=R.open_reference_command([path],line.replace('/run/snapd/ns','/unrelated/unknown'))
            E.require(command==['lsof','-nP','-Fpn','--',str(path)] and not excluded,'unknown mount inspection was suppressed')
            positive('lsof-unknown-mount-preserved')
    with scene() as root:
        spec=importlib.util.spec_from_file_location('retention_reuse_gate',E.ROOT/'scripts/device-mapping-gate.py')
        gate=importlib.util.module_from_spec(spec)
        with patch.dict(os.environ):spec.loader.exec_module(gate)
        origin=root/'backups/origin';(origin/'attempts').mkdir(parents=True)
        static=('diff-check','shell-syntax','inventory')
        retained=('selftest-kernel','physical-boot-normal','unknown-command')
        for name in (*static,*retained):(origin/'attempts'/('20260929T000000.000000Z-'+name)).mkdir()
        selected=gate.reusable_attempts(origin)
        E.require(not any(n.endswith(tuple('-'+s for s in static)) for n in selected),'static commands imported twice')
        positive('reused-static-commands-not-imported')
        E.require(selected==sorted('attempts/20260929T000000.000000Z-'+n for n in retained),
                  'required build/host attempt omitted')
        positive('reused-build-and-host-commands-retained')
    for name,reason,change in (
        ('observation-import-wrong-identity','wrong historical observation identity',
         lambda r:r.update(candidate_id='0'*64)),
        ('observation-import-duplicate-member','closure bounds',lambda r:r['files'].append(dict(r['files'][0]))),
        ('observation-import-missing-member','member inventory differs',lambda r:r['files'][0].update(path='missing')),
        ('observation-import-altered-bytes','historical observation bytes differ',lambda r:r['files'][0].update(sha256='0'*64)),
        ('observation-import-forged-candidate','historical observation candidate differs',lambda r:None)):
        with scene() as root:
            origin=root/'backups/origin';source=origin/'previous-observation';source.mkdir(parents=True)
            (source/'candidate.json').write_text('{}')
            record={'copy':'previous-observation','candidate_id':'ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c',
                'files':[{'path':'candidate.json','size':2,'sha256':E.sha(source/'candidate.json')}]}
            change(record);E.write(origin/'previous-observation.json',record)
            target=root/'backups/new';target.mkdir()
            reject(name,reason,lambda:gate.adopt_observation(target,origin))
            E.require(not list(target.iterdir()),'invalid observation import published output')
    with scene() as root:
        spec=importlib.util.spec_from_file_location('retention_selector',E.ROOT/'scripts/physical-translation-review.py')
        selector=importlib.util.module_from_spec(spec)
        with patch.dict(os.environ):spec.loader.exec_module(selector)
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
            import validation_stages as stages
            evidence=root/'backups/static-proof';evidence.mkdir()
            E.write(evidence/'image-static.json',{'status':'PASS'})
            base=evidence/'stage-revisions/current-static/20261003T000000.000000Z'
            helper=create(root,str((base/'static-vectors/check').relative_to(root)),b'\x7fELFsealed-host-fixture')
            for name in ('static.json','image-static.json'):E.write(base/name,{'status':'PASS'})
            seal={'schema':1,'status':'PASS','build_input_sha256':E.identity(E.manifest()),
                'harness_sha256':E.identity(E.manifest(harness=True)),
                'predecessor_sha256':E.sha(evidence/'image-static.json'),
                'files':[{'path':p.relative_to(base).as_posix(),'size':p.stat().st_size,'sha256':E.sha(p)}
                    for p in sorted(base.rglob('*')) if p.is_file()]}
            seal_path=base/'complete.json';E.write(seal_path,seal);original=helper.read_bytes()
            E.require(selector.sealed_static_host_helper(helper,True) and stages.static_base(evidence)==base,
                'sealed static helper consumer/selection disagrees')
            positive('sealed-static-helper-retained')
            E.require(not selector.sealed_static_host_helper(helper,False) and
                not selector.sealed_static_host_helper(base/'unrelated/check',True),
                'static helper exemption escaped its current sealed role')
            positive('static-helper-contract-boundary')
            helper.write_bytes(original[:-1]+b'?')
            reject('static-helper-byte-substitution','sealed static host helper differs',
                lambda:selector.sealed_static_host_helper(helper,True))
            helper.write_bytes(original)
            member=next(r for r in seal['files'] if r['path']=='static-vectors/check')
            for name,files in (
                ('static-helper-missing-seal-member',[r for r in seal['files'] if r!=member]),
                ('static-helper-duplicate-seal-member',seal['files']+[member])):
                seal_path.write_bytes(E.canonical(dict(seal,files=files)))
                reject(name,'sealed static host helper differs',lambda:selector.sealed_static_host_helper(helper,True))
            for name,data in (('static-helper-format',b'NOTELF'),('static-helper-size-bound',b'\x7fELF'+b'x'*65533)):
                helper.write_bytes(data)
                files=[dict(r,size=len(data),sha256=E.sha(helper)) if r==member else r for r in seal['files']]
                seal_path.write_bytes(E.canonical(dict(seal,files=files)))
                reject(name,'sealed static host helper differs',lambda:selector.sealed_static_host_helper(helper,True))
            helper.write_bytes(original);seal_path.write_bytes(E.canonical(seal))
            relative=helper.relative_to(root).as_posix()
            required=R.kept_manifest(root,[{'path':relative,'consumers':['validation_stages.static_base:sealed raw member']}])
            reject('required-static-helper','retained artifact',lambda:R.plan(root,entries(relative),required))
            helper.unlink()
            reject('static-helper-missing-consumer-input','static successor raw dependency differs',
                lambda:stages.static_base(evidence))
            helper.write_bytes(original)
            E.require(stages.static_base(evidence)==base and selector.sealed_static_host_helper(helper,True),
                'restored static consumer did not return to positive')
            positive('static-helper-restored-consumer-input')
        campaign=root/'backups/campaign';campaign.mkdir()
        E.write(campaign/'regression-selection.json',{'formatting':'regression-attempts/selected-formatting'})
        for name,path,expected in (
            ('campaign-copy-index-not-tree-dependency','reuse.json',True),
            ('formatting-resume-index-not-tree-dependency','formatting-reuse.json',True),
            ('selected-native-copy-index-not-tree-dependency','regression-attempts/selected-formatting/evidence/reuse.json',True),
            ('unknown-reuse-file-remains-dependency','profiles/run/reuse.json',False),
            ('raw-receipt-remains-dependency','profiles/run/result.json',False)):
            E.require(selector.local_reuse_inventory(campaign/path,campaign)==expected,'copy-index dependency boundary')
            positive(name)
        attempt=campaign/'regression-attempts/discarded';attempt.mkdir(parents=True)
        source='1'*64
        E.write(attempt/'started.json',{'requirement':{'key':'formatting'},
            'build_input_sha256':source,'started_ns':1})
        (attempt/'command-0.stdout.log').write_bytes(b'actual failed command output\n')
        E.write(attempt/'command-0.json',{'exit_status':1,'build_input_sha256':source,
            'stdout_sha256':E.sha(attempt/'command-0.stdout.log'),
            'environment':{'FORMAT_EVIDENCE_DIR':str(attempt/'evidence')}})
        outcome={'status':'FAIL','key':'formatting','build_input_sha256':source,
            'commands':['command-0.json'],'finished_ns':2}
        E.write(attempt/'result.json',outcome)
        for name,path,expected in (
            ('discarded-command-not-tree-dependency',attempt/'command-0.json',True),
            ('discarded-outcome-not-tree-dependency',attempt/'result.json',True),
            ('selected-command-remains-dependency',campaign/'regression-attempts/selected-formatting/command-0.json',False),
            ('causal-child-receipt-remains-dependency',attempt/'evidence/raw-result.json',False)):
            E.require(selector.local_reuse_inventory(path,campaign)==expected,'discarded native dependency boundary')
            positive(name)
        (attempt/'result.json').unlink()
        reject('discarded-missing-terminal','discarded native attempt has no terminal outcome',
            lambda:selector.local_reuse_inventory(attempt/'command-0.json',campaign))
        (attempt/'result.json').write_bytes(E.canonical(dict(outcome,status='RUNNING')))
        reject('discarded-invalid-terminal','discarded native outcome is invalid',
            lambda:selector.local_reuse_inventory(attempt/'command-0.json',campaign))
        (attempt/'result.json').write_bytes(E.canonical(outcome))
        (attempt/'command-0.stdout.log').write_bytes(b'changed')
        reject('discarded-command-substitution','discarded native command bytes differ',
            lambda:selector.local_reuse_inventory(attempt/'command-0.json',campaign))
    with scene() as root:
        f=create(root); k=keep(root); p=R.plan(root,entries(str(f.relative_to(root))),k)
        before={q.relative_to(root).as_posix() for q in root.rglob('*') if q.is_file()}
        result=R.apply(root,p,k,root/'backups/journal.jsonl')
        E.require(result['removed_files']==1 and not f.exists() and (root/'source.c').exists(), 'obsolete original survived')
        E.require(not any(q.name.endswith(('.img','.gz')) for q in root.rglob('*')), 'pointless backup created')
        positive('obsolete-original-removed-without-copy')
        history=[json.loads(line) for line in (root/'backups/journal.jsonl').read_text().splitlines()]
        R.verify_journal(p,history,complete=True)
        reject('journal-source-binding','intent identity differs',lambda:R.verify_journal(p,
            [dict(history[0],sha256='0'*64),history[1]],complete=True))
        reject('journal-missing-intent','no matching intent',lambda:R.verify_journal(p,history[1:],complete=True))
        reject('journal-duplicate-outcome','repeats completed',lambda:R.verify_journal(p,history+[history[1]],complete=True))
        reject('journal-missing-terminal','terminal retirement missing',lambda:R.verify_journal(p,history[:1],complete=True))
        reject('journal-block-substitution','block estimate differs',lambda:R.verify_journal(p,
            [history[0],dict(history[1],allocated_estimate=0)],complete=True))
        base=root/'backups';E.write(base/'plan.json',p);E.write(base/'result.json',result)
        report={'schema':1,'status':'APPLIED','proof_files':[
            {'path':n,'size':(base/n).stat().st_size,'sha256':E.sha(base/n)}
            for n in ('plan.json','result.json','journal.jsonl')],
            'batches':[{'plan':'plan.json','result':'result.json','journal':'journal.jsonl','outcome':result}],
            'removed_files':result['removed_files'],'removed_logical_bytes':result['removed_logical_bytes'],
            'released_allocated_estimate':result['released_allocated_estimate'],
            'observed_free_delta_sum':result['observed_free_delta']}
        E.write(base/'report.json',report);R.verify_report(base);positive('retirement-report-replay')
        (base/'report.json').write_bytes(E.canonical(dict(report,removed_files=2)))
        reject('retirement-report-count-substitution','retention aggregate differs',lambda:R.verify_report(base))
        (base/'report.json').write_bytes(E.canonical(report));(base/'result.json').write_text('{}')
        reject('retirement-report-proof-substitution','retention report dependency changed',lambda:R.verify_report(base))
        result=R.apply(root,p,k,root/'backups/journal.jsonl')
        E.require(result['removed_files']==0 and result['already_completed']==1,'resume double counted');positive('idempotent-resume')
    for name in ('source.c','untracked.c','../escape','backups-other/image'):
        with scene() as root:
            create(root,name if '..' not in name else 'backups/safe')
            reject('protected-or-prefix-'+name,'unsafe evidence path' if '..' in name else 'outside generated',
                   lambda: R.plan(root,entries(name),keep(root)))
    with scene() as root:
        f=create(root); k=keep(root,str(f.relative_to(root)))
        reject('retained-raw-only','retained artifact',lambda:R.plan(root,entries(str(f.relative_to(root))),k))
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        for name,path,consumer in (
            ('required-bootstrap-prelaunch','backups/diagnostic/bootstrap-before-private.bin','low_address_evidence.readback:pre-retirement physical table bytes'),
            ('required-null-candidate','backups/candidates/selftest/kernel.elf','low-half-gate.destructive:exact NULL instruction candidate')):
            with scene() as root:
                f=create(root,path);digest=E.sha(f)
                k=R.kept_manifest(root,[{'path':path,'consumers':[consumer]}])
                reject(name,'retained artifact',lambda:R.plan(root,entries(path),k))
                E.require(f.is_file() and E.sha(f)==digest,'required low-domain proof changed during refusal')
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        for name,path,consumer in (
            ('required-historical-stream','backups/preservation/qemu-debugcon.log','device-mapping-replay.repair_identity:retained historical-positive/negative raw stream'),
            ('required-image-table-bytes','backups/diagnostic/table-frames.bin','kernel_image_evidence.physical_tables:actual effective-rights proof'),
            ('required-protection-candidate','backups/candidates/selftest/kernel.elf','kernel-wx-gate.runtime:exact fixed protection instruction candidate')):
            with scene() as root:
                f=create(root,path);digest=E.sha(f)
                k=R.kept_manifest(root,[{'path':path,'consumers':[consumer]}])
                reject(name,'retained artifact',lambda:R.plan(root,entries(path),k))
                E.require(f.is_file() and E.sha(f)==digest,'required current/historical proof changed during refusal')
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
        for name,path,consumer in (
            ('required-stack-owners','backups/diagnostic/stack-owners.bin','stack_guard_evidence.physical_tables:complete stable owner records'),
            ('required-stack-retirement-bytes','backups/diagnostic/lifetime-after.bin','stack_readback.verify_lifetime:actual ACK and PMM bytes before reuse'),
            ('required-stack-candidate','backups/candidates/selftest/kernel.elf','stack-guard-gate.experiments:exact guard load and overflow instructions')):
            with scene() as root:
                f=create(root,path);digest=E.sha(f)
                k=R.kept_manifest(root,[{'path':path,'consumers':[consumer]}])
                reject(name,'retained artifact',lambda:R.plan(root,entries(path),k))
                E.require(f.is_file() and E.sha(f)==digest,'required guarded-stack proof changed during refusal')
    with scene() as root:
        a=create(root,'backups/candidate/image'); b=create(root,'backups/duplicate/image',a.read_bytes()); k=keep(root,'backups/candidate/image')
        p=R.plan(root,entries('backups/duplicate/image'),k);R.apply(root,p,k,root/'backups/journal')
        E.require(a.is_file() and not b.exists(),'duplicate retirement');positive('duplicate-with-authoritative-original')
    with scene() as root:
        f=create(root); k=keep(root); p=R.plan(root,entries(str(f.relative_to(root))),k)
        f.write_bytes(b'changed');reject('changed-bytes','changed since plan',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        f=create(root); k=keep(root); p=R.plan(root,entries(str(f.relative_to(root))),k)
        f.rename(f.with_suffix('.saved'));create(root);reject('replaced-inode','changed since plan',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        (root/'backups/link').symlink_to(root, target_is_directory=True)
        reject('symlink-directory-escape','Not a directory',lambda:R.plan(root,entries('backups/link/source.c'),keep(root)))
    with scene() as root:
        f=create(root); f.unlink(); f.symlink_to(root/'source.c')
        reject('symlink-file','Too many levels',lambda:R.plan(root,entries(str(f.relative_to(root))),keep(root)))
    with scene() as root:
        f=create(root); k=keep(root); p=R.plan(root,entries(str(f.relative_to(root))),k,['backups/retired'])
        create(root,'backups/retired/unlisted');reject('unexpected-directory-child','unexpected directory child',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        f=create(root); k=keep(root); p=R.plan(root,entries(str(f.relative_to(root))),k)
        with f.open('rb'):
            reject('real-open-reader','in-use artifact',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        f=create(root); k=keep(root); e=entries(str(f.relative_to(root)));e[0]['backing_consumers']=['retained-child.qcow2']
        reject('needed-backing-chain','backing dependency',lambda:R.plan(root,e,k))
    with scene() as root:
        f=create(root);os.link(root/'source.c',root/'backups/alias')
        reject('hardlink-source-alias','unsafe hardlink',lambda:R.plan(root,entries('backups/alias'),keep(root)))
    for point in ('before','after'):
        with scene() as root:
            f=create(root);k=keep(root);p=R.plan(root,entries(str(f.relative_to(root))),k)
            def stop(at,row):
                if at==point:raise InterruptedError('synthetic interruption')
            reject('interrupted-'+point,'synthetic interruption',lambda:R.apply(root,p,k,root/'backups/journal',interrupt=stop))
            result=R.apply(root,p,k,root/'backups/journal')
            E.require(not f.exists() and (result['removed_files']==1 if point=='before' else len(result['recovered_interrupted_intents'])==1),'interrupted recovery')
            positive('resumed-'+point)
    with scene() as root:
        spec=importlib.util.spec_from_file_location('retention_mapping_verifier',E.ROOT/'scripts/verify-device-mapping.py')
        native=importlib.util.module_from_spec(spec);spec.loader.exec_module(native)
        f=create(root,'backups/required/commands.json',E.canonical(native.COMMANDS))
        def consumer(path): native.complete_command_order(json.loads(path.read_text()))
        row=R.pack_required(root,str(f.relative_to(root)),root/'backups/store',consumer)
        k=keep(root,row['path']);e=entries(str(f.relative_to(root)));e[0].update(category='compacted-evidence',replacement=row)
        p=R.plan(root,e,k);R.apply(root,p,k,root/'backups/journal')
        restored=root/'backups/restored.json';R.verify_pack(root,row,restored);consumer(restored)
        E.require(not f.exists(),'expanded raw retained');positive('lossless-pack-with-consuming-parser')
        (root/row['path']).write_bytes(b'corrupt')
        reject('corrupt-required-pack','invalid compact reference',lambda:R.verify_pack(root,row))
    with scene() as root:
        f=create(root);k=keep(root);e=entries(str(f.relative_to(root)));e[0]['category']='compacted-evidence'
        reject('missing-durable-replacement','missing/corrupt',lambda:R.plan(root,e,k))
    with scene() as root:
        f=create(root);k=keep(root);p=R.plan(root,entries(str(f.relative_to(root))),k)
        (root/'source.c').write_text('changed required input')
        reject('stale-kept-pointer','retained input mismatch',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        f=create(root);k=keep(root);p=R.plan(root,entries(str(f.relative_to(root))),k)
        p['root']='/foreign';reject('foreign-root','root/kept binding',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        f=create(root);k=keep(root);p=R.plan(root,entries(str(f.relative_to(root))),k)
        with R.exclusion(root,exclusive=False):
            reject('launch-maintenance-exclusion','Resource temporarily unavailable',lambda:R.apply(root,p,k,root/'backups/journal'))
    with scene() as root:
        with patch.dict(os.environ), patch.object(tempfile,'tempdir',None):
            scratch=resources.configure_scratch(root/'backups/scratch')
            child=json.loads(subprocess.check_output([sys.executable,'-c',
                'import os,tempfile,json; print(json.dumps([os.environ["TMPDIR"],os.environ["HOBBYOS_REVIEW_SCRATCH"],tempfile.gettempdir()]))']))
            E.require(child==[scratch]*3,'child scratch differs');positive('nested-scratch-propagation')
        with patch.object(resources,'filesystem',return_value=dict(resources.filesystem(root),device_id=1)):
            evidence=root/'backups/admission';evidence.mkdir()
            result=resources.admit(evidence,stages={'runtime':{'state':'MISSING'}},durable={'runtime':1024},
                temporary=2048,scratch=4096,scratch_path=root,working_path=root,purpose='fixture-same-device')
            E.require(result['filesystems']['1']['required_bytes']==R.RESERVE+1024+2048+4096,
                      'same-device overlap not charged');positive('same-device-overlap-accounting')
            with patch.object(resources,'filesystem',return_value=dict(resources.filesystem(root),device_id=1,free_bytes=R.RESERVE+7167)):
                reject('genuine-working-set-shortage','BLOCKED_STORAGE',lambda:resources.admit(evidence,
                    stages={'runtime':{'state':'MISSING'}},durable={'runtime':1024},temporary=2048,scratch=4096))
    with scene() as root:
        manifest=E.manifest();harness=E.manifest(harness=True)
        store=root/'backups/canonical';store.mkdir()
        artifacts={}
        for name in ('kernel.elf','hobbyos.img','BOOTX64.EFI'):
            p=create(root,'backups/canonical/'+name,name.encode());artifacts[name]={'size':p.stat().st_size,'sha256':E.sha(p)}
        payload=dict(artifacts);payload.pop('hobbyos.img')
        for name,source in [('zap-light16.psf','bootloader/fonts/zap-light16.psf'),('logo.bmp','bootloader/images/logo.bmp'),('startup.nsh','bootloader/startup.nsh')]:
            p=E.ROOT/source;payload[name]={'size':p.stat().st_size,'sha256':E.sha(p)}
        receipt={'schema':2,'status':'PASS','build_input_sha256':E.identity(manifest),'inputs_before':E.identity(manifest),
                 'artifacts':artifacts,'profile':['SYNTHETIC_HOST_ONLY'],
                 'payload':{'status':'PASS','image_sha256':artifacts['hobbyos.img']['sha256'],'files':payload}}
        candidate={'schema':2,'source_sha256':E.identity(manifest),'build_inputs':manifest,'harness_inputs':harness,
            'harness_sha256':E.identity(harness),'artifacts':artifacts,'profile':receipt['profile'],'build_receipt_sha256':E.identity(receipt)}
        E.write(store/'candidate.json',candidate);E.write(store/'build-receipt.json',receipt)
        def verify(path):
            path=candidates.resolve(path);v=json.loads((path/'candidate.json').read_text());r=json.loads((path/'build-receipt.json').read_text())
            identity=E.verify_candidate(v,r)
            E.require(all(E.sha(path/n)==f['sha256'] for n,f in v['artifacts'].items()),'fixture binary differs')
            return identity
        with patch.object(E,'ROOT',root):
            target=root/'backups/successor/candidates/selftest';candidates.adopt(store,target,verify)
            E.require(not (target/'hobbyos.img').exists() and candidates.artifact(target/'hobbyos.img')==store/'hobbyos.img',
                      'canonical candidate copied rather than resolved');positive('candidate-role-reuse-without-binary-copy')
            location=json.loads((target/candidates.LOCATION).read_text());location['candidate_id']='0'*64
            (target/candidates.LOCATION).write_text(json.dumps(location))
            reject('stale-candidate-reference','identity differs',lambda:candidates.resolve(target))
    with scene() as root:
        import base64,struct
        directory=root/'backups/run';directory.mkdir();session=root/'backups/session';runtime=session/'r';runtime.mkdir(parents=True)
        store=root/'backups/canonical';store.mkdir();image=create(root,'backups/canonical/hobbyos.img',b'\0'*65536)
        frozen={'artifacts':{'hobbyos.img':{'size':image.stat().st_size,'sha256':E.sha(image)}}};E.write(store/'candidate.json',frozen)
        create(root,'backups/session/r/working.img',image.read_bytes())
        E.write(session/'session.json',{'kind':'isolated-runtime','parent_evidence':str(directory)})
        E.write(directory/'result.json',{'cleanup':True,'status':'COLLECTED','runtime_session':str(session),
                'canonical_candidate':str(store),'candidate_id':E.identity(frozen),'candidate_image':E.sha(image)})
        E.write(directory/'candidate.json',frozen);E.write(directory/'launch.json',{'synthetic':True})
        for name,data in [('qemu-serial.log',b'test\n'),('qemu-debugcon.log',b'debug\n'),('qemu-trace.log',b''),('launch.env',b'synthetic\n')]:
            create(root,'backups/session/r/'+name,data);create(root,'backups/run/'+name,data)
        create(root,'backups/session/r/serial-receipts.bin',struct.pack('<QI',123,5)+b'test\n')
        E.write(directory/'serial-observations.jsonl',{'offset':0,'length':5,'monotonic_ns':123,'bytes_base64':base64.b64encode(b'test\n').decode()})
        with patch.object(E,'ROOT',root), patch.object(R,'image_info',return_value={'format':'raw'}):
            (directory/'qemu-debugcon.log').write_bytes(b'wrong')
            reject('unsealed-run-stream','unsealed runtime stream',lambda:R.retire_run(directory))
            (directory/'qemu-debugcon.log').write_bytes(b'debug\n');result=R.retire_run(directory)
            E.require(result['removed_files']==6 and image.exists() and not (runtime/'working.img').exists(),'run retirement failed')
            positive('sealed-run-working-copy-retired')
            E.require(R.retire_run(directory)==result,'run retirement not idempotent');positive('retired-run-idempotent')
    with scene() as root:
        image=create(root,'backups/working.img',b'synthetic immutable disk bytes')
        candidate=root/'backups/candidate';candidate.mkdir()
        directory=root/'backups/run';directory.mkdir()
        payloads={'::/kernel.elf':b'kernel','::/EFI/BOOT/BOOTX64.EFI':b'loader',
            '::/EFI/fonts/zap-light16.psf':b'font','::/EFI/images/logo.bmp':b'logo',
            '::/startup.nsh':b'start','::/NvVars':b'firmware variable state'}
        import hashlib
        facts={Path(n).name:{'size':len(b),'sha256':hashlib.sha256(b).hexdigest()}
               for n,b in payloads.items() if n!='::/NvVars'}
        E.write(candidate/'build-receipt.json',{'payload':{'files':facts}})
        listing='\n'.join(payloads)+'\n'
        def copy(argv,**kwargs):
            E.require(argv[:3]==['mcopy','-i',str(image)],'unexpected synthetic disk operation')
            Path(argv[-1]).write_bytes(payloads[argv[-2]])
        with patch.object(R.subprocess,'check_output',return_value=listing),patch.object(R.subprocess,'run',side_effect=copy):
            target=R.seal_firmware_disk(image,directory,candidate)
            E.require((target/'NvVars.bin').read_bytes()==payloads['::/NvVars'] and image.exists(),'firmware data lost')
            positive('firmware-state-sealed')
            E.require(R.seal_firmware_disk(image,directory,candidate)==target,'sealing resume changed location')
            positive('firmware-seal-resumed')
            payloads['::/kernel.elf']=b'changed kernel'
            reject('firmware-payload-change','working boot payload changed',lambda:R.seal_firmware_disk(image,directory,candidate))
            payloads['::/kernel.elf']=b'kernel'
            (target/'NvVars.bin').write_bytes(b'wrong state')
            reject('firmware-staged-change','staged firmware state changed',lambda:R.seal_firmware_disk(image,directory,candidate))
            (target/'NvVars.bin').write_bytes(payloads['::/NvVars'])
        with patch.object(R.subprocess,'check_output',return_value=listing+'::/unknown\n'):
            reject('firmware-unknown-file','unclassified guest disk contents',lambda:R.seal_firmware_disk(image,directory,candidate))
        def changed_copy(argv,**kwargs):
            copy(argv,**kwargs);image.write_bytes(b'concurrently modified disk')
        with patch.object(R.subprocess,'check_output',return_value=listing),patch.object(R.subprocess,'run',side_effect=changed_copy):
            reject('firmware-image-change','working image changed during sealing',lambda:R.seal_firmware_disk(image,directory,candidate))
    result={'schema':1,'status':'PASS','kind':'actual-host-retention','cases':cases,'count':len(cases),
            'build_input_sha256':E.identity(E.manifest()),'harness_sha256':E.identity(E.manifest(harness=True))}
    verify_receipt(result)
    E.write(output,result);print(json.dumps({'status':'PASS','cases':len(cases)}));return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    run(p.parse_args().output)
