#!/usr/bin/env python3
"""Select explicit control-state evidence using the existing exact-mode packager."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import stat
import zipfile

os.environ['HOBBYOS_VALIDATION_CONTRACT']='cpu-control-state'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E

ROOT=Path(__file__).resolve().parent.parent


def module(name):
    spec=importlib.util.spec_from_file_location('control_review_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[spec.name]=value;spec.loader.exec_module(value);return value


def select(session,campaign,review,preview=False,stream=False):
    P=module('package-memory-review');S=module('memory-review-collect')
    class StreamingSelection(S.Selection):
        def add(self,source,relative,purpose,synthetic=False):
            before=len(self.files)
            super().add(source,relative,purpose,synthetic)
            # The index retains the exact original path/hash. Archive creation
            # verifies and streams it; no evidence is removed from its session.
            if len(self.files)>before:
                self.files[-1]['local_os_permissions']=stat.S_IMODE(source.stat().st_mode)
                (self.review/relative).unlink()
    P.export(ROOT,review);selection=(StreamingSelection if stream else S.Selection)(review)
    top=(*S.TOP,'control-static.json','control-host.json','control-build.json','control-runtime.json',
         'control-production.json','control-fixtures.json','control-acceptance.json','previous-observation.json')
    for name in top:
        source=campaign/name
        if source.is_file():selection.add(source,'evidence/current-campaign/'+name,'current control campaign receipt')
    for name in (*S.GROUPS,'control-host','control-fixtures','supplemental','previous-observation'):
        directory=campaign/name
        if preview and name in ('regression-attempts','attempts'):
            terminal='result.json' if name=='regression-attempts' else 'command.json'
            for child in sorted(directory.glob('*')):
                if (child/terminal).is_file():selection.tree(child,'evidence/current-campaign/'+name+'/'+child.name,'completed current '+name)
        else:selection.tree(directory,'evidence/current-campaign/'+name,'current '+name)
    for name in ('source-manifest.json','preflight.json','protected-anchors.json','commit.txt','unstaged.diff','staged.diff',
                 'accepted-evidence-index.json','spare-slot-proof.json'):
        selection.add(session/'before'/name,'preservation/'+name,'approved pre-edit source and evidence identity')
    selection.tree(session/'before/source','baseline-source','immutable approved observation source for historical replay')
    selection.tree(session/'before/accepted-index','preservation/accepted-index','approved acceptance and binary index')
    # Development failures and focused experiments are immutable and separate
    # from selected successes. Never transport an extracted review or old ZIP.
    for name in ('boot-development','expected-failures','fixture-development'):
        selection.tree(session/'evidence'/name,'evidence/development/'+name,'retained development attempts, not acceptance repetitions')
    for path in sorted((session/'evidence').glob('*.json')):
        selection.add(path,'evidence/development/'+path.name,'owned storage/diagnostic experiment receipt')
    report=session/'evidence/final-report.md'
    if report.is_file():selection.add(report,'final-report.md','current acceptance, file purposes and explicit limitations')
    # A packager command cannot include its own still-open output. Its final
    # external receipt remains local and is eligible in a subsequent selection.
    for attempt in sorted((session/'attempts').glob('*')):
        if (attempt/'command.json').is_file():
            selection.tree(attempt,'evidence/local-commands/'+attempt.name,'completed append-only local command records')
    selection.finish()
    E.write(review/'local-candidate-index.json',{'schema':1,'build_input_sha256':E.identity(E.manifest()),
        'candidates':[{ 'profile':name,'receipt':str(campaign/'candidates'/name/'candidate.json'),
                        'identity':E.sha(campaign/'candidates'/name/'candidate.json'),
                        'build':json.loads((campaign/'candidates'/name/'candidate.json').read_text()) }
                      for name in ('selftest','debug','production')],
        'all_local_binaries':selection.binaries})
    E.write(review/'delivery-state.json',{'schema':1,'current_acceptance':'NOT_ASSERTED' if preview else 'PASS',
        'portable_replay':'PENDING_FRESH_EXTRACTION','independent_review':'PENDING',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','human_visual_review':'PENDING',
        'next_increment':'NOT_AUTHORIZED','selection':'explicit current campaign and one approved baseline capture'})
    E.write(review/'replay.json',{'commands':[] if preview else [{'argv':['python3','source/scripts/cpu-control-portable-replay.py',
        '--package','.','--host-tests'],'expected_exit':0}]})
    return {'status':'PREVIEW' if preview else 'SELECTED','raw_files':len(selection.files),
            'local_binaries':len(selection.binaries),'synthetic_deltas':len(selection.synthetic)}


def archive(review,output,extraction):
    """Write the same literal-mode format without a second full raw staging tree."""
    P=module('package-memory-review');index=json.loads((review/'content-selection.json').read_text())
    sources={r['path']:Path(r['local_path']) for r in index['raw']}
    expected={r['path']:r for r in index['raw']}
    for path in review.rglob('*'):
        if path.is_file():
            name=path.relative_to(review).as_posix();E.safe(name)
            E.require(name not in sources,'duplicate streamed/staged member')
            sources[name]=path
    E.require('manifest.json' not in sources and not output.exists(),'archive already finalized')
    rows=[];original_modes={}
    for name,path in sorted(sources.items()):
        E.safe(name);E.require(path.is_file() and not path.is_symlink(),'archive source is not a regular file')
        mode=P.mode(path);size=path.stat().st_size;digest=E.sha(path)
        original_modes[name]=stat.S_IMODE(path.stat().st_mode)
        if name in expected:
            recorded=expected[name]
            E.require((size,digest,mode)==(recorded['size'],recorded['sha256'],recorded['os_permissions']),
                      'selected raw evidence changed before streaming')
            E.require(original_modes[name]==recorded['local_os_permissions'],'selected original exact mode changed')
        rows.append({'path':name,'size':size,'sha256':digest,'os_permissions':mode})
    P.dump(review/'manifest.json',{'schema':2,'permission_contract':'literal-posix-0644-or-0755',
        'self_excluded':'manifest.json','files':rows})
    sources['manifest.json']=review/'manifest.json'
    with zipfile.ZipFile(output,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as zipped:
        for name,path in sorted(sources.items()):
            info=zipfile.ZipInfo.from_file(path,arcname=name);info.create_system=3
            info.external_attr=(stat.S_IFREG|P.mode(path))<<16;info.compress_type=zipfile.ZIP_DEFLATED
            with zipped.open(info,'w',force_zip64=True) as dest,path.open('rb') as source:
                import shutil
                shutil.copyfileobj(source,dest)
    result=P.verify_archive(output,extraction)
    groups={}
    for row in rows:
        group='/'.join(row['path'].split('/')[:3]);groups[group]=groups.get(group,0)+row['size']
    result.update(path=str(output),size=output.stat().st_size,logical_bytes=sum(r['size'] for r in rows)+(review/'manifest.json').stat().st_size,
        sha256=E.sha(output),source_files=len(json.loads((review/'source-manifest.json').read_text())['files']),
        largest_groups=sorted(groups.items(),key=lambda r:r[1],reverse=True)[:20],
        local_binary_index=str(review/'content-selection.json'))
    # Replay checks the extraction; also recheck the actual immutable inputs.
    for row in rows:
        path=sources[row['path']]
        E.require(path.stat().st_size==row['size'] and E.sha(path)==row['sha256'] and P.mode(path)==row['os_permissions'],
                  'original selected evidence changed during replay')
        E.require(stat.S_IMODE(path.stat().st_mode)==original_modes[row['path']],
                  'original selected exact permission changed during replay')
    result['selected_originals_rechecked']=True
    Path(str(output)+'.sha256').write_text(result['sha256']+'  '+output.name+'\n')
    P.dump(Path(str(output)+'.receipt.json'),result)
    return {k:v for k,v in result.items() if k!='replay'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session',type=Path,required=True);p.add_argument('--campaign',type=Path,required=True)
    p.add_argument('--review',type=Path,required=True);p.add_argument('--preview-completed',action='store_true')
    p.add_argument('--archive',type=Path);p.add_argument('--extract',type=Path)
    a=p.parse_args();session=a.session.resolve();campaign=a.campaign.resolve();review=a.review.resolve()
    for path in (session,campaign,review):path.relative_to(ROOT/'backups')
    if not a.preview_completed:
        C=module('cpu-control-state-gate');derived=C.verify(campaign)
        E.require(json.loads((campaign/'control-acceptance.json').read_text())['result']==derived,
                  'control acceptance must replay before delivery selection')
    E.require(bool(a.archive)==bool(a.extract),'archive and fresh extraction destinations are both required')
    result=select(session,campaign,review,a.preview_completed,bool(a.archive))
    if a.archive:
        a.archive=a.archive.resolve();a.extract=a.extract.resolve()
        a.archive.relative_to(ROOT/'backups');a.extract.relative_to(ROOT/'backups')
        result=archive(review,a.archive,a.extract)
    print(json.dumps(result))


if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
