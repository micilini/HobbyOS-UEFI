#!/usr/bin/env python3
"""Exercise real admission/resumption and terminal-audit predicates off guest.

Synthetic records test a semantic boundary; they are never runtime evidence.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zlib
from unittest.mock import patch
import memory_evidence as E
import validation_resources as R
import validation_stages as S

ROOT=Path(__file__).resolve().parent.parent
HOST_EPOCH_CASES = set('host-epoch-reset host-epoch-overlap host-epoch-missing '
    'host-epoch-return host-epoch-backwards-utc host-interruption-resume '
    'host-interruption-success host-interruption-wrong-epoch host-interruption-raw '
    'host-interruption-changed-raw host-interruption-missing-raw '
    'host-interruption-wrong-candidate host-interruption-wrong-host '
    'host-epoch-historical-contract'.split())
def module(name):
    spec=importlib.util.spec_from_file_location('resume_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);sys.modules[spec.name]=result;spec.loader.exec_module(result)
    return result

def verify_receipt(result):
    required=set('missing-stage-receipts malformed-receipts-cannot-lower-native-floor verified-small-replay '
        'stale-candidate stale-verifier failed-stage-status wrong-stage-mode raw-failure-mislabeled-pass '
        'missing-raw-stage independent-scratch-exhaustion bounded-small-replay-admitted inode-exhaustion '
        'read-only-target between-boots-reserve-exhaustion mocked-nested-runtime-launch-plan '
        'partial-runtime-summary-not-reusable invalid-series-not-reusable independent-sparse-copy '
        'wrong-build-role wrong-production-role '
        'old-order correct-list-with-old-missing-tail synthetic-complete-terminal-positive '
        'bad-ack-with-valid-synthetic-record-crc restored-positive-after-valid-crc-negative'.split())
    required.update('actual-memory-qemu-arguments-'+name for name in
                    ('tlb-matrix','nx-disabled','xapic','hub-keyboard','framebuffer-reference')+
                    (('high-ram','high-ram-fallback') if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks') else ()))
    mutations=('missing-last-record','duplicate-generation','bad-terminal-ack','hidden-terminal-timeout',
               'wrong-terminal-root','missing-terminal-checkpoint','unrecorded-concurrent-request','maintenance-after-checkpoint')
    required.update(mutations);required.update('restored-positive-after-'+name for name in mutations)
    required.update('runtime-verifier-provenance-replay runtime-replay-wrong-root '
                    'runtime-replay-missing-series runtime-replay-missing-run '
                    'fresh-static-recheck static-recheck-forged-result '
                    'static-recheck-changed-predecessor'.split())
    required.update('runtime-revision-positive runtime-revision-idempotent runtime-revision-changed-facts '
        'runtime-revision-changed-predecessor runtime-revision-missing-raw runtime-revision-stale-source '
        'runtime-revision-failed-replay runtime-revision-restored'.split())
    required.update('completed-native-replay completed-native-failed-command '
        'completed-native-changed-raw completed-native-wrong-origin completed-native-wrong-target '
        'completed-native-missing-proof completed-native-portable-target completed-native-portable-wrong-target'.split())
    required.update('portable-scope-without-git portable-scope-source-bytes '
        'portable-scope-rebound-current-tree portable-scope-baseline-tree '
        'portable-scope-unlisted-source portable-scope-missing-source portable-scope-symlink '
        'portable-scope-unapproved-edit portable-scope-baseline-identity '
        'portable-scope-restored-positive'.split())
    required.update('selector-contract-'+state+suffix for state in ('unset','physical','owned','direct','low')
                    for suffix in ('','-exception'))
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
        required.update(('selector-contract-image','selector-contract-image-exception'))
        required.update(HOST_EPOCH_CASES)
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            required.update(('selector-contract-stack','selector-contract-stack-exception'))
            required.update('stack-runtime-revision-'+suffix for suffix in ('positive','idempotent','changed-facts',
                'changed-predecessor','missing-raw','stale-source','failed-replay','restored'))
        required.update('image-runtime-revision-'+suffix for suffix in ('positive','idempotent','changed-facts',
            'changed-predecessor','missing-raw','stale-source','failed-replay','restored'))
    required.update('completion-correction-positive completion-correction-restored '
        'completion-correction-missing completion-correction-object completion-correction-ancestry '
        'completion-correction-old-bytes completion-correction-linked-input '
        'completion-correction-duplicate completion-correction-diff completion-correction-parent'.split())
    E.require(result['schema']==1 and result['status']=='PASS' and result['base'] and
              result['build_input_sha256']==E.identity(E.manifest()) and
              result['harness_sha256']==E.identity(E.manifest(harness=True)), 'resumption fixture identity/status')
    E.require(len(result['cases'])==len(required) and {r['name'] for r in result['cases']}==required and
              all(r['status']=='PASS' and r['expected'] in ('ACCEPT','REJECT') and
                  (r['expected']=='ACCEPT' or r.get('actual_boundary')) for r in result['cases']),
              'resumption fixture coverage/boundary incomplete')
    return {'status':'PASS','cases':len(required)}


def portable_scope_cases(check, gate, root):
    package=root/'portable-scope';D=gate.D;P=module('package-memory-review')
    direct=os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks')
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
        import stack_current as D
    elif os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='kernel-image-protection':
        import kernel_image_current as D
    elif os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='low-address-isolation':
        import low_current as D
    elif direct:
        import direct_current as D
    if (ROOT/'.git').exists():
        P.export(ROOT,package)
        rows=[]
        for entry in subprocess.check_output(['git','ls-tree','-r','-z',D.BASE],cwd=ROOT).decode().split('\0'):
            if not entry:continue
            fields,name=entry.split('\t');mode,kind,blob=fields.split()
            E.safe(name);E.require(kind=='blob' and mode in ('100644','100755'),'fixture approved source type')
            data=subprocess.check_output(['git','cat-file','blob',blob],cwd=ROOT)
            path=package/'baseline-source'/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(data);path.chmod(int(mode[3:],8))
            rows.append({'path':name,'size':len(data),'sha256':E.sha(path),'mode':mode,'current_blob':blob})
        (package/'preservation').mkdir()
        E.write(package/'preservation/source-manifest.json',{'schema':2,'head':D.BASE,
            'head_tree':D.TREE,'export_tree':D.TREE,'files':rows})
    else:
        package.mkdir();shutil.copytree(ROOT,package/'source')
        shutil.copytree(ROOT.parent/'baseline-source',package/'baseline-source')
        (package/'preservation').mkdir()
        shutil.copyfile(ROOT.parent/'source-manifest.json',package/'source-manifest.json')
        shutil.copyfile(ROOT.parent/'preservation/source-manifest.json',package/'preservation/source-manifest.json')
    expected=D.scope('owned-device-mapping')
    def positive():
        with patch.object(D,'ROOT',package/'source'),patch.object(subprocess,'check_output',
                side_effect=AssertionError('portable scope invoked Git')):
            E.require(D.scope('owned-device-mapping')==expected,'portable/live scope differs')
    check('portable-scope-without-git',positive)
    manifest=package/'source-manifest.json';saved=manifest.read_bytes();current=json.loads(saved)
    path=package/'source'/current['files'][0]['path'];original=path.read_bytes();mode=path.stat().st_mode&0o777
    def rebound(row,path):
        data=path.read_bytes();row.update(size=len(data),sha256=E.sha(path),
            current_blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest())
    path.write_bytes(original+b'\nSynthetic source mutation.\n')
    check('portable-scope-source-bytes',lambda:D.portable_changes(package),'portable scope source bytes/mode')
    changed=copy.deepcopy(current);rebound(changed['files'][0],path);manifest.write_text(json.dumps(changed))
    check('portable-scope-rebound-current-tree',lambda:D.portable_changes(package),'portable scope source tree')
    path.write_bytes(original);manifest.write_bytes(saved)
    baseline=package/'preservation/source-manifest.json';old=baseline.read_bytes();data=json.loads(old)
    prior=package/'baseline-source'/data['files'][0]['path'];prior_bytes=prior.read_bytes()
    prior.write_bytes(prior_bytes+b'\nSynthetic baseline mutation.\n');rebound(data['files'][0],prior)
    baseline.write_text(json.dumps(data))
    check('portable-scope-baseline-tree',lambda:D.portable_changes(package),'portable scope source tree')
    prior.write_bytes(prior_bytes);baseline.write_bytes(old)
    extra=package/'source/scripts/synthetic-unapproved-mapper.py';extra.write_text('# Synthetic scope negative.\n')
    check('portable-scope-unlisted-source',lambda:D.portable_changes(package),'portable scope source inventory')
    extra.unlink();path.unlink()
    check('portable-scope-missing-source',lambda:D.portable_changes(package),'portable scope path/mode')
    path.symlink_to(prior)
    check('portable-scope-symlink',lambda:D.portable_changes(package),'portable scope path/mode')
    path.unlink();path.write_bytes(original);path.chmod(mode)
    extra.write_text('# Synthetic scope negative.\n');extra.chmod(0o644)
    changed=copy.deepcopy(current);row={'path':extra.relative_to(package/'source').as_posix(),'mode':'100644'}
    rebound(row,extra);changed['files'].append(row);changed['export_tree']=P.source_tree(changed['files'])
    manifest.write_text(json.dumps(changed))
    with patch.object(D,'ROOT',package/'source'):
        reason=('unrelated guarded-stack edit' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks' else 'unrelated image protection edits' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='kernel-image-protection' else 'unrelated low-domain edits' if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='low-address-isolation'
                else 'unrelated RAM mapping edits' if direct else 'unrelated device mapping edits')
        check('portable-scope-unapproved-edit',lambda:D.scope('owned-device-mapping'),reason)
    extra.unlink();manifest.write_bytes(saved)
    data=json.loads(old);data['head']='0'*40;baseline.write_text(json.dumps(data))
    check('portable-scope-baseline-identity',lambda:D.portable_changes(package),'portable scope approved baseline')
    baseline.write_bytes(old);check('portable-scope-restored-positive',positive)


def selector_contract_cases(check):
    fixture=module('physical-translation-fixtures');key='HOBBYOS_VALIDATION_CONTRACT'
    def invoke(value,fail):
        with patch.dict(os.environ):
            if value is None:os.environ.pop(key,None)
            else:os.environ[key]=value
            if fail:
                def failed_import(name):
                    os.environ[key]='physical-translation'
                    raise ValueError('synthetic selector import failure')
                with patch.object(fixture,'module',side_effect=failed_import):
                    try:fixture.selector_module()
                    except ValueError as error:
                        E.require(str(error)=='synthetic selector import failure','selector exception changed')
                    else:raise ValueError('selector exception swallowed')
            else:E.require(callable(fixture.selector_module().requirement_source),'actual selector import missing')
            E.require(os.environ.get(key)==value,'selector leaked its historical contract')
    states=(('unset',None),('physical','physical-translation'),
            ('owned','owned-device-mapping'),('direct','verified-ram-direct-map'),('low','low-address-isolation'))
    if os.environ.get(key) in ('kernel-image-protection','guarded-kernel-stacks'):states+=(('image','kernel-image-protection'),)
    if os.environ.get(key)=='guarded-kernel-stacks':states+=(('stack','guarded-kernel-stacks'),)
    for name,value in states:
        check('selector-contract-'+name,lambda value=value:invoke(value,False))
        check('selector-contract-'+name+'-exception',lambda value=value:invoke(value,True))


def completion_correction_cases(check,package):
    replay=module('device-mapping-replay');P=module('package-memory-review')
    source=json.loads((package/'source-manifest.json').read_text());rows=copy.deepcopy(source['files'])
    name='scripts/physical-translation-fixtures.py';path=package/'source'/name
    old=path.read_text()+'\n# Synthetic predecessor for commit-chain validation.\n';data=old.encode()
    selected=next(r for r in rows if r['path']==name)
    selected.update(size=len(data),sha256=hashlib.sha256(data).hexdigest(),
        current_blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest())
    base='1'*40
    def commit(tree,parent):
        raw=f'tree {tree}\nparent {parent}\n\nSynthetic fixture.\n'.encode()
        return raw,hashlib.sha1(b'commit '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
    implementation,parent=commit(P.source_tree(rows),base)
    current,head=commit(source['export_tree'],parent)
    source.update(head=head,head_tree=source['export_tree'])
    (package/'git').mkdir(exist_ok=True);(package/'git/commit-object.txt').write_bytes(current)
    harness=E.manifest(harness=True);entry=next(r for r in harness['files'] if r['path']==name)
    entry.update(size=len(data),sha256=hashlib.sha256(data).hexdigest())
    diff='Synthetic delta; actual old/current blobs establish tree identity.\n'
    proof={'schema':1,'implementation_commit':implementation.decode(),'old_files':[
        {'path':name,'mode':selected['mode'],'utf8':old}],
        'build_input_sha256':E.identity(E.manifest()),'implementation_harness_sha256':E.identity(harness),
        'git_diff':diff,'git_diff_sha256':hashlib.sha256(diff.encode()).hexdigest()}
    closure={'parent':parent,'harness_correction':proof}
    call=lambda c,ancestor=base:replay.completion_parent(package,source,c,ancestor)
    check('completion-correction-positive',lambda:call(closure))
    check('completion-correction-missing',lambda:call({'parent':parent}),'completion correction missing')
    check('completion-correction-ancestry',lambda:call(closure,'2'*40),'completion approved ancestry')
    mutations=[('object',lambda c:c['harness_correction'].update(implementation_commit='invalid'),
                'completion commit object identity'),
        ('old-bytes',lambda c:c['harness_correction']['old_files'][0].update(utf8='altered'),
         'completion implementation tree reconstruction'),
        ('linked-input',lambda c:c['harness_correction']['old_files'][0].update(path='makefile'),
         'completion correction touches linked or unlisted source'),
        ('duplicate',lambda c:c['harness_correction']['old_files'].append(c['harness_correction']['old_files'][0]),
         'completion correction bound'),
        ('diff',lambda c:c['harness_correction'].update(git_diff_sha256='0'*64),'completion supplied Git diff identity'),
        ('parent',lambda c:c.update(parent=base),'completion current parent/tree')]
    for name,mutate,reason in mutations:
        changed=copy.deepcopy(closure);mutate(changed)
        check('completion-correction-'+name,lambda changed=changed:call(changed),reason)
    check('completion-correction-restored',lambda:call(closure))


def host_epoch_cases(check):
    verifier = module('verify-memory-observation')
    old, new = '11111111-1111-1111-1111-111111111111', '22222222-2222-2222-2222-222222222222'
    first = {'status':'PASS','started_ns':900,'finished_ns':950,
        'clock':{'host_boot_id':old,'started_utc':'20261002T150000.000000Z'}}
    second = {'status':'PASS','started_ns':10,'finished_ns':20,
        'clock':{'host_boot_id':new,'started_utc':'20261002T160000.000000Z'}}
    check('host-epoch-reset',lambda:verifier.attempt_order([first,second]))
    same = copy.deepcopy(second);same['clock']['host_boot_id']=old
    check('host-epoch-overlap',lambda:verifier.attempt_order([first,same]),'inconsistent attempt chronology')
    missing = {k:v for k,v in second.items() if k!='clock'}
    check('host-epoch-missing',lambda:verifier.attempt_order([first,missing]),'attempt host clock coverage missing')
    returned=copy.deepcopy(first);returned['clock']['started_utc']='20261002T170000.000000Z'
    check('host-epoch-return',lambda:verifier.attempt_order([first,second,returned]),'host boot epoch order changed')
    backwards=copy.deepcopy(second);backwards['clock']['started_utc']='20261002T140000.000000Z'
    check('host-epoch-backwards-utc',lambda:verifier.attempt_order([first,backwards]),'host boot epoch order changed')
    stopped=copy.deepcopy(first);stopped.update(status='FAIL',finished_ns=None)
    stopped['clock'].update(interrupted=True,recovered_host_boot_id=new,recovered_utc='2026-10-02T15:59:00+00:00')
    check('host-interruption-resume',lambda:verifier.attempt_order([stopped,second]))
    success=copy.deepcopy(stopped);success['status']='PASS'
    check('host-interruption-success',lambda:verifier.attempt_order([success]),'interruption cannot supply a successful completion')
    check('host-interruption-wrong-epoch',lambda:verifier.attempt_order([stopped,same]),'interrupted attempt resumed in wrong host epoch')
    with patch.dict(os.environ,{'HOBBYOS_VALIDATION_CONTRACT':'low-address-isolation'}):
        check('host-epoch-historical-contract',lambda:verifier.attempt_order([first,second]),
              'host clock successor used outside current contract')
    with tempfile.TemporaryDirectory(prefix='interrupted-host-fixture-') as temporary:
        root=Path(temporary)
        start={'candidate_id':'a'*64,'started_ns':900,'started_utc':'20261002T150000.000000Z'}
        E.write(root/'launch.json',{'candidate_id':start['candidate_id'],'host_boot_id':old,'monotonic_ns':901})
        for name in ('boot-receipt.json','qemu-serial.log','qemu-debugcon.log','qemu-trace.log','serial-receipts.bin'):
            (root/name).write_bytes(b'Synthetic interruption fixture.\n')
        row={'status':'INTERRUPTED_HOST_REBOOT','classification':'transport','host_boot_id':old,
            'recovered_host_boot_id':new,'recovered_utc':'2026-10-02T15:59:00+00:00',
            'candidate_id':start['candidate_id'],'processes_alive':False,
            'files':[{'path':p.name,'size':p.stat().st_size,'sha256':E.sha(p)} for p in sorted(root.iterdir())]}
        def invoke(value):
            (root/'interruption.json').write_bytes(E.canonical(value))
            ref={'path':'interruption.json','sha256':E.sha(root/'interruption.json')}
            finish={'status':'FAIL','finished_ns':None,'interruption':ref}
            (root/'result.json').write_bytes(E.canonical(dict(finish,cleanup=True,candidate_id=start['candidate_id'])))
            return verifier.attempt_clock(root,start,finish)
        check('host-interruption-raw',lambda:invoke(row))
        data=(root/'qemu-debugcon.log').read_bytes();(root/'qemu-debugcon.log').write_bytes(b'Changed synthetic bytes')
        check('host-interruption-changed-raw',lambda:invoke(row),'interruption raw dependency changed')
        (root/'qemu-debugcon.log').write_bytes(data)
        missing=copy.deepcopy(row);missing['files']=[r for r in missing['files'] if r['path']!='qemu-debugcon.log']
        check('host-interruption-missing-raw',lambda:invoke(missing),'interruption raw closure incomplete')
        check('host-interruption-wrong-candidate',lambda:invoke(dict(row,candidate_id='b'*64)),
              'interruption is not a failed prior-host attempt')
        check('host-interruption-wrong-host',lambda:invoke(dict(row,recovered_host_boot_id=old)),
              'interruption is not a failed prior-host attempt')


def runtime_revision_cases(check,root,source,harness,prefix='low',tag=''):
    successor=root/('runtime-revision-'+prefix);successor.mkdir()
    payload=successor/'raw.log';payload.write_text('immutable synthetic runtime payload\n')
    original=successor/(prefix+'-runtime.json')
    prior={'schema':1,'status':'PASS','build_input_sha256':source,'harness_sha256':'0'*64,
           'result':{'raw_sha256':E.sha(payload)}}
    E.write(original,prior);original_bytes=original.read_bytes()
    def replay_revision(path):
        def replay(row):
            S.runtime_predecessor(successor,row)
            E.require(E.sha(payload)==row['result']['raw_sha256'],'runtime revision raw bytes differ')
            return {'status':'PASS'}
        return R.classify(path,replay,source=source,harness=harness)
    def revision_ok():
        row=replay_revision(S.runtime_receipt(successor,prefix))
        E.require(row['state']=='REUSABLE',row['reason'])
    check(tag+'runtime-revision-positive',lambda:S.revalidate_runtime(successor,prefix,replay_revision))
    complete=S.runtime_receipt(successor,prefix);complete_bytes=complete.read_bytes()
    E.require(original.read_bytes()==original_bytes,'runtime replay mutated original receipt')
    check(tag+'runtime-revision-idempotent',lambda:S.revalidate_runtime(successor,prefix,replay_revision))
    E.require(S.runtime_receipt(successor,prefix)==complete,'reusable runtime created another revision')
    forged=json.loads(complete_bytes);forged['result']['raw_sha256']='0'*64
    complete.write_bytes(E.canonical(forged))
    check(tag+'runtime-revision-changed-facts',revision_ok,'runtime revision changed captured facts')
    complete.write_bytes(complete_bytes);original.write_bytes(original_bytes+b' ')
    check(tag+'runtime-revision-changed-predecessor',revision_ok,'previous runtime receipt changed')
    original.write_bytes(original_bytes);payload.rename(successor/'retained.log')
    check(tag+'runtime-revision-missing-raw',revision_ok,'No such file')
    (successor/'retained.log').rename(payload)
    stale=root/('runtime-stale-'+prefix);stale.mkdir()
    E.write(stale/(prefix+'-runtime.json'),dict(prior,build_input_sha256='0'*64))
    check(tag+'runtime-revision-stale-source',lambda:S.revalidate_runtime(stale,prefix,replay_revision),
          'runtime revalidation changed linked inputs')
    failed=root/('runtime-failed-'+prefix);failed.mkdir();E.write(failed/(prefix+'-runtime.json'),prior)
    def failed_replay(path):
        return R.classify(path,lambda row:E.require(False,'synthetic runtime semantic failure'),
                          source=source,harness=harness)
    check(tag+'runtime-revision-failed-replay',lambda:S.revalidate_runtime(failed,prefix,failed_replay),
          'current runtime replay failed')
    E.require(not list((failed/'stage-revisions').glob('*/*/complete.json')) and
              S.runtime_receipt(failed,prefix)==failed/(prefix+'-runtime.json'),'failed runtime revision was published')
    check(tag+'runtime-revision-restored',revision_ok)

def run(output,base):
    cases=[]
    def check(name,action,reason=None):
        try:action()
        except (ValueError,KeyError,OSError,AssertionError) as error:
            E.require(reason is not None and reason in str(error),'wrong rejection for '+name+': '+str(error))
            cases.append({'name':name,'status':'PASS','expected':'REJECT','actual_boundary':reason})
        else:
            E.require(reason is None,'unexpected admission: '+name)
            cases.append({'name':name,'status':'PASS','expected':'ACCEPT'})
    gate=module('device-mapping-gate');source=E.identity(E.manifest());harness=E.identity(E.manifest(harness=True))
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):host_epoch_cases(check)
    selector_contract_cases(check)
    with tempfile.TemporaryDirectory(prefix='validation-resumption-') as temporary:
        root=Path(temporary);evidence=root/'evidence';evidence.mkdir()
        native=module('memory-current-regression');attempt=root/'completed-native';attempt.mkdir()
        original={'status':'FAIL','key':'interrupts','reason':'aggregate replay failed',
            'build_input_sha256':source,'commands':['command-0.json'],'finished_ns':10}
        E.write(attempt/'result.json',original);E.write(attempt/'started.json',{'synthetic':True})
        (attempt/'command-0.stdout.log').write_text('complete synthetic collection\n')
        collected={'exit_status':0,'build_input_sha256':source,
            'stdout_sha256':E.sha(attempt/'command-0.stdout.log')}
        E.write(attempt/'command-0.json',collected);proof=attempt/'verification';proof.mkdir()
        (proof/'stdout.log').write_text('[IRQ][EVIDENCE] PASS\n')
        verification={'exit_status':0,'build_input_sha256':source,'started_ns':11,'finished_ns':12,
            'argv':['python3','-B','scripts/verify-interrupt-bringup.py','verify','current'],
            'environment':{'HOBBYOS_TEST_ARTIFACT':str(attempt/'evidence')},
            'stdout_sha256':E.sha(proof/'stdout.log')}
        E.write(proof/'command.json',verification)
        completed={'schema':1,'status':'PASS','build_input_sha256':source,
            'original_sha256':E.sha(attempt/'result.json'),'started_sha256':E.sha(attempt/'started.json'),
            'commands':{'command-0.json':E.sha(attempt/'command-0.json')},'verification':'verification',
            'verification_sha256':E.sha(proof/'command.json')}
        E.write(attempt/'replayed-result.json',completed)
        check('completed-native-replay',lambda:E.require(native.family_result(attempt)['status']=='PASS' and
            json.loads((attempt/'result.json').read_text())==original,'native original was changed'))
        portable=module('memory-portable-replay').Transport.__new__(module('memory-portable-replay').Transport)
        key=str(attempt/'evidence/evidence.json');portable.paths={key:attempt/'evidence/evidence.json'}
        check('completed-native-portable-target',lambda:native.family_result(attempt,portable))
        portable.paths[key]=root/'other/evidence.json'
        check('completed-native-portable-wrong-target',lambda:native.family_result(attempt,portable),
            'completed native replay verification binding')
        (attempt/'command-0.json').write_bytes(E.canonical(dict(collected,exit_status=1)))
        changed=copy.deepcopy(completed);changed['commands']['command-0.json']=E.sha(attempt/'command-0.json')
        (attempt/'replayed-result.json').write_bytes(E.canonical(changed))
        check('completed-native-failed-command',lambda:native.family_result(attempt),
            'completed native replay failed or changed collection')
        (attempt/'command-0.json').write_bytes(E.canonical(collected))
        (attempt/'replayed-result.json').write_bytes(E.canonical(completed))
        (attempt/'command-0.stdout.log').write_text('changed')
        check('completed-native-changed-raw',lambda:native.family_result(attempt),
            'completed native replay failed or changed collection')
        (attempt/'command-0.stdout.log').write_text('complete synthetic collection\n')
        (attempt/'replayed-result.json').write_bytes(E.canonical(dict(completed,original_sha256='0'*64)))
        check('completed-native-wrong-origin',lambda:native.family_result(attempt),
            'completed native replay original/source binding')
        wrong=copy.deepcopy(verification);wrong['environment']['HOBBYOS_TEST_ARTIFACT']=str(root/'another')
        (proof/'command.json').write_bytes(E.canonical(wrong))
        (attempt/'replayed-result.json').write_bytes(E.canonical(dict(completed,verification_sha256=E.sha(proof/'command.json'))))
        check('completed-native-wrong-target',lambda:native.family_result(attempt),
            'completed native replay verification binding')
        (proof/'command.json').unlink()
        check('completed-native-missing-proof',lambda:native.family_result(attempt),'No such file')
        small=shutil._ntuple_diskusage(1024*R.MIB,896*R.MIB,128*R.MIB)
        with patch.object(shutil,'disk_usage',return_value=small):
            check('missing-stage-receipts',lambda:gate.P.storage_admission(evidence),'BLOCKED_STORAGE')
            for name in ('physical-build','physical-runtime','regression-ledger','physical-fixtures'):
                (evidence/(name+'.json')).write_text('{}')
            check('malformed-receipts-cannot-lower-native-floor',lambda:gate.P.storage_admission(evidence),'BLOCKED_STORAGE')
            last=sorted((evidence/'resource-attempts').glob('*/storage.json'))[-1]
            resource=json.loads(last.read_text())
            E.require(resource['admission_floor_bytes']==4*R.GIB and
                      all(r['state']=='INVALID' for r in resource['stages'].values()),'actual malformed stage classification')
        raw=root/'raw.log';raw.write_text('complete immutable synthetic raw\n')
        receipt=root/'stage.json'
        good={'schema':1,'status':'PASS','build_input_sha256':source,'harness_sha256':harness,
              'raw_sha256':E.sha(raw),'mode':'test-only-small-replay'}
        def semantic(row):
            E.require(row['mode']=='test-only-small-replay' and E.sha(raw)==row['raw_sha256'],
                      'synthetic raw predicate failed')
            return {'status':'PASS'}
        def state(expected):
            observed=R.classify(receipt,semantic,source=source,harness=harness)
            E.require(observed['state']==expected,'classification differs: '+str(observed))
        receipt.write_text(json.dumps(good));check('verified-small-replay',lambda:state('REUSABLE'))
        for name,changes,expected in [('stale-candidate',{'build_input_sha256':'0'*64},'STALE'),
            ('stale-verifier',{'harness_sha256':'0'*64},'STALE'),
            ('failed-stage-status',{'status':'FAIL'},'INVALID'),
            ('wrong-stage-mode',{'mode':'runtime'},'INVALID'),
            ('raw-failure-mislabeled-pass',{'raw_sha256':'0'*64},'INVALID')]:
            receipt.write_text(json.dumps(good|changes));check(name,lambda expected=expected:state(expected))
        receipt.write_text(json.dumps(good));raw.rename(root/'retained.log')
        check('missing-raw-stage',lambda:state('PARTIAL'))
        (evidence/'device-runtime.json').write_text(json.dumps(good|{'result':{'status':'PASS'}}))
        partial=S.stage(gate,evidence,'device','runtime')
        E.require(partial['state']=='PARTIAL','partial actual runtime summary incorrectly reused')
        check('partial-runtime-summary-not-reusable',lambda:None)
        runtime_revision_cases(check,root,source,harness)
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
            runtime_revision_cases(check,root,source,harness,'image','image-')
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            runtime_revision_cases(check,root,source,harness,'stack','stack-')
        captured={'series':{'framebuffer-reference':{'series':{'replay_verifier_sha256':'captured'},
            'runs':[{'path':'synthetic','result':{'inherited':{'status':'PASS','candidate_id':'synthetic',
                'profile':'synthetic','vm_id':'one','boot_id':'one','ready_cpus':1,'policy':{'root':4096}}}}]}}}
        current=copy.deepcopy(captured)
        current['series']['framebuffer-reference']['series']['replay_verifier_sha256']='current'
        check('runtime-verifier-provenance-replay',lambda:gate.reconcile_runtime(captured,copy.deepcopy(current)))
        wrong=copy.deepcopy(current);wrong['series']['framebuffer-reference']['runs'][0]['result']['inherited']['policy']['root']=8192
        check('runtime-replay-wrong-root',lambda:gate.reconcile_runtime(captured,wrong),'completed mode raw replay differs')
        check('runtime-replay-missing-series',lambda:gate.reconcile_runtime(captured,{'series':{}}),'device runtime series omitted')
        wrong=copy.deepcopy(current);wrong['series']['framebuffer-reference']['runs']=[]
        check('runtime-replay-missing-run',lambda:gate.reconcile_runtime(captured,wrong),'device runtime receipt omitted run')
        E.write(evidence/'device-test-plan.json',gate.plan())
        static={'schema':1,'status':'PASS','build_input_sha256':source,'harness_sha256':'0'*64,
                'result':S.static_result(gate,evidence,'device')}
        static['result']['scope_contract_sha256']='synthetic-predecessor-scope'
        E.write(evidence/'device-static.json',static);before=(evidence/'device-static.json').read_bytes()
        def static_positive():
            S.revalidate_host_tools(gate,evidence,'device')
            result=S.stage(gate,evidence,'device','static')
            E.require(result['state']=='REUSABLE' and (evidence/'device-static.json').read_bytes()==before,
                      'fresh static recheck did not preserve the predecessor')
        check('fresh-static-recheck',static_positive)
        complete=next((evidence/'stage-revisions/device-static').glob('*/complete.json'))
        saved=complete.read_bytes();value=json.loads(saved);value['result']['guard']={}
        complete.write_text(json.dumps(value))
        def static_accepted():
            result=S.stage(gate,evidence,'device','static')
            E.require(result['state']=='REUSABLE',result['reason'])
        check('static-recheck-forged-result',static_accepted,'static native receipt differs')
        complete.write_bytes(saved);(evidence/'device-static.json').write_bytes(before+b' ')
        check('static-recheck-changed-predecessor',static_accepted,'previous replay receipt changed')
        (evidence/'device-static.json').write_bytes(before)
        portable_scope_cases(check,gate,root)
        completion_correction_cases(check,root/'portable-scope')
        (evidence/'series.json').write_text('{}')
        check('invalid-series-not-reusable',lambda:S.series(gate.G,evidence,'tlb-matrix',root/'candidate',None),
              'nonreusable series requires a new successor destination')
        original=root/'retained.log';clone=root/'independent.log'
        R.sparse_copy(original,clone);clone.write_text('independent changed copy\n')
        E.require(original.read_text()=='complete immutable synthetic raw\n','candidate copy shared original inode')
        check('independent-sparse-copy',lambda:None)
        S.candidate_role(['DEBUG_ASSERT=0','SELFTEST=0'],'production')
        check('wrong-build-role',lambda:S.candidate_role(['DEBUG_ASSERT=1','SELFTEST=1','SELFTEST_AUTORUN=0'],'production'),
              'candidate role/options mismatch')
        production=[{'profile':p,'production':True,'candidate_id':'synthetic-candidate',
                     'vm_id':str(i),'boot_id':str(i)} for i,p in enumerate(('q35-tcg-smp1','q35-kvm-smp4'))]
        S.production_roles(production,'synthetic-candidate');production[0]['production']=False
        check('wrong-production-role',lambda:S.production_roles(production,'synthetic-candidate'),
              'production role/profile/boot identity mismatch')
        states={'native':{'state':'REUSABLE'}}
        fs=R.filesystem(evidence)
        def capacity(path):
            return dict(fs,requested=str(path),resolved=str(path),device_id=2 if str(path).endswith('scratch') else 1,
                        free_bytes=128*R.MIB if str(path).endswith('scratch') else 8*R.GIB)
        with patch.object(R,'filesystem',side_effect=capacity):
            check('independent-scratch-exhaustion',lambda:R.admit(evidence,stages=states,durable={'native':0},
                scratch=256*R.MIB,scratch_path=root/'scratch'),'BLOCKED_STORAGE')
            check('bounded-small-replay-admitted',lambda:R.admit(evidence,stages=states,durable={'native':0}))
        with patch.object(R,'filesystem',return_value=dict(fs,free_inodes=0)):
            check('inode-exhaustion',lambda:R.before_step(evidence,'runtime'),'BLOCKED_STORAGE')
        with patch.object(R,'filesystem',return_value=dict(fs,read_only=True)):
            check('read-only-target',lambda:R.before_step(evidence,'runtime'),'BLOCKED_STORAGE')
        with patch.object(R,'filesystem',return_value=dict(fs,free_bytes=511*R.MIB)):
            check('between-boots-reserve-exhaustion',lambda:R.before_step(evidence,'runtime'),'BLOCKED_STORAGE')
        calls=[]
        def capture(directory,**kw):
            calls.append((kw['scope'],kw['commands'],kw.get('validation')))
        with patch.object(gate.G,'runtime',side_effect=capture),patch.object(gate.T,'collect_baseline'),\
             patch.object(gate.T,'failure_experiments'),patch.object(gate.T,'verify_runtime',return_value={}),\
             patch.object(gate.P,'selected_runs',return_value=[]),patch.object(gate,'verify_runtime',return_value={}),\
             patch.object(gate.T,'receipt'):
            gate.runtime(evidence)
        expected_scopes=['tlb-matrix','nx-disabled','xapic','hub-keyboard']
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):expected_scopes+=['high-ram','high-ram-fallback']
        expected_scopes+=['framebuffer-reference']
        E.require([r[0] for r in calls]==expected_scopes,
                  'actual nested launch scope inventory')
        E.require(all(commands==gate.V.COMMANDS and validator==gate.V.verify_complete_run
                      for _,commands,validator in calls[:-1]),'complete predicate/command plan lost in actual gate chain')
        check('mocked-nested-runtime-launch-plan',lambda:None)
        for scope,commands,_ in calls:
            arguments=gate.G.runtime_command_arguments(commands,scope,'file')
            selected=['rammap high' if command=='rammap' and scope in ('high-ram','high-ram-fallback') else command for command in commands]
            expected=[value for command in selected for value in ('--command',command)]
            if scope in ('high-ram','high-ram-fallback'):expected+=['--ram-profile','high' if scope=='high-ram' else 'high-fallback']
            if scope=='hub-keyboard':expected+=['--device-profile','hub-keyboard']
            expected+=['--serial-backend','file']
            E.require(arguments==expected,'actual collector command arguments differ')
            check('actual-memory-qemu-arguments-'+scope,lambda:None)
    if base is not None:
        V=gate.V;T=module('verify-tlb-shootdown');D=module('device_mapping_evidence');O=module('verify-memory-observation')
        serial=(base/'qemu-serial.log').read_text().splitlines()
        transactions=[json.loads(l) for l in (base/'framed.jsonl').read_text().splitlines()]
        blocks=T.trace_blocks(base);audits=[];cycles=[]
        for row in transactions:
            segment=O.FOUNDATION.transaction_segment(serial,row)
            if row['payload']=='tlbtest':audits.append(T.rows(T.bind_trace(segment,blocks[len(audits)])))
            if row['payload']=='mappingtest':
                cycles.append([D.fields(l,'[MAPPING][UNMAP] ') for l in segment if l.startswith('[MAPPING][UNMAP] ')])
        mapping=D.mapping_rows(D.split_traces((base/'qemu-debugcon.log').read_bytes())['MAPPING'][0])
        views=mapping['VIEW'];root=T.n(audits[0]['AUDIT'][0],'root');targets=sum(1<<n for n in V.topology(serial))
        protection=None
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('kernel-image-protection','guarded-kernel-stacks'):
            import kernel_image_evidence as image
            plan=image.elf_plan(base);active=image.RAM.activation(serial)
            raw=D.split_traces((base/'qemu-debugcon.log').read_bytes())
            ram=image.RAM.trace(raw['RAM_MAP'][0],serial,active,image_plan=plan)
            protection=image.verify_directory(base,ram)
        stack_services=None
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='guarded-kernel-stacks':
            import stack_guard_evidence as stack
            stack_services=stack.maintenance(base,audits,root,V.topology(serial))
        call=lambda data,proof=protection,stack_proof=stack_services:V.terminal_chronology(data,cycles,views,root,targets,proof,stack_proof)
        check('old-order',lambda:V.complete_command_order(V.WORKLOAD_CAPTURE_COMMANDS),
              'final mapping maintenance follows the last service audit')
        synthetic=copy.deepcopy(audits);second=synthetic[1];checkpoint=second['AUDIT'][0]
        count=2*len(cycles[1]);tail_stack=copy.deepcopy(stack_services)
        if [row['payload'] for row in transactions]==V.WORKLOAD_CAPTURE_COMMANDS:
            missing_tail=audits
            before=T.n(checkpoint,'before');extra=[]
            for cycle in cycles[1]:
                pages=D.number(cycle,'backing')//4096;first=D.number(cycle,'va')&~4095
                for _ in range(2):
                    row=copy.deepcopy(synthetic[0]['SERVICE'][-1])
                    row.update(generation=str(before+len(extra)+1),operation='1',mapping='1',first=hex(first),
                               last=hex(first+(pages-1)*4096),pages=str(pages),strategy='1')
                    extra.append(row)
            for kind in ('REQUEST','SERVICE'):
                for row in second[kind]:row['generation']=str(T.n(row,'generation')+len(extra))
            second['SERVICE']=extra+second['SERVICE']
            checkpoint['before']=checkpoint['completed_before']=str(before+len(extra))
            checkpoint['after']=str(T.n(checkpoint,'after')+len(extra))
        else:
            V.complete_command_order([row['payload'] for row in transactions]);call(audits)
            missing_tail=copy.deepcopy(audits);tail=missing_tail[1];count=2*len(cycles[1])
            if stack_services is None:
                tail['SERVICE']=tail['SERVICE'][count:]
                renumber=lambda generation:generation-count
            else:
                # Remove the actual mapping prelude, preserving the independent
                # stack requests and compacting only this labeled synthetic copy.
                removed={T.n(row,'generation') for row in tail['SERVICE']
                    if T.n(row,'generation')<=T.n(checkpoint,'before') and
                    T.n(row,'generation') not in stack_services and T.n(row,'operation')==1}
                E.require(len(removed)==count,'stack fixture mapping prelude selection')
                renumber=lambda generation:generation-sum(old<generation for old in removed)
                tail['SERVICE']=[row for row in tail['SERVICE'] if T.n(row,'generation') not in removed]
                tail_stack={renumber(generation):copy.deepcopy(event) for generation,event in stack_services.items()}
                for generation,event in tail_stack.items():event['row']['request']=str(generation)
            for kind in ('REQUEST','SERVICE'):
                for row in tail[kind]:row['generation']=str(renumber(T.n(row,'generation')))
            for key in ('before','completed_before','after'):
                tail['AUDIT'][0][key]=str(T.n(tail['AUDIT'][0],key)-count)
        tail_proof=copy.deepcopy(protection)
        if tail_proof is not None:
            # Keep this labeled synthetic probe generation consistent with the
            # renumbered audit so the missing mapping lifetime is the boundary.
            tail_proof['collections'][1]['generation']-=count
            tail_proof['collections'][1]['service']['generation']=str(tail_proof['collections'][1]['generation'])
        check('correct-list-with-old-missing-tail',lambda:call(missing_tail,tail_proof,tail_stack),
              'mapping lifetimes or device prelude missing from terminal audit')
        check('synthetic-complete-terminal-positive',lambda:call(synthetic))
        mutations=[('missing-last-record',lambda d:d[1]['SERVICE'].pop(),'terminal service sequence'),
            ('duplicate-generation',lambda d:d[1]['SERVICE'].append(d[1]['SERVICE'][-1]),'terminal service sequence'),
            ('bad-terminal-ack',lambda d:d[1]['SERVICE'][-1].update(ack='0'),'unsuccessful completion'),
            ('hidden-terminal-timeout',lambda d:d[1]['SERVICE'][-1].update(status='1'),'unsuccessful completion'),
            ('wrong-terminal-root',lambda d:d[1]['AUDIT'][0].update(root='0'),'checkpoint identity'),
            ('missing-terminal-checkpoint',lambda d:d[1]['AUDIT'].clear(),'checkpoint missing'),
            ('unrecorded-concurrent-request',lambda d:d[1]['REQUEST'].pop(),'unexplained concurrent request'),
            ('maintenance-after-checkpoint',lambda d:next(row for row in d[1]['SERVICE'] if
                T.n(row,'generation')==T.n(d[1]['REQUEST'][-1],'generation')).update(mapping='1'),'unexplained maintenance')]
        for name,mutation,reason in mutations:
            changed=copy.deepcopy(synthetic);mutation(changed)
            check(name,lambda changed=changed:call(changed),reason)
            check('restored-positive-after-'+name,lambda:call(synthetic))
        changed=copy.deepcopy(synthetic)
        row=changed[1]['SERVICE'][-1];row['ack']='0'
        def encoded(kind,fields):
            line='[TLB]['+kind+'] '+' '.join(k+'='+v for k,v in fields.items())
            return line+f' crc={zlib.crc32(line.encode()):08x}'
        decoded=T.rows([encoded('BEGIN',changed[1]['BEGIN'][0]),encoded('SERVICE',row)])
        changed[1]['SERVICE'][-1]=decoded['SERVICE'][0]
        check('bad-ack-with-valid-synthetic-record-crc',lambda:call(changed),'unsuccessful completion')
        check('restored-positive-after-valid-crc-negative',lambda:call(synthetic))
    result={'schema':1,'status':'PASS','build_input_sha256':source,'harness_sha256':harness,'cases':cases,
            'authority':'Actual host admission/gate/predicate calls; synthetic terminal positive is not guest acceptance.',
            'base':{name:E.sha(base/name) for name in ('qemu-serial.log','qemu-debugcon.log','framed.jsonl')} if base else None}
    if base is not None:verify_receipt(result)
    E.write(output,result);print(json.dumps({'status':'PASS','cases':len(cases),'output':str(output)}))
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--base',type=Path)
    args=parser.parse_args();run(args.output,args.base)
