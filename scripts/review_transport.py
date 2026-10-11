"""Bounded lossless transport for explicitly selected immutable review files.

Current source stays directly readable. Other files reference verified source
bytes or content-addressed chunks. Reconstruction never writes into extraction.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import zipfile
import zlib
import memory_evidence as E

CHUNK = 1024 * 1024
MAX_FILE = 1024 * 1024 * 1024
MAX_TOTAL = 16 * 1024 * 1024 * 1024
MAX_FILES = 100000
MAX_REFERENCES = 200000
MAX_ARCHIVE_PAYLOAD = 2 * 1024 * 1024 * 1024
WORKSPACE_RESERVE = 64 * 1024 * 1024
LIMITATION = ('Offline replay checks original raw evidence and saved binary facts. '
             'Omitted full ELF/image binaries are neither executed nor rehashed offline.')

def regular(path):
    E.require(stat.S_ISREG(path.lstat().st_mode), 'transport nonregular input: '+str(path))

def digest_bytes(data): return hashlib.sha256(data).hexdigest()

def local_binary_only(suffix, prefix, size, synthetic):
    binary=suffix.lower() in ('.elf','.img','.efi','.fd','.o') or prefix==b'\x7fELF'
    focused=synthetic and size<128*1024 and suffix.lower() not in ('.img','.fd')
    # Native oracle fixtures use tiny text stand-ins for payload identities.
    # They are fixture bytes, not executable VM images, and must reconstruct.
    stand_in=synthetic and size<4096 and prefix!=b'\x7fELF'
    return binary and not (focused or stand_in)

class Store:
    def __init__(self, root):
        self.root=Path(root);self.rows={};self.blobs={}
        self.source={}
        for row in json.loads((self.root/'source-manifest.json').read_text())['files']:
            path='source/'+row['path'];E.safe(path)
            self.source[row['sha256']]={'path':path,'size':row['size'],'sha256':row['sha256']}
        self.groups={};self.local=[]

    def chunks(self, source, digest, size):
        if digest in self.source:
            ref=self.source[digest];E.require(ref['size']==size,'source content collision')
            return [ref]
        refs=[]
        with source.open('rb') as stream:
            for data in iter(lambda:stream.read(CHUNK),b''):
                sha=digest_bytes(data);name='objects/'+sha
                if sha not in self.blobs:
                    target=self.root/name;target.parent.mkdir(exist_ok=True)
                    encoded=zlib.compress(data,6)
                    with target.open('xb') as out:out.write(encoded)
                    os.chmod(target,0o644);self.blobs[sha]={'path':name,'size':len(data),'sha256':sha,
                        'codec':'zlib-1','stored_size':len(encoded),'stored_sha256':digest_bytes(encoded)}
                E.require(self.blobs[sha]['size']==len(data),'chunk content collision')
                refs.append({k:self.blobs[sha][k] for k in ('path','size','sha256')})
        return refs

    def add(self, source, name, purpose, consumers, *, group='raw', blob=None):
        source=Path(source);regular(source);E.safe(name)
        E.require(consumers and purpose,'transport object has no consuming validator/purpose')
        size=source.stat().st_size;E.require(size<=MAX_FILE,'transport file expansion limit')
        row={'path':name,'size':size,'sha256':E.sha(source),
             'os_permissions':0o755 if source.stat().st_mode&0o111 else 0o644,
             'purpose':purpose,'consumers':sorted(set(consumers)),
             'local_path':str(source.resolve()),'local_os_permissions':stat.S_IMODE(source.stat().st_mode),'group':group,
             'transport':'SYNTHETIC_FILE_DELTA' if blob else 'EXACT_RAW_FILE'}
        if blob: E.safe(blob);row['blob']=blob
        if name in self.rows:
            old=self.rows[name]
            E.require(all(old[k]==row[k] for k in ('size','sha256','os_permissions','group','transport')),
                      'conflicting logical transport reference')
            old['consumers']=sorted(set(old['consumers'])|set(consumers));return old
        row['chunks']=self.chunks(source,row['sha256'],size);self.rows[name]=row
        return row

    def omitted(self,row,consumers):
        row=dict(row,consumers=sorted(set(consumers)))
        E.safe(row['path']);E.require(consumers,'omitted binary consumer missing')
        E.require(row['path'] not in self.rows and not any(r['path']==row['path'] for r in self.local),
                  'duplicate omitted binary path')
        self.local.append(row)

    def finish(self, local_only):
        raw=[];synthetic=[]
        for row in sorted(self.rows.values(),key=lambda r:r['path']):
            (synthetic if row['group']=='synthetic_deltas' else raw).append(row)
        index={'schema':2,'raw':raw,'synthetic_deltas':synthetic,
               'local_binaries':sorted(self.local,key=lambda r:r['path']),
               'binary_replay_limit':LIMITATION,'local_only':local_only,
               'limits':{'file_bytes':MAX_FILE,'logical_bytes':MAX_TOTAL,'files':MAX_FILES,
                         'chunk_bytes':CHUNK,'references':MAX_REFERENCES},
               'blobs':sorted(self.blobs.values(),key=lambda r:r['path'])}
        E.write(self.root/'transport-manifest.json',index)
        E.write(self.root/'content-selection.json',{'schema':2,'manifest':'transport-manifest.json',
            'sha256':E.sha(self.root/'transport-manifest.json')})
        validate(self.root)
        return index

def index(root):
    pointer=json.loads((root/'content-selection.json').read_text())
    E.require(pointer=={'schema':2,'manifest':'transport-manifest.json',
                       'sha256':E.sha(root/'transport-manifest.json')},'transport canonical manifest binding')
    return json.loads((root/'transport-manifest.json').read_text())

def selection_audit(value):
    """Reject whole historical trees independently of compression or replay."""
    prefixes=('evidence/current-campaign/','baseline-source/','observation-source/',
              'preservation/','binary-facts/')
    rows=value['raw']+value['synthetic_deltas']+value['local_binaries']
    names={row['path'] for row in rows};edges=0
    for row in rows:
        E.require(row['path'].startswith(prefixes),'unapproved review selection root')
        for consumer in row['consumers']:
            if consumer.startswith('receipt:'):
                E.require('evidence/current-campaign/'+consumer[8:] in names,
                          'selected dependency has no consuming receipt')
                edges+=1
    return {'schema':1,'status':'PASS','selected_objects':len(rows),
            'explicit_receipt_edges':edges,'permitted_roots':list(prefixes),
            'whole_retired_campaign_trees':0,'whole_development_attempt_trees':0,
            'authority':'Manifest references plus full semantic/native replay of reconstructed bytes'}

def validate(root):
    root=Path(root).resolve();value=index(root)
    for path in (root/'objects').rglob('*'):
        E.require(not path.is_symlink() and (path.is_dir() or stat.S_ISREG(path.lstat().st_mode)),
                  'transport nonregular object type')
    E.require(value['schema']==2 and value['limits']=={'file_bytes':MAX_FILE,'logical_bytes':MAX_TOTAL,
        'files':MAX_FILES,'chunk_bytes':CHUNK,'references':MAX_REFERENCES},'transport limits altered')
    blobs=value['blobs'];blob_index={r['path']:r for r in blobs}
    E.require(len(blobs)==len(blob_index),'duplicate compressed object')
    rows=value['raw']+value['synthetic_deltas'];names=set();destinations={};refs={};total=0;count=0
    E.require(len(rows)+len(value['local_binaries'])<=MAX_FILES,'transport file count exceeds bound')
    for row in rows:
        name=row['path'];E.safe(name)
        E.require(name not in names,'duplicate transport destination');names.add(name)
        E.require(row['consumers'] and row['purpose'],'transport unused selection')
        E.require(row['os_permissions'] in (0o644,0o755) and type(row['size']) is int and 0<=row['size']<=MAX_FILE,
                  'transport mode/size limit')
        total+=row['size'];count+=len(row['chunks'])
        E.require(total<=MAX_TOTAL and count<=MAX_REFERENCES,'transport expansion limit')
        destination=row.get('blob',name);E.safe(destination)
        E.require(destination!='.' and destination.split('/')[0] not in ('objects',), 'reserved transport destination')
        signature=(row['size'],row['sha256'],row['os_permissions'] if 'blob' not in row else 0o644)
        E.require(destination not in destinations or destinations[destination]==signature,'conflicting reconstruction destination')
        destinations[destination]=signature
        observed=0;digest=hashlib.sha256()
        for ref in row['chunks']:
            E.require(set(ref)=={'path','size','sha256'},'invalid chunk reference fields');E.safe(ref['path'])
            E.require(ref['path'].startswith(('objects/','source/')),'unexpected transport reference domain')
            E.require(type(ref['size']) is int and 0<ref['size']<= (MAX_FILE if ref['path'].startswith('source/') else CHUNK),
                      'transport chunk expansion limit')
            path=root/ref['path']
            for parent in [path,*path.parents]:
                if parent==root:break
                E.require(not parent.is_symlink(),'transport symlink reference')
            regular(path)
            if ref['path'].startswith('objects/'):
                E.require(ref['path']=='objects/'+ref['sha256'],'transport object name hash differs')
                E.require(stat.S_IMODE(path.stat().st_mode)==0o644,'transport chunk exact mode')
                E.require(ref['path'] in blob_index,'missing compressed object manifest')
                encoded=blob_index[ref['path']]
                E.require(all(ref[k]==encoded[k] for k in ref),'compressed reference conflicts')
                digest.update(decode(path,encoded));refs[ref['path']]=encoded
            else:
                E.require(path.stat().st_size==ref['size'] and E.sha(path)==ref['sha256'],'transport chunk hash/size substitution')
                with path.open('rb') as stream:
                    for block in iter(lambda:stream.read(CHUNK),b''):digest.update(block)
            observed+=ref['size']
        E.require(observed==row['size'] and digest.hexdigest()==row['sha256'],'transport reconstructed hash/extent differs')
    for row in value['local_binaries']:
        E.safe(row['path']);E.require(row['path'] not in names,'duplicate omitted destination');names.add(row['path'])
        E.require(row['transport']=='LOCAL_BINARY_ONLY' and row['consumers'],'omitted binary contract')
    E.require(len(blobs)==len({r['path'] for r in blobs}) and {r['path']:r for r in blobs}==refs,'unlisted/missing transport blobs')
    actual={p.relative_to(root).as_posix() for p in (root/'objects').rglob('*') if not p.is_dir()}
    E.require(actual==set(refs),'unexpected transport object')
    ordered=sorted(destinations)
    for a,b in zip(ordered,ordered[1:]):E.require(not b.startswith(a+'/'),'transport file/directory collision')
    return value

def decode(path,row):
    E.require(row['codec']=='zlib-1' and type(row['stored_size']) is int and
              0<row['stored_size']<=CHUNK+1024 and 0<row['size']<=CHUNK,'compressed object bounds/codec')
    E.require(path.stat().st_size==row['stored_size'] and E.sha(path)==row['stored_sha256'],
              'compressed object hash/size substitution')
    decoder=zlib.decompressobj()
    try:data=decoder.decompress(path.read_bytes(),row['size']+1)
    except zlib.error as error:raise ValueError('invalid compressed object') from error
    E.require(len(data)==row['size'] and not decoder.unconsumed_tail and not decoder.unused_data and decoder.eof,
              'compressed object expansion/termination')
    E.require(digest_bytes(data)==row['sha256'],'decoded object hash substitution')
    return data

def archive_admission(path, destination):
    """Check ZIP declarations before CRC expansion or extraction begins."""
    with zipfile.ZipFile(path) as archive:
        items=archive.infolist();names=set();size=0
        E.require(len(items)<=MAX_FILES,'archive member count exceeds bound')
        for item in items:
            E.safe(item.filename)
            E.require(item.filename not in names,'duplicate archive destination');names.add(item.filename)
            kind=item.external_attr>>16
            E.require(item.create_system==3 and stat.S_ISREG(kind) and
                      stat.S_IMODE(kind) in (0o644,0o755),'archive exact mode/type')
            E.require(not item.flag_bits&1 and item.compress_type in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED),
                      'unsupported archive encoding')
            E.require(0<=item.file_size<=MAX_FILE,'archive file expansion limit')
            size+=item.file_size
            E.require(size<=MAX_ARCHIVE_PAYLOAD,'archive payload expansion limit')
    available=Path(destination).absolute().parent
    while not available.exists():available=available.parent
    E.require(shutil.disk_usage(available).free>=size+WORKSPACE_RESERVE,'insufficient archive extraction space')
    return {'members':len(items),'declared_payload_bytes':size,'limit_bytes':MAX_ARCHIVE_PAYLOAD}

def materialize(root, destination):
    root=Path(root).resolve();destination=Path(destination).absolute()
    E.require(not destination.exists() and root!=destination and root not in destination.parents and destination not in root.parents,
              'reconstruction requires new external scratch directory')
    for p in destination.parents:E.require(not p.is_symlink(),'scratch ancestor symlink')
    value=validate(root)
    direct={p.relative_to(root).as_posix():p.stat().st_size for p in root.rglob('*')
            if p.is_file() and p.relative_to(root).parts[0]!='objects'}
    destinations={row.get('blob',row['path']):row['size'] for row in value['raw']+value['synthetic_deltas']}
    required=sum(direct.values())+sum(size for name,size in destinations.items() if name not in direct)
    available=destination.parent
    while not available.exists():available=available.parent
    E.require(shutil.disk_usage(available).free>=required+WORKSPACE_RESERVE,'insufficient reconstruction scratch space')
    destination.mkdir(parents=True,exist_ok=False)
    # Direct source and metadata are small, immutable inputs. Objects are streamed
    # to their exact logical locations, never copied as another object store.
    for path in sorted(root.rglob('*')):
        name=path.relative_to(root)
        if name.parts[0]=='objects':continue
        if path.is_dir():continue
        regular(path);target=destination/name;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,target);os.chmod(target,stat.S_IMODE(path.stat().st_mode))
    blob_index={r['path']:r for r in value['blobs']}
    for row in value['raw']+value['synthetic_deltas']:
        target=destination/row.get('blob',row['path'])
        mode=0o644 if 'blob' in row else row['os_permissions']
        if target.exists():
            E.require(target.is_file() and E.sha(target)==row['sha256'] and target.stat().st_size==row['size'] and
                      (row.get('blob','').startswith('source/') or stat.S_IMODE(target.stat().st_mode)==mode),
                      'reconstruction conflicts with direct payload')
            continue
        target.parent.mkdir(parents=True,exist_ok=True)
        with target.open('xb') as out:
            for ref in row['chunks']:
                if ref['path'].startswith('objects/'):out.write(decode(root/ref['path'],blob_index[ref['path']]))
                else:
                    with (root/ref['path']).open('rb') as stream:shutil.copyfileobj(stream,out,CHUNK)
        os.chmod(target,mode)
        E.require(target.stat().st_size==row['size'] and E.sha(target)==row['sha256'],'reconstructed bytes differ')
    return value

def size_report(root, archive=None):
    root=Path(root);value=index(root);rows=value['raw']+value['synthetic_deltas']
    logical=sum(r['size'] for r in rows);whole={};groups={};owners={}
    for row in rows:
        whole.setdefault(row['sha256'],row['size'])
        group=groups.setdefault(row['purpose'],{'logical_bytes':0,'files':0,'consumers':set(),
                                               'attributed_stored_chunk_bytes':0})
        group['logical_bytes']+=row['size'];group['files']+=1;group['consumers'].update(row['consumers'])
        for ref in row['chunks']:
            if ref['path'].startswith('objects/'):owners.setdefault(ref['path'],row['purpose'])
    for blob in value['blobs']:
        groups[owners[blob['path']]]['attributed_stored_chunk_bytes']+=blob['stored_size']
    payload=sum(r['size'] for r in value['blobs'])
    source=json.loads((root/'source-manifest.json').read_text())
    source_bytes=sum(r['size'] for r in source['files'])
    report={'schema':1,'target_compressed_bytes':100*1024*1024,'reconstructed_evidence_bytes':logical,
        'current_source_bytes':source_bytes,'reconstructed_logical_bytes':logical+source_bytes,
        'unique_content_bytes':payload+source_bytes,'chunk_payload_bytes':payload,
        'stored_chunk_bytes':sum(r['stored_size'] for r in value['blobs']),
        'exact_whole_file_duplicate_savings':logical-sum(whole.values()),
        'total_content_savings':logical-payload,
        'savings_rule':'Whole-file duplicates are included in total savings; categories must not be added.',
        'group_attribution':'Each stored chunk is charged once to its first purpose in canonical manifest order; direct source, metadata and ZIP overhead are separate.',
        'logical_files':len(rows),'source_files':len(source['files']),'unique_chunks':len(value['blobs']),
        'purpose_groups':{k:dict(v,consumers=sorted(v['consumers'])) for k,v in groups.items()},
        'largest_20_objects':sorted(value['blobs'],key=lambda r:r['stored_size'],reverse=True)[:20],
        'largest_20_logical_files':[{k:r[k] for k in ('path','size','purpose','consumers')}
            for r in sorted(rows,key=lambda r:r['size'],reverse=True)[:20]],
        'local_only':value['local_only'],'budget_exceptions':[]}
    if archive:
        import zipfile
        with zipfile.ZipFile(archive) as z:
            compressed=sum(i.compress_size for i in z.infolist());size=Path(archive).stat().st_size
            report.update(compressed_bytes=size,compressed_payload_bytes=compressed,
                physical_zip_overhead_bytes=size-compressed,members=len(z.infolist()),
                archive_logical_bytes=sum(i.file_size for i in z.infolist()))
        if size>100*1024*1024:
            report['budget_exceptions']=[{'reason':'Irreducible selected current raw proof after exact content deduplication',
                'bytes_above_target':size-100*1024*1024,
                'required_groups':[{ 'purpose':name,'attributed_stored_chunk_bytes':group['attributed_stored_chunk_bytes'],
                    'consuming_validators':sorted(group['consumers'])}
                    for name,group in sorted(groups.items(),key=lambda item:item[1]['attributed_stored_chunk_bytes'],reverse=True)
                    if group['attributed_stored_chunk_bytes']],
                'boundary':'Only selected current contracts and named baseline dependencies; no retired or development campaign tree.'}]
    return report
