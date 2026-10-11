#!/usr/bin/env python3
"""Small hosted archive path/manifest/CRC guards, isolated beneath a supplied output."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import zipfile
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('package_review',Path(__file__).with_name('package-memory-review.py'))
P=importlib.util.module_from_spec(spec);spec.loader.exec_module(P)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False);results=[]
    for name in ('/absolute','../traversal','a/../traversal','a//duplicate','a/./alias','a\\windows','c:drive'):
        try:P.safe(name)
        except ValueError as e:results.append({'case':name,'expected_exit':1,'actual_exit':1,'reason':str(e)})
        else:raise ValueError('unsafe path accepted')
    tree=a.output/'tree';(tree/'source').mkdir(parents=True)
    path=tree/'source'/'input.txt';path.write_text('fixture\n');os.chmod(path,0o644);data=path.read_bytes()
    rows=[{'path':'input.txt','size':len(data),'sha256':P.sha(path),'mode':'100644','current_blob':hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()}]
    tree_id=P.source_tree(rows)
    commit=('tree '+tree_id+'\nauthor Fixture <fixture@example.invalid> 0 +0000\ncommitter Fixture <fixture@example.invalid> 0 +0000\n\nSynthetic packaging fixture\n').encode()
    commit_id=hashlib.sha1(b'commit '+str(len(commit)).encode()+b'\0'+commit).hexdigest()
    source={'schema':2,'state':'CLEAN_COMMITTED_SOURCE','git_worktree_clean':True,'source_clean':True,
            'head':commit_id,'head_tree':tree_id,'export_tree':tree_id,'files':rows}
    P.dump(tree/'source-manifest.json',source)
    (tree/'git').mkdir()
    for name,value in {'commit-object.txt':commit,'head.txt':(commit_id+'\n').encode(),
                       'tree.txt':(tree_id+'\n').encode(),'status.txt':b''}.items():(tree/'git'/name).write_bytes(value)
    P.dump(tree/'replay.json',{'commands':[{'argv':['python3','-c','raise SystemExit(0)'],'expected_exit':0}]})
    P.manifest(tree);P.validate_tree(tree)
    archive=a.output/'positive.zip'
    with zipfile.ZipFile(archive,'w') as z:
        for f in tree.rglob('*'):
            if f.is_file():z.write(f,f.relative_to(tree).as_posix())
    P.verify_archive(archive,a.output/'positive-extraction');results.append({'case':'fresh-extraction','expected_exit':0,'actual_exit':0})
    for name,member in [('absolute','/x'),('traversal','../x'),('symlink','link'),('duplicate','duplicate')]:
        archive=a.output/(name+'.zip')
        with zipfile.ZipFile(archive,'w') as z:
            info=zipfile.ZipInfo(member)
            info.external_attr=((stat.S_IFLNK if name=='symlink' else stat.S_IFREG)|0o644)<<16
            z.writestr(info,b'../escape' if name=='symlink' else b'fixture')
            if name=='duplicate':z.writestr(info,b'again')
        try:P.verify_archive(archive,a.output/(name+'-extraction'))
        except ValueError as e:results.append({'case':name,'expected_exit':1,'actual_exit':1,'reason':str(e)})
        else:raise ValueError('unsafe archive accepted')
    archive=a.output/'metadata-mode.zip'
    with zipfile.ZipFile(archive,'w') as z:
        for f in tree.rglob('*'):
            if f.is_file():
                info=zipfile.ZipInfo(f.relative_to(tree).as_posix());info.create_system=3
                info.external_attr=(stat.S_IFREG | (0o664 if f.name=='replay.json' else 0o644))<<16
                z.writestr(info,f.read_bytes())
    try:P.verify_archive(archive,a.output/'metadata-mode-extraction')
    except ValueError as e:
        if 'exact ZIP permissions' not in str(e):raise
        results.append({'case':'metadata-permissions-0664','expected_exit':1,'actual_exit':1,'reason':str(e)})
    else:raise ValueError('noncanonical metadata permissions accepted')
    for name,target,value,boundary in (
            ('forged-commit','git/commit-object.txt',commit+b'changed\n','commit object identity'),
            ('false-clean-state','git/status.txt',b' M input.txt\n','Git clean-state receipt'),
            ('false-source-state','source-manifest.json',json.dumps(dict(source,state='DIRTY_DIAGNOSTIC_SOURCE')).encode(),'declared source/commit state')):
        changed=tree/target;original=changed.read_bytes();changed.write_bytes(value);P.manifest(tree)
        try:P.validate_tree(tree)
        except ValueError as e:
            if boundary not in str(e):raise
            results.append({'case':name,'synthetic':True,'outer_manifest_recomputed':True,'expected_exit':1,'actual_exit':1,'reason':str(e)})
        else:raise ValueError('false source identity accepted')
        changed.write_bytes(original);P.manifest(tree)
    path.write_text('changed\n')
    try:P.validate_tree(tree)
    except ValueError as e:results.append({'case':'payload-hash','expected_exit':1,'actual_exit':1,'reason':str(e)})
    else:raise ValueError('changed payload accepted')
    path.write_bytes(data);os.chmod(path,0o755)
    try:P.validate_tree(tree)
    except ValueError as e:results.append({'case':'executable-mode','expected_exit':1,'actual_exit':1,'reason':str(e)})
    else:raise ValueError('changed executable mode accepted')
    P.dump(a.output/'result.json',{'status':'PASS','cases':results})
    print(json.dumps({'status':'PASS','checks':len(results)}))


if __name__=='__main__':main()
