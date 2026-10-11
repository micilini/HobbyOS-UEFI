#!/usr/bin/env python3
"""Synthetic transport controls; no captured boot or accepted file is modified."""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import tempfile
import stat
import struct
import zipfile
import zlib
import memory_evidence as E
import review_transport as R

REQUIRED={'traversal','duplicate-destination','conflicting-reference','unlisted-object','missing-object',
          'hash-substitution','logical-expansion','chunk-expansion','unknown-type','wrong-mode',
          'unconsumed-object','source-substitution','inside-extraction','existing-scratch','ancestor-symlink',
          'compressed-expansion','compressed-codec','whole-history-selection','unbound-dependency',
          'archive-expansion','archive-mode','scratch-exhaustion'}

def run(output):
    output=Path(output);output.mkdir(parents=True,exist_ok=False);cases=[]
    E.require(not R.local_binary_only('.img',b'fake',13,True) and
              not R.local_binary_only('.elf',b'\x7fELF',65536,True) and
              R.local_binary_only('.img',b'fake',13,False) and
              R.local_binary_only('.img',b'boot',67108864,True),
              'native fixture stand-in/full binary selection boundary')
    with tempfile.TemporaryDirectory(prefix='review-transport-controls-') as temporary:
        root=Path(temporary);package=root/'package';(package/'source').mkdir(parents=True)
        source=package/'source/input.txt';source.write_text('Source remains directly readable.\n')
        E.write(package/'source-manifest.json',{'files':[{'path':'input.txt','size':source.stat().st_size,'sha256':E.sha(source),'mode':'100644'}]})
        original=root/'capture.log';original.write_bytes(b'Exact original framed bytes\n'*50000)
        second=root/'cumulative.log';second.write_bytes(original.read_bytes()+b'next receipt\n')
        store=R.Store(package)
        store.add(original,'evidence/original.log','current-raw',['byte-replay'])
        store.add(original,'evidence/alias.log','current-raw',['byte-replay'])
        store.add(second,'evidence/cumulative.log','current-raw',['byte-replay'])
        store.add(source,'baseline-source/input.txt','prior-source',['source-tree'])
        store.finish([]);pristine=json.loads((package/'transport-manifest.json').read_text())
        R.materialize(package,root/'restored')
        E.require((root/'restored/evidence/original.log').read_bytes()==original.read_bytes() and
                  (root/'restored/evidence/cumulative.log').read_bytes()==second.read_bytes(),
                  'positive lossless byte reconstruction failed')
        def manifest(value):
            (package/'transport-manifest.json').write_bytes(E.canonical(value))
            (package/'content-selection.json').write_bytes(E.canonical({'schema':2,'manifest':'transport-manifest.json',
                'sha256':E.sha(package/'transport-manifest.json')}))
        def reject(name,reason,action):
            try:action()
            except (ValueError,OSError) as error:
                actual=str(error);E.require(reason in actual,'transport rejection boundary: '+name+': '+actual)
            else:raise ValueError('transport negative accepted: '+name)
            cases.append({'name':name,'synthetic':True,'expected_boundary':reason,'actual_reason':actual.replace(str(root),'<SCRATCH>'),
                          'exit_status':1})
        selected=copy.deepcopy(pristine)
        for row in selected['raw']:
            if row['path'].startswith('evidence/'):
                row['path']=row['path'].replace('evidence/','evidence/current-campaign/',1)
        R.selection_audit(selected)
        historical=copy.deepcopy(selected);historical['raw'][0]['path']='evidence/retired/entire-campaign/capture.log'
        reject('whole-history-selection','unapproved review selection root',lambda:R.selection_audit(historical))
        unbound=copy.deepcopy(selected);unbound['raw'][0]['consumers']=['receipt:missing.json']
        reject('unbound-dependency','no consuming receipt',lambda:R.selection_audit(unbound))
        for name,reason,change in [
            ('traversal','unsafe evidence path',lambda v:v['raw'][0].update(path='../escape')),
            ('duplicate-destination','duplicate transport destination',lambda v:v['raw'].append(copy.deepcopy(v['raw'][0]))),
            ('conflicting-reference','conflicting reconstruction destination',lambda v:v['raw'][1].update(blob=v['raw'][0]['path'],sha256='0'*64)),
            ('hash-substitution','transport reconstructed hash',lambda v:v['raw'][1].update(sha256='0'*64)),
            ('logical-expansion','transport mode/size limit',lambda v:v['raw'][1].update(size=R.MAX_FILE+1)),
            ('chunk-expansion','transport chunk expansion limit',lambda v:v['raw'][1]['chunks'][0].update(size=R.CHUNK+1)),
            ('unconsumed-object','transport unused selection',lambda v:v['raw'][0].update(consumers=[])),
            ('wrong-mode','transport mode/size limit',lambda v:v['raw'][0].update(os_permissions=0o664)),
            ('compressed-codec','compressed object bounds/codec',lambda v:v['blobs'][0].update(codec='unrecognized')),
        ]:
            value=copy.deepcopy(pristine);change(value);manifest(value)
            reject(name,reason,lambda:R.validate(package));manifest(pristine)
        extra=package/'objects'/('0'*64);extra.write_text('unlisted')
        reject('unlisted-object','unexpected transport object',lambda:R.validate(package));extra.unlink()
        blob=package/pristine['blobs'][0]['path'];saved=blob.read_bytes();blob.unlink()
        reject('missing-object','No such file',lambda:R.validate(package));blob.write_bytes(saved);os.chmod(blob,0o644)
        blob.unlink();blob.symlink_to(source)
        reject('unknown-type','transport nonregular object type',lambda:R.validate(package));blob.unlink();blob.write_bytes(saved);os.chmod(blob,0o644)
        malicious=zlib.compress(b'x'*(R.CHUNK*2));blob.write_bytes(malicious)
        expanded=copy.deepcopy(pristine);expanded['blobs'][0].update(stored_size=len(malicious),stored_sha256=R.digest_bytes(malicious))
        manifest(expanded)
        reject('compressed-expansion','compressed object expansion/termination',lambda:R.validate(package))
        blob.write_bytes(saved);manifest(pristine)
        before=source.read_bytes();source.write_bytes(b'bad')
        reject('source-substitution','transport chunk hash/size substitution',lambda:R.validate(package));source.write_bytes(before)
        reject('inside-extraction','external scratch',lambda:R.materialize(package,package/'scratch'))
        reject('existing-scratch','external scratch',lambda:R.materialize(package,root/'restored'))
        (root/'alias').symlink_to(root/'restored',target_is_directory=True)
        reject('ancestor-symlink','scratch ancestor symlink',lambda:R.materialize(package,root/'alias/new'))
        usage=R.shutil.disk_usage
        try:
            R.shutil.disk_usage=lambda p:shutil._ntuple_diskusage(1,1,0)
            reject('scratch-exhaustion','insufficient reconstruction scratch space',lambda:R.materialize(package,root/'too-large'))
        finally:R.shutil.disk_usage=usage
        compressed=root/'controlled.zip'
        info=zipfile.ZipInfo('manifest.json');info.create_system=3;info.external_attr=(stat.S_IFREG|0o644)<<16
        with zipfile.ZipFile(compressed,'x') as archive:archive.writestr(info,b'{}\n')
        R.archive_admission(compressed,root/'extract')
        clean=compressed.read_bytes();central=clean.index(b'PK\x01\x02')
        mutated=bytearray(clean);struct.pack_into('<I',mutated,central+24,R.MAX_FILE+1);compressed.write_bytes(mutated)
        reject('archive-expansion','archive file expansion limit',lambda:R.archive_admission(compressed,root/'extract'))
        mutated=bytearray(clean);struct.pack_into('<I',mutated,central+38,(stat.S_IFREG|0o664)<<16);compressed.write_bytes(mutated)
        reject('archive-mode','archive exact mode/type',lambda:R.archive_admission(compressed,root/'extract'))
        compressed.write_bytes(clean)
        R.validate(package)
        report=R.size_report(package)
        E.require(report['exact_whole_file_duplicate_savings']==original.stat().st_size and
                  report['total_content_savings']>report['exact_whole_file_duplicate_savings'],
                  'prefix and exact deduplication positive control')
        E.require(sum(g['attributed_stored_chunk_bytes'] for g in report['purpose_groups'].values())==
                  sum(p.stat().st_size for p in (package/'objects').iterdir()),
                  'stored group attribution duplicates or omits bytes')
    value={'schema':1,'status':'PASS','positive':['exact-bytes','current-source-direct','whole-file-dedup','prefix-chunks','immutable-originals',
           'small-synthetic-payload-retained','full-vm-image-local','nonoverlapping-size-attribution'],
           'cases':cases}
    verify(value);E.write(output/'results.json',value);return value

def verify(value):
    E.require(value['status']=='PASS' and len(value['cases'])==len(REQUIRED) and
              {r['name'] for r in value['cases']}==REQUIRED,'transport fixture coverage')
    for row in value['cases']:
        E.require(row['synthetic'] and row['exit_status']==1 and row['expected_boundary'] in row['actual_reason'],
                  'transport fixture wrong boundary')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();print(json.dumps(run(args.output),sort_keys=True))
