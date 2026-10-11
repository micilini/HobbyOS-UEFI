#!/usr/bin/env python3
"""Reconstruct immutable owned-mapping proof and replay every current gate."""
import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import memory_evidence as E
import review_transport as R
import device_current as D
import validation_resources as resources
import validation_retention as retention
ROOT=Path(__file__).resolve().parent.parent
CONTRACT=os.environ.get('HOBBYOS_VALIDATION_CONTRACT','owned-device-mapping')
E.require(CONTRACT in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'),'unknown owned-memory replay contract')
STACK=CONTRACT=='guarded-kernel-stacks'
IMAGE=STACK or CONTRACT=='kernel-image-protection'
LOW=IMAGE or CONTRACT=='low-address-isolation'
DIRECT=LOW or CONTRACT=='verified-ram-direct-map'
os.environ['HOBBYOS_VALIDATION_CONTRACT']=CONTRACT
os.environ['PYTHONDONTWRITEBYTECODE']='1';sys.dont_write_bytecode=True

def module(name):
    spec=importlib.util.spec_from_file_location('owned_portable_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value

def completion_parent(package, source, closure, base):
    """Reconstruct each preserved commit; the stack successor admits three corrections."""
    def commit(data, expected):
        E.require(len(data)<=16384 and hashlib.sha1(b'commit '+str(len(data)).encode()+b'\0'+data).hexdigest()==expected,
                  'completion commit object identity')
        header=data.decode().split('\n\n',1)[0].splitlines()
        trees=[r[5:] for r in header if r.startswith('tree ')]
        parents=[r[7:] for r in header if r.startswith('parent ')]
        E.require(len(trees)==len(parents)==1,'completion commit ancestry')
        return trees[0],parents[0]
    tree,parent=commit((package/'git/commit-object.txt').read_bytes(),source['head'])
    E.require(tree==source['head_tree'] and parent==closure['parent'],'completion current parent/tree')
    proof=closure.get('harness_correction')
    if parent==base:
        E.require(proof is None,'unexpected completion correction')
        return
    rows={r['path']:dict(r) for r in source['files']};build=E.manifest();depth=0
    while parent!=base:
        depth+=1
        E.require(depth<=(3 if STACK else 1),'completion correction chain bound')
        E.require(isinstance(proof,dict) and proof.get('schema')==1,
            'completion correction missing' if depth==1 else 'completion approved ancestry')
        old_tree,old_parent=commit(proof['implementation_commit'].encode(),parent)
        if not STACK:E.require(old_parent==base,'completion approved ancestry')
        changes=proof['old_files'];names=[r['path'] for r in changes]
        E.require(0<len(names)<=8 and len(set(names))==len(names) and
            sum(len(r['utf8'].encode()) for r in changes)<=2*1024*1024,'completion correction bound')
        for r in changes:
            name=r['path'];E.safe(name);data=r['utf8'].encode()
            E.require(name in rows and name.startswith(('scripts/','docs/memory/')) and
                name not in {f['path'] for f in build['files']} and r['mode'] in ('100644','100755'),
                'completion correction touches linked or unlisted source')
            digest=hashlib.sha256(data).hexdigest()
            E.require(digest!=rows[name]['sha256'] or r['mode']!=rows[name]['mode'],
                      'completion correction unchanged file')
            rows[name].update(size=len(data),sha256=digest,mode=r['mode'],
                current_blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest())
        E.require(module('package-memory-review').source_tree(list(rows.values()))==old_tree,
                  'completion implementation tree reconstruction')
        for r in build['files']:
            old=rows[r['path']]
            E.require((old['size'],old['sha256'],old['mode'])==(r['size'],r['sha256'],r['git_mode']),
                      'completion linked-input equality')
        harness=E.manifest(harness=True)
        for r in harness['files']:
            old=rows[r['path']];r.update(size=old['size'],sha256=old['sha256'],git_mode=old['mode'])
        E.require(proof['build_input_sha256']==E.identity(build) and
            proof['implementation_harness_sha256']==E.identity(harness),'completion input identities')
        diff=proof['git_diff'].encode()
        E.require(0<len(diff)<=2*1024*1024 and hashlib.sha256(diff).hexdigest()==proof['git_diff_sha256'],
                  'completion supplied Git diff identity')
        parent=old_parent;proof=proof.get('previous')
    E.require(proof is None,'unexpected trailing completion correction')

def completion_correction_fixtures(package,source,closure,base):
    """Exercise the real chain verifier using bounded, explicitly synthetic metadata."""
    proof=closure.get('harness_correction')
    E.require(STACK and isinstance(proof,dict) and isinstance(proof.get('previous'),dict) and
        isinstance(proof['previous'].get('previous'),dict) and proof['previous']['previous'].get('previous') is None,
        'completion fixtures require the three current host corrections')
    completion_parent(package,source,closure,base)
    cases=[]
    mutations={
        'missing-previous':'completion approved ancestry',
        'trailing-previous':'unexpected trailing completion correction',
        'wrong-parent':'completion current parent/tree',
        'wrong-current-parent-object':'completion commit object identity',
        'wrong-implementation-object':'completion commit object identity',
        'linked-file-change':'completion correction touches linked or unlisted source',
        'wrong-old-mode':'completion correction touches linked or unlisted source',
        'wrong-intermediate-tree':'completion implementation tree reconstruction',
        'wrong-intermediate-harness':'completion input identities',
        'wrong-implementation-harness':'completion input identities',
        'wrong-diff-digest':'completion supplied Git diff identity',
        'duplicate-old-file':'completion correction bound',
    }
    for name,boundary in mutations.items():
        changed=copy.deepcopy(closure);current=changed['harness_correction'];previous=current['previous']
        if name=='missing-previous':del current['previous']
        elif name=='trailing-previous':previous['previous']['previous']=copy.deepcopy(previous)
        elif name=='wrong-parent':changed['parent']='0'*40
        elif name=='wrong-current-parent-object':current['implementation_commit']+='synthetic change\n'
        elif name=='wrong-implementation-object':previous['previous']['implementation_commit']+='synthetic change\n'
        elif name=='linked-file-change':current['old_files'][0]['path']='kernel/src/core/task.h'
        elif name=='wrong-old-mode':current['old_files'][0]['mode']='100600'
        elif name=='wrong-intermediate-tree':current['old_files'][0]['utf8']+='\n# synthetic tree mismatch\n'
        elif name=='wrong-intermediate-harness':current['implementation_harness_sha256']='0'*64
        elif name=='wrong-implementation-harness':previous['previous']['implementation_harness_sha256']='0'*64
        elif name=='wrong-diff-digest':current['git_diff_sha256']='0'*64
        elif name=='duplicate-old-file':current['old_files'].append(copy.deepcopy(current['old_files'][0]))
        try:completion_parent(package,source,changed,base)
        except ValueError as error:
            reason=str(error);E.require(boundary in reason,'completion fixture unrelated rejection: '+name+': '+reason)
        else:raise ValueError('completion contradiction accepted: '+name)
        cases.append({'name':name,'expected_boundary':boundary,'actual_reason':reason,'synthetic':True})
    with tempfile.TemporaryDirectory(prefix='completion-depth-negative-') as tmp:
        work=Path(tmp);(work/'git').mkdir();changed=copy.deepcopy(closure)
        def reparent(text,parent):
            header,body=text.split('\n\n',1);lines=header.splitlines()
            E.require(sum(line.startswith('parent ') for line in lines)==1,'completion synthetic parent cardinality')
            data=('\n'.join('parent '+parent if line.startswith('parent ') else line for line in lines)+'\n\n'+body).encode()
            return data,hashlib.sha1(b'commit '+str(len(data)).encode()+b'\0'+data).hexdigest()
        first=changed['harness_correction'];second=first['previous'];third=second['previous']
        data,identity=reparent(third['implementation_commit'],'1'*40)
        third['implementation_commit']=data.decode();third['previous']={'schema':1}
        for item in (second,first):
            data,identity=reparent(item['implementation_commit'],identity);item['implementation_commit']=data.decode()
        changed['parent']=identity
        data,identity=reparent((package/'git/commit-object.txt').read_text(),identity)
        (work/'git/commit-object.txt').write_bytes(data)
        try:completion_parent(work,dict(source,head=identity),changed,base)
        except ValueError as error:
            reason=str(error);E.require(reason=='completion correction chain bound','completion depth fixture unrelated rejection')
        else:raise ValueError('completion excessive correction chain accepted')
        cases.append({'name':'chain-too-deep','expected_boundary':'completion correction chain bound',
            'actual_reason':reason,'synthetic':True,'outer_metadata':'SYNTHETIC_COMMIT_IDENTITIES_REBOUND'})
    # Reconstruct every actual earlier source and retain all historical paths.
    rows={r['path']:dict(r) for r in source['files']};current=proof;positive=1
    with tempfile.TemporaryDirectory(prefix='completion-chain-controls-') as tmp:
        work=Path(tmp);(work/'git').mkdir()
        while current is not None:
            commit=current['implementation_commit'].encode();head=hashlib.sha1(b'commit '+str(len(commit)).encode()+b'\0'+commit).hexdigest()
            header=commit.decode().split('\n\n',1)[0].splitlines()
            tree=next(row[5:] for row in header if row.startswith('tree '));parent=next(row[7:] for row in header if row.startswith('parent '))
            for old in current['old_files']:
                data=old['utf8'].encode();rows[old['path']].update(size=len(data),sha256=hashlib.sha256(data).hexdigest(),mode=old['mode'],
                    current_blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest())
            (work/'git/commit-object.txt').write_bytes(commit)
            earlier=dict(source,head=head,head_tree=tree,files=list(rows.values()))
            prior={'parent':parent,'harness_correction':current.get('previous')}
            completion_parent(work,earlier,prior,base);positive+=1;current=current.get('previous')
    completion_parent(package,source,closure,base);positive+=1
    return {'status':'PASS','positive':positive,'negative':len(cases),'cases':cases,
        'authority':'ACTUAL_COMMIT_TREE_AND_LINKED_INPUT_CHECKER_WITH_SYNTHETIC_METADATA_NEGATIVES'}

def repair_identity(package):
    saved=json.loads((package/'preservation/repair-source-manifest.json').read_text());rows=[]
    E.require(saved['head']==D.BASE and saved['head_tree']==D.TREE and
        saved['export_tree']=='72d484becfd86d89c86cfad8b294e75b1a6dd0ab' and not saved['source_clean'],
        'reviewed repair starting identity')
    root=package/'preservation/repair-source'
    for r in saved['files']:
        E.safe(r['path']);path=root/r['path'];b=path.read_bytes();blob=hashlib.sha1(b'blob '+str(len(b)).encode()+b'\0'+b).hexdigest()
        E.require(len(b)==r['size'] and E.sha(path)==r['sha256'] and blob==r['current_blob'] and
            path.stat().st_mode&0o777==int(r['mode'][3:],8),'repair starting bytes/modes changed')
        rows.append({'path':r['path'],'mode':r['mode'],'current_blob':blob})
    E.require({r['path'] for r in rows}=={p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()} and
        module('package-memory-review').source_tree(rows)==saved['export_tree'],'repair source reconstruction incomplete')
    controls=json.loads((package/'preservation/hub-controls.json').read_text())
    E.require(controls['status']=='HISTORICAL_FAILED_CONTROLS','historical hub status changed')
    for r in controls['files']:
        E.safe(r['path']);path=package/'preservation/hub-controls'/r['path']
        E.require(path.stat().st_size==r['size'] and E.sha(path)==r['sha256'],'original hub control bytes changed')
    for name in ('mapped-blocked','approved-predecessor'):
        root=package/'preservation/hub-controls'/name
        result=json.loads((root/'result.json').read_text());serial=(root/'qemu-serial.log').read_text()
        E.require(result['status']=='FAIL' and 'CC=5' in serial and 'Product QEMU USB Hub' in
            (root/'usb-topology.stdout.log').read_text(),'historical hub failure evidence incomplete')
        module('device-mapping-partial-replay').hub_fields(json.loads((root/'launch.json').read_text()),
            result,serial,(root/'usb-topology.stdout.log').read_text(),
            [json.loads(line) for line in (root/'framed.jsonl').read_text().splitlines()])
    return {'repair_tree':saved['export_tree'],'source_files':len(rows),'historical_hub_controls':2}

def replay(package,host):
    os.chdir(ROOT);M=module('memory-portable-replay');transport=M.Transport(package);transport.verify_source_identity()
    prior=module('physical-translation-portable-replay')
    if STACK:
        import stack_current
        prior.P=stack_current
    elif IMAGE:
        import kernel_image_current
        prior.P=kernel_image_current
    elif LOW:
        import low_current
        prior.P=low_current
    elif DIRECT:
        import direct_current
        prior.P=direct_current
    else:prior.P=D
    os.environ['HOBBYOS_VALIDATION_CONTRACT']=CONTRACT
    changed=prior.source_identity(package);source_fixtures=prior.source_fixtures(package);repair=repair_identity(package)
    retired=retention.verify_report(package/'preservation/retention')
    campaign=package/'evidence/current-campaign';G=module('stack-guard-gate' if STACK else 'kernel-wx-gate' if IMAGE else 'low-half-gate' if LOW else 'direct-map-gate' if DIRECT else 'device-mapping-gate')
    historical_copy=module('low-half-gate').V.historical_copy(package/'preservation/copy-baseline') if LOW else None
    accepted_name='stack-acceptance.json' if STACK else 'image-acceptance.json' if IMAGE else 'low-acceptance.json' if LOW else 'direct-acceptance.json' if DIRECT else 'device-acceptance.json'
    source=json.loads((package/'source-manifest.json').read_text())
    completion_fixtures=None
    if source['source_clean']:
        closure=json.loads((package/'preservation/closure.json').read_text())
        completion_parent(package,source,closure,prior.P.BASE)
        if STACK and isinstance(closure.get('harness_correction'),dict) and closure['harness_correction'].get('previous') is not None:
            completion_fixtures=completion_correction_fixtures(package,source,closure,prior.P.BASE)
        E.require(closure['status']=='PASS' and closure['head']==source['head'] and closure['tree']==source['head_tree'] and
            closure['branch']=='feat/memory-hardening' and closure['git_status']=='' and
            closure['build_input_sha256']==E.identity(E.manifest()) and closure['harness_sha256']==E.identity(E.manifest(harness=True)) and
            closure['current_acceptance_receipt_sha256']==E.sha(campaign/accepted_name) and
            closure['owned_live_processes']==[] and closure['network_git_operations']==[] and
            closure['hardware']=='DEFERRED_TO_FINAL_CLOSURE' and closure['next_increment']=='NOT_STARTED','owned closure/source contradiction')
    native=M.selected_native(campaign,transport)
    E.require(set(native)=={r['key'] for r in module('memory-current-regression').inventory()['requirements']},'owned native family missing')
    native_fixtures=M.native_fixtures(campaign,transport);inherited=M.current_fixtures(campaign,transport,host_tests=host)
    derived=G.verify(campaign,transport);accepted=json.loads((campaign/accepted_name).read_text())
    E.require(accepted['status']=='PASS' and accepted['build_input_sha256']==E.identity(E.manifest()) and
        accepted['harness_sha256']==E.identity(E.manifest(harness=True)) and accepted['result']==derived,'owned acceptance differs from full replay')
    with tempfile.TemporaryDirectory(prefix='owned-review-fixtures-') as temp:
        dest=Path(temp)
        for kind,script in [('tlb','tlb-shootdown-fixtures'),('physical','physical-translation-fixtures'),('device','device-mapping-fixtures')]+([('direct','direct-map-fixtures')] if DIRECT else [])+([('low','low-half-fixtures')] if LOW else [])+([('image','kernel-wx-fixtures')] if IMAGE else [])+([('stack','stack-guard-fixtures')] if STACK else []):
            receipt=json.loads((campaign/(kind+'-fixtures.json')).read_text())['result']
            expected=json.loads((campaign/receipt['path']/'results.json').read_text())
            actual=module(script).run(campaign,dest/kind)
            E.require(actual==expected,'owned regenerated fixture differs: '+kind)
        receipt=json.loads((campaign/'device-fixtures.json').read_text())['result']
        E.safe(receipt['resumption_base'])
        expected=json.loads((campaign/receipt['path']/'resumption.json').read_text())
        with contextlib.redirect_stdout(io.StringIO()):
            actual=module('validation-resumption-fixtures').run(dest/'resumption.json',campaign/receipt['resumption_base'])
        E.require(actual==expected,'owned resumption/terminal fixture replay differs')
        expected=json.loads((campaign/receipt['path']/'retention.json').read_text())
        with contextlib.redirect_stdout(io.StringIO()):
            actual=module('validation-retention-fixtures').run(dest/'retention.json')
        E.require(actual==expected,'owned retention/lifecycle fixture replay differs')
        policy=module('cpu-control-fixtures').run(campaign,dest/'policy',package/'observation-source')
        receipt=json.loads((campaign/'tlb-fixtures.json').read_text())['result']
        E.require(policy==json.loads((campaign/receipt['path']/'policy/results.json').read_text()),'owned policy negative replay differs')
        if host:
            (dest/'host').mkdir()
            with contextlib.redirect_stdout(io.StringIO()):G.host(dest/'host')
            owned=G.DEVICE if IMAGE else G.D.D if LOW else G.D if DIRECT else G
            owned.T.verify_host(dest/'host',True);owned.P.verify_host(dest/'host',True);owned.verify_host(dest/'host')
            if DIRECT:G.verify_host(dest/'host')
    return {'current_acceptance':'PASS','complete_portable_acceptance':'PASS','independent_review':'PENDING',
        **({'completion_correction_fixtures':completion_fixtures} if completion_fixtures is not None else {}),
        **({'guarded_stack_complete_boots':39,'stack_overflow_boots':2,
            'stack_timeout_boots':1,'stack_taskman_peak_boots':2} if STACK else {}),
        'image_protection_complete_boots':39 if IMAGE else 0,
        'low_root_complete_boots':39 if LOW else 0,'null_destructive_boots':2 if LOW else 0,
        'high_ram_one_gib_boots':3 if DIRECT else 0,'high_ram_fallback_boots':3 if DIRECT else 0,
        'normal_boots':21,'nx_disabled_boots':3,'xapic_boots':3,'sole_hub_keyboard_boots':6,'wb_reference_boots':6,
        'native_families':sorted(native),'native_fixtures':native_fixtures,'inherited_fixtures':sorted(inherited),
        'source_reconstruction':source_fixtures,'repair_start':repair,'changed_files':changed,'retention_accounting':retired,
        **({'historical_copy':historical_copy} if LOW else {}),
        'binary_limitation':transport.index['binary_replay_limit'],'hardware':'DEFERRED_TO_FINAL_CLOSURE'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--package',type=Path,required=True)
    p.add_argument('--host-tests',action='store_true');p.add_argument('--materialized',action='store_true')
    p.add_argument('--scratch-parent',type=Path,default=os.environ.get('HOBBYOS_REVIEW_SCRATCH'))
    a=p.parse_args();package=a.package.resolve()
    if a.scratch_parent: resources.configure_scratch(a.scratch_parent)
    if a.materialized:print(json.dumps(replay(package,a.host_tests),sort_keys=True));return
    P=module('package-memory-review');P.validate_tree(package)
    E.require(R.selection_audit(R.index(package))==json.loads((package/'selection-audit.json').read_text()),'owned selection audit changed')
    before={p.relative_to(package).as_posix():(E.sha(p),p.stat().st_mode&0o777) for p in package.rglob('*') if p.is_file()}
    with tempfile.TemporaryDirectory(prefix='owned-review-materialize-',dir=a.scratch_parent) as temp:
        restored=Path(temp)/'proof';R.materialize(package,restored)
        argv=[sys.executable,'-B',str(restored/'source/scripts'/('stack-guard-replay.py' if STACK else 'kernel-wx-replay.py' if IMAGE else 'low-half-replay.py' if LOW else 'direct-map-replay.py' if DIRECT else 'device-mapping-replay.py')),'--package',str(restored),'--materialized']
        if a.host_tests:argv.append('--host-tests')
        process=subprocess.run(argv,text=True,capture_output=True,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
        E.require(process.returncode==0,'owned materialized semantic/native replay failed:\n'+process.stdout+process.stderr)
        result=json.loads(process.stdout.splitlines()[-1])
    after={p.relative_to(package).as_posix():(E.sha(p),p.stat().st_mode&0o777) for p in package.rglob('*') if p.is_file()}
    E.require(before==after,'owned replay mutated original evidence');P.validate_tree(package)
    result.update(exact_reconstruction='PASS',immutable_original_recheck='PASS');print(json.dumps(result,sort_keys=True))
if __name__=='__main__':main()
