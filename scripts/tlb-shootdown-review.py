#!/usr/bin/env python3
"""Select a complete-source invalidation delivery with explicit acceptance state."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys

os.environ['HOBBYOS_VALIDATION_CONTRACT']='tlb-coherency'
os.environ['PYTHONDONTWRITEBYTECODE']='1'
import memory_evidence as E
ROOT=Path(__file__).resolve().parent.parent

def module(name):
    spec=importlib.util.spec_from_file_location('tlb_review_'+name.replace('-','_'),ROOT/'scripts'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);sys.modules[spec.name]=result;spec.loader.exec_module(result)
    os.environ['HOBBYOS_VALIDATION_CONTRACT']='tlb-coherency'
    return result

def select(session,campaign,review,incomplete):
    P=module('package-memory-review');S=module('memory-review-collect')
    class Selection(S.Selection):
        def add(self,source,relative,purpose,synthetic=False):
            before=len(self.files);super().add(source,relative,purpose,synthetic)
            if len(self.files)>before:
                self.files[-1]['local_os_permissions']=stat.S_IMODE(source.stat().st_mode)
                (self.review/relative).unlink()
    P.export(ROOT,review);selection=Selection(review)
    top=(*S.TOP,'test-plan.json','tlb-static.json','tlb-host.json','control-host.json','tlb-build.json',
         'tlb-runtime.json','tlb-production.json','tlb-failure-experiments.json','tlb-fixtures.json',
         'tlb-acceptance.json','previous-observation.json','blocker.json','focused-fixtures.json')
    for name in top:
        path=campaign/name
        if path.is_file():selection.add(path,'evidence/current-campaign/'+name,'current immutable campaign receipt')
    for name in (*S.GROUPS,'production','tlb-host','tlb-fixtures','control-host','nx-disabled','xapic',
                 'expected-failures','previous-observation','focused-fixtures','resource-attempts','static-vectors'):
        selection.tree(campaign/name,'evidence/current-campaign/'+name,'current '+name)
    selection.tree(campaign/'replay-revisions','evidence/current-campaign/replay-revisions',
                   'explicit verifier revisions; original attempt outcomes remain unchanged')
    for name in ('source-manifest.json','preflight.json','protected-anchors.json','commit.txt',
                 'unstaged.diff','staged.diff','accepted-evidence-index.json'):
        if (session/'before'/name).is_file():
            selection.add(session/'before'/name,'preservation/'+name,'approved pre-edit identity')
    selection.tree(session/'before/source','baseline-source','immutable approved control source')
    selection.tree(session/'before/observation-source','observation-source','original nonuniform observation verifier source')
    selection.tree(session/'before/accepted-index','preservation/accepted-index','accepted previous evidence index')
    for attempt in sorted((session/'attempts').iterdir()):
        # A currently executing packager cannot include its own open output.
        if attempt.is_dir() and ((attempt/'command.json').is_file() or
             any(attempt.glob('*/result.json')) or (attempt/'result.json').is_file()):
            selection.tree(attempt,'evidence/development/'+attempt.name,'retained implementation attempt; not current repetitions')
    for old in sorted((session/'evidence').glob('tlb-campaign-*')):
        if old==campaign:continue
        if not (old/'candidate-retirement.json').is_file():continue
        selection.add(old/'candidate-retirement.json','evidence/retired/'+old.name+'/candidate-retirement.json',
                      'reason old candidate cannot supply current repetitions')
        for name in ('candidates','profiles','campaign-attempts','input-source'):
            selection.tree(old/name,'evidence/retired/'+old.name+'/'+name,'immutable superseded candidate '+name)
    for path in sorted((session/'evidence').glob('*.json')):
        selection.add(path,'evidence/development/'+path.name,'diagnostic and owned storage receipt')
    if (session/'evidence/final-report.md').is_file():
        selection.add(session/'evidence/final-report.md','final-report.md','current results, scope and limitations')
    selection.finish()
    E.write(review/'local-candidate-index.json',{'schema':1,'build_input_sha256':E.identity(E.manifest()),
        'candidates':[{'profile':name,'path':str(campaign/'candidates'/name),
                       'receipt':json.loads((campaign/'candidates'/name/'candidate.json').read_text())}
                      for name in ('selftest','debug','production')],'all_local_binaries':selection.binaries})
    E.write(review/'delivery-state.json',{'schema':1,'current_acceptance':'INCOMPLETE' if incomplete else 'PASS',
        'portable_replay':'PENDING_FRESH_EXTRACTION','independent_review':'PENDING',
        'hardware':'DEFERRED_TO_FINAL_CLOSURE','human_visual_review':'PENDING','next_increment':'NOT_AUTHORIZED'})
    argv=['python3','source/scripts/tlb-shootdown-portable-replay.py','--package','.','--host-tests']
    if incomplete:argv.append('--incomplete')
    E.write(review/'replay.json',{'commands':[{'argv':argv,'expected_exit':0}]})

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('session','campaign','review','archive','extract'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--incomplete',action='store_true');args=parser.parse_args()
    for name in ('session','campaign','review','archive'):
        path=getattr(args,name).resolve();path.relative_to(ROOT/'backups');setattr(args,name,path)
    args.extract=args.extract.resolve();E.require(not args.extract.exists(),'fresh extraction destination exists')
    G=module('tlb-shootdown-gate')
    if args.incomplete:
        blocker=json.loads((args.campaign/'blocker.json').read_text())
        E.require(blocker['current_acceptance']=='INCOMPLETE' and blocker['reason'],'blocked delivery cause missing')
        G.T.scope('blocked-delivery');G.verify_host(args.campaign)
        for name in ('debug','production','selftest'):G.verify_codegen(args.campaign/'candidates'/name,name!='selftest')
    else:
        derived=G.verify(args.campaign)
        E.require(json.loads((args.campaign/'tlb-acceptance.json').read_text())['result']==derived,
                  'current acceptance must replay before packaging')
        E.require(not __import__('subprocess').check_output(['git','status','--porcelain=v1'],cwd=ROOT).strip(),
                  'completed delivery requires clean committed source')
    select(args.session,args.campaign,args.review,args.incomplete)
    result=module('cpu-control-review').archive(args.review,args.archive,args.extract)
    print(json.dumps(result,sort_keys=True))

if __name__=='__main__':
    try:main()
    except Exception as error:sys.exit(str(error))
