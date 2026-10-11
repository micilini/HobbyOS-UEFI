#!/usr/bin/env python3
"""Export real tracked/current source and curated evidence; verify safe ZIP payloads."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import zipfile


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()


def safe(name):
    p=PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or '\\' in name or str(p)!=name or ':' in name:
        raise ValueError('unsafe archive path: '+name)
    return p


def mode(path):return 0o755 if path.stat().st_mode & 0o111 else 0o644


def dump(path,value):path.write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')


def export(root,review):
    if review.exists():raise ValueError('review staging already exists')
    review.mkdir(parents=True)
    (review/'source').mkdir()
    tracked={}
    for item in subprocess.check_output(['git','ls-files','-s','-z'],cwd=root).decode().split('\0'):
        if not item:continue
        left,name=item.split('\t');gm,blob,stage=left.split()
        if stage!='0' or gm not in ('100644','100755'):raise ValueError('inspect unmerged/symlink/submodule: '+name)
        tracked[name]=(gm,blob)
    untracked=subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=root).decode().split('\0')
    names=set(tracked)
    for name in filter(None,untracked):
        if Path(name).suffix in ('.su', '.o', '.d', '.pyc'): continue
        if not name.startswith(('kernel/src/','scripts/','docs/memory/')) or Path(name).suffix not in ('.c','.h','.S','.py','.sh','.md','.json'):
            raise ValueError('unexpected untracked source/output: '+name)
        names.add(name)
    rows=[]
    for name in sorted(names):
        safe(name);path=root/name
        if path.is_symlink() or not path.is_file():raise ValueError('source is not a regular file: '+name)
        target=review/'source'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target);os.chmod(target,mode(path))
        gm,base_blob=tracked.get(name,(None,None))
        current_blob=subprocess.check_output(['git','hash-object','--stdin'],cwd=root,input=path.read_bytes()).decode().strip()
        rows.append({'path':name,'size':path.stat().st_size,'sha256':sha(path),'mode':f'100{mode(path):03o}',
                     'base_git_mode':gm,'base_blob':base_blob,'current_blob':current_blob,'tracked':name in tracked})
    status = subprocess.check_output(['git','status','--porcelain=v1','--untracked-files=all'],cwd=root).decode()
    changed = subprocess.check_output(['git','diff','HEAD','--name-only','-z'],cwd=root).decode().split('\0')
    source_dirty = bool(set(filter(None, changed)) & names or names - set(tracked))
    tree = source_tree(rows)
    head = subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    head_tree = subprocess.check_output(['git','rev-parse','HEAD^{tree}'],cwd=root,text=True).strip()
    if not source_dirty and tree != head_tree: raise ValueError('clean source tree differs from HEAD')
    dump(review/'source-manifest.json',{'schema':2,'state':'DIRTY_WORKTREE_SOURCE' if source_dirty else 'CLEAN_COMMITTED_SOURCE',
         'git_worktree_clean':not bool(status.strip()), 'source_clean':not source_dirty,
         'head':head,'head_tree':head_tree,'export_tree':tree,'files':rows})
    (review/'git').mkdir()
    commands={'head.txt':['rev-parse','HEAD'],'tree.txt':['rev-parse','HEAD^{tree}'],'branch.txt':['branch','--show-current'],
              'identity.txt':['log','-1','--format=%H%n%T%n%P%n%s'],'commit-object.txt':['cat-file','-p','HEAD'],
              'tracked.txt':['ls-files','-s'],'status.txt':['status','--porcelain=v1','--untracked-files=all'],
              'changes.patch':['diff','--binary'],'staged.patch':['diff','--cached','--binary'],'diff-stat.txt':['diff','--stat'],
              'changed-commit-files.txt':['diff-tree','--no-commit-id','--name-status','-r','HEAD']}
    for name,args in commands.items():(review/'git'/name).write_bytes(subprocess.check_output(['git',*args],cwd=root))
    return rows


def source_tree(rows):
    tree = {}
    for row in rows:
        parts = row['path'].split('/'); node = tree
        for part in parts[:-1]: node = node.setdefault(part, {})
        if parts[-1] in node: raise ValueError('duplicate source path')
        node[parts[-1]] = (row['mode'], row['current_blob'])
    def digest(node):
        data = bytearray()
        for name, value in sorted(node.items(), key=lambda pair: (pair[0] + ('/' if isinstance(pair[1], dict) else '')).encode()):
            gm, blob = ('40000', digest(value)) if isinstance(value, dict) else value
            data.extend(gm.encode() + b' ' + name.encode() + b'\0' + bytes.fromhex(blob))
        return hashlib.sha1(b'tree ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    return digest(tree)


def manifest(review):
    rows=[]
    for path in sorted(review.rglob('*')):
        kind=path.lstat().st_mode
        if stat.S_ISDIR(kind):continue
        if not stat.S_ISREG(kind):raise ValueError('unexpected staging file type: '+str(path))
        os.chmod(path,mode(path))
        name=path.relative_to(review).as_posix();safe(name)
        if name=='manifest.json':continue
        rows.append({'path':name,'size':path.stat().st_size,'sha256':sha(path),'os_permissions':mode(path)})
    dump(review/'manifest.json',{'schema':2,'permission_contract':'literal-posix-0644-or-0755','self_excluded':'manifest.json','files':rows})
    os.chmod(review/'manifest.json',0o644)


def validate_tree(directory):
    manifest_data=json.loads((directory/'manifest.json').read_text())
    if manifest_data['schema']!=2 or manifest_data['self_excluded']!='manifest.json' or manifest_data['permission_contract']!='literal-posix-0644-or-0755':raise ValueError('manifest schema')
    if stat.S_IMODE((directory/'manifest.json').stat().st_mode)!=0o644:raise ValueError('manifest exact permissions')
    rows=manifest_data['files'];seen=set()
    for row in rows:
        safe(row['path'])
        if row['path'] in seen:raise ValueError('duplicate manifest path')
        seen.add(row['path']);p=directory/row['path']
        if p.is_symlink() or not p.is_file() or p.stat().st_size!=row['size'] or sha(p)!=row['sha256'] or stat.S_IMODE(p.stat().st_mode)!=row['os_permissions']:
            raise ValueError('payload differs: '+row['path'])
    actual={p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    if actual!=seen|{'manifest.json'}:raise ValueError('unexpected/missing payload files')
    source=json.loads((directory/'source-manifest.json').read_text())
    names=set()
    for r in source['files']:
        safe(r['path']);p=directory/'source'/r['path'];names.add(r['path'])
        if p.stat().st_size!=r['size'] or sha(p)!=r['sha256'] or f'100{mode(p):03o}'!=r['mode']:
            raise ValueError('source manifest mismatch: '+r['path'])
        data=p.read_bytes();blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        if blob!=r['current_blob']:raise ValueError('current Git blob identity mismatch')
    if source.get('schema') == 2:
        if source_tree(source['files']) != source['export_tree']:raise ValueError('exported Git tree differs')
        commit=(directory/'git/commit-object.txt').read_bytes()
        commit_id=hashlib.sha1(b'commit '+str(len(commit)).encode()+b'\0'+commit).hexdigest()
        fields=commit.decode().split('\n\n',1)[0].splitlines()
        trees=[line[5:] for line in fields if line.startswith('tree ')]
        if commit_id!=source['head'] or (directory/'git/head.txt').read_text().strip()!=commit_id:
            raise ValueError('commit object identity differs')
        if trees!=[source['head_tree']] or (directory/'git/tree.txt').read_text().strip()!=source['head_tree']:
            raise ValueError('commit tree identity differs')
        clean=not bool((directory/'git/status.txt').read_text().strip())
        if source['git_worktree_clean']!=clean:raise ValueError('Git clean-state receipt differs')
        expected='CLEAN_COMMITTED_SOURCE' if source['source_clean'] else 'DIRTY_WORKTREE_SOURCE'
        if source['state']!=expected or (source['source_clean'] and source['export_tree']!=source['head_tree']):
            raise ValueError('declared source/commit state differs')
    if len(names)!=len(source['files']):raise ValueError('duplicate source manifest entry')
    if names!={p.relative_to(directory/'source').as_posix() for p in (directory/'source').rglob('*') if p.is_file()}:
        raise ValueError('incomplete source export')
    return len(rows)+1


def verify_archive(archive,destination):
    destination.mkdir(parents=True,exist_ok=False)
    with zipfile.ZipFile(archive) as z:
        names=[]
        for info in z.infolist():
            safe(info.filename);names.append(info.filename)
            kind=info.external_attr>>16
            if info.create_system!=3 or not stat.S_ISREG(kind):raise ValueError('unexpected ZIP file type')
            if stat.S_IMODE(kind) not in (0o644,0o755):raise ValueError('noncanonical exact ZIP permissions: '+info.filename)
        if len(names)!=len(set(names)):raise ValueError('duplicate ZIP members')
        bad=z.testzip()
        if bad:raise ValueError('ZIP CRC failure: '+bad)
        for info in z.infolist():
            target=destination/info.filename;target.parent.mkdir(parents=True,exist_ok=True)
            with z.open(info) as src,target.open('wb') as dst:shutil.copyfileobj(src,dst)
            os.chmod(target,(info.external_attr>>16)&0o777)
    count=validate_tree(destination)
    plan=json.loads((destination/'replay.json').read_text())
    results=[]
    before={p.relative_to(destination).as_posix():sha(p) for p in destination.rglob('*') if p.is_file()}
    for item in plan['commands']:
        argv=[sys.executable if arg=='python3' else arg for arg in item['argv']]
        process=subprocess.run(argv,cwd=destination,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',
                               GIT_CEILING_DIRECTORIES=str(destination)),text=True,capture_output=True)
        if process.returncode!=item['expected_exit']:
            raise ValueError('portable replay: '+str(argv)+'\n'+process.stdout+'\n'+process.stderr)
        results.append({'argv':item['argv'],'expected_exit':item['expected_exit'],'actual_exit':process.returncode,
                        'stdout':process.stdout,'stderr':process.stderr})
    validate_tree(destination)
    after={p.relative_to(destination).as_posix():sha(p) for p in destination.rglob('*') if p.is_file()}
    if before!=after:raise ValueError('portable replay modified packaged evidence')
    return {'status':'PASS','members':count,'replay':results,'permission_contract':'literal-posix-0644-or-0755','original_evidence_unchanged':True}


def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='mode',required=True)
    e=sub.add_parser('export');e.add_argument('--review',type=Path,required=True)
    z=sub.add_parser('archive');z.add_argument('--review',type=Path,required=True);z.add_argument('--output',type=Path,required=True);z.add_argument('--extract',type=Path,required=True)
    v=sub.add_parser('verify');v.add_argument('--archive',type=Path,required=True);v.add_argument('--extract',type=Path,required=True)
    args=p.parse_args();root=Path(__file__).resolve().parent.parent
    if args.mode=='export':
        actual=Path(subprocess.check_output(['git','rev-parse','--show-toplevel'],cwd=root,text=True).strip())
        args.review.resolve().relative_to(actual/'backups');rows=export(actual,args.review)
        print(json.dumps({'status':'EXPORTED','source_files':len(rows)}));return
    if args.mode=='archive':
        args.output.resolve().relative_to(root/'backups')
        if args.output.exists():raise ValueError('archive already exists')
        manifest(args.review);validate_tree(args.review)
        with zipfile.ZipFile(args.output,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as out:
            for path in sorted(args.review.rglob('*')):
                if path.is_file():out.write(path,path.relative_to(args.review).as_posix())
        result=verify_archive(args.output,args.extract)
        with zipfile.ZipFile(args.output) as zipped:
            logical=sum(item.file_size for item in zipped.infolist())
            groups={}
            for item in zipped.infolist():
                group='/'.join(item.filename.split('/')[:3]);groups[group]=groups.get(group,0)+item.file_size
        result.update(path=str(args.output.resolve()),size=args.output.stat().st_size,logical_bytes=logical,sha256=sha(args.output),
                      source_files=len(json.loads((args.review/'source-manifest.json').read_text())['files']),
                      largest_groups=sorted(groups.items(),key=lambda item:item[1],reverse=True)[:20])
        Path(str(args.output)+'.sha256').write_text(result['sha256']+'  '+args.output.name+'\n')
        dump(Path(str(args.output)+'.receipt.json'),result)
        print(json.dumps({k:v for k,v in result.items() if k!='replay'}));return
    print(json.dumps(verify_archive(args.archive,args.extract)))


if __name__=='__main__':main()
