#!/usr/bin/env python3
"""Select current memory evidence explicitly; keep large executable artifacts local."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import memory_evidence as E
import memory_elf_proof as ELF

ROOT = Path(__file__).resolve().parent.parent
TOP = ('static.json', 'inventory.json', 'build.json', 'build-reuse.json', 'series.json',
       'runtime.json', 'baseline.json', 'production.json', 'regression-selection.json',
       'regression-ledger.json', 'acceptance.json', 'layout-roles.json', 'current-requirement-inventory.json')
GROUPS = ('candidates', 'profiles', 'diagnostics', 'campaign-attempts', 'production-attempts',
          'regression-attempts', 'reused-build-commands', 'layout-role-receipts', 'attempts', 'fixture-attempts')


def read(path): return json.loads(path.read_text())


class Selection:
    def __init__(self, review):
        self.review = review
        self.files = []; self.binaries = []; self.synthetic = []
        self.proofs = {}
        self.source_blobs = {r['sha256']: 'source/' + r['path'] for r in read(review / 'source-manifest.json')['files']}

    def proof(self, path, digest):
        if digest in self.proofs: return self.proofs[digest]
        result = {'sha256': digest, 'size': path.stat().st_size}
        if path.name.endswith('.elf') and 'fixtures' not in path.parts:
            try:
                if path.name == 'kernel-orphan.elf':
                    elf = ELF.V.ElfFile.from_path(path)
                    proof = ELF.capture(path, orphan_payload=elf.section_data(elf.section('.layout_probe')))
                else:
                    proof = ELF.capture(path)
            except ELF.V.ValidationError:
                # An orphan probe is deliberately not a valid production layout.
                result['layout'] = 'NOT_A_CANONICAL_KERNEL_LAYOUT'
            else:
                relative = 'binary-facts/' + digest + '.layout.json'
                target = self.review / relative; target.parent.mkdir(exist_ok=True)
                E.write(target, proof); result['layout_proof'] = relative
                for label, argv in [('symbols', ['nm', '-an']), ('segments', ['readelf', '-lW']), ('undefined', ['nm', '-u']), ('strings', ['strings', '-a']),
                                    ('headers', ['readelf', '-W', '-h', '-S', '-l'])]:
                    target = self.review / 'binary-facts' / (digest + '.' + label + '.txt')
                    output = subprocess.check_output([*argv, str(path)])
                    target.write_bytes(output)
                    result[label] = {'path': target.relative_to(self.review).as_posix(), 'sha256': E.sha(target)}
        self.proofs[digest] = result
        return result

    def add(self, source, relative, purpose, synthetic=False):
        E.safe(relative)
        mode = source.lstat().st_mode
        E.require(stat.S_ISREG(mode), 'unexpected evidence file type: ' + str(source))
        with source.open('rb') as stream: prefix = stream.read(4)
        size = source.stat().st_size; digest = E.sha(source)
        is_binary = source.suffix.lower() in ('.elf', '.img', '.efi', '.fd', '.o') or prefix == b'\x7fELF'
        # Small synthetic stand-ins and ELF fixtures are deliberately transportable.
        focused_fixture = synthetic and size < 128 * 1024 and source.suffix.lower() not in ('.img', '.fd')
        stand_in = synthetic and size < 4096 and prefix != b'\x7fELF'
        row = {'path': relative, 'size': size, 'sha256': digest, 'os_permissions': 0o755 if mode & 0o111 else 0o644,
               'purpose': purpose, 'local_path': str(source.resolve())}
        if is_binary and not (focused_fixture or stand_in):
            row.update(transport='LOCAL_BINARY_ONLY', local_path=str(source.resolve()), facts=self.proof(source, digest))
            self.binaries.append(row)
            return
        if synthetic:
            blob = self.source_blobs.get(digest, 'synthetic-blobs/' + digest)
            target = self.review / blob
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source, target); os.chmod(target, 0o644)
            E.require(E.sha(target) == digest, 'synthetic blob identity collision')
            row.update(transport='SYNTHETIC_FILE_DELTA', blob=blob)
            self.synthetic.append(row)
            return
        target = self.review / relative
        E.require(not target.exists(), 'duplicate selected evidence path: ' + relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target); os.chmod(target, row['os_permissions'])
        row['transport'] = 'EXACT_RAW_FILE'; self.files.append(row)

    def tree(self, source, relative, purpose):
        if not source.is_dir(): return
        for directory, names, files in os.walk(source):
            # Retain every regular file named by the native ledger. Executable
            # caches are indexed locally and their small source/receipt inputs
            # remain available; sockets and live process identifiers are omitted.
            names[:] = sorted(n for n in names if n not in ('.git', '__pycache__'))
            for name in sorted(files):
                path = Path(directory) / name
                if path.suffix in ('.sock', '.pid', '.pyc') or name == 'harness.lock': continue
                suffix = path.relative_to(source)
                synthetic = any(part in ('fixtures', 'fixture', 'replay-fixtures', 'oracle-fixtures') for part in suffix.parts)
                self.add(path, relative + '/' + suffix.as_posix(), purpose, synthetic)

    def finish(self):
        paths = [r['path'] for r in self.files + self.binaries + self.synthetic]
        E.require(len(paths) == len(set(paths)), 'duplicate content selection')
        E.write(self.review / 'content-selection.json', {'schema': 1, 'raw': self.files, 'local_binaries': self.binaries,
                'synthetic_deltas': self.synthetic, 'binary_replay_limit':
                'Offline replay verifies saved identities, raw observations and bounded ELF fragments; full omitted binaries are not executed or rehashed.'})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--campaign', type=Path, required=True)
    parser.add_argument('--review', type=Path, required=True)
    parser.add_argument('--extra', type=Path, help='Explicit additional immutable evidence roots and purposes.')
    parser.add_argument('--preview-completed', action='store_true', help='Preview completed attempts while another family is collecting; never declares acceptance.')
    args = parser.parse_args()
    session = args.session.resolve(); campaign = args.campaign.resolve(); review = args.review.resolve()
    for path in (session, campaign, review): path.relative_to(ROOT / 'backups')
    E.require((review / 'source-manifest.json').is_file(), 'export complete source before selecting evidence')
    selection = Selection(review)
    for name in TOP:
        path = campaign / name
        if path.is_file(): selection.add(path, 'evidence/current-campaign/' + name, 'current campaign receipt')
    for name in GROUPS:
        if args.preview_completed and name in ('regression-attempts', 'attempts'):
            for directory in sorted((campaign / name).glob('*')):
                terminal = 'result.json' if name == 'regression-attempts' else 'command.json'
                if (directory / terminal).is_file():
                    selection.tree(directory, 'evidence/current-campaign/' + name + '/' + directory.name, 'completed preview ' + name)
        else:
            selection.tree(campaign / name, 'evidence/current-campaign/' + name, 'current ' + name)
    selection.tree(session / 'attempts', 'evidence/repair-attempts', 'append-only repair command outcomes')
    for name in ('source-modes.json', 'initial-current-requirements.json', 'preflight.json', 'protected.json',
                 'reviewed-source-comparison.json', 'changes-staged.patch'):
        if (session / 'before' / name).is_file(): selection.add(session / 'before' / name, 'preservation/' + name, 'before-edit identity')
    selection.tree(session / 'before/lineage', 'preservation/lineage', 'original synchronization and review lineage')
    selection.tree(session / 'before/export/git', 'preservation/git-before', 'before-edit Git identity and complete tracked diff')
    selection.add(session / 'before/export/source-manifest.json', 'preservation/source-manifest-before.json', 'before-edit complete source and mode inventory')
    if args.extra:
        extra = read(args.extra)
        for row in extra['roots']:
            source = Path(row['local_path']).resolve(); source.relative_to(ROOT / 'backups')
            E.safe(row['destination'])
            if source.is_file(): selection.add(source, row['destination'], row['purpose'])
            else: selection.tree(source, row['destination'], row['purpose'])
        selection.add(args.extra.resolve(), 'extra-evidence-selection.json', 'explicit causal and historical content list')
    selection.finish()
    print(json.dumps({'status': 'SELECTED', 'raw_files': len(selection.files), 'local_binaries': len(selection.binaries),
                      'synthetic_deltas': len(selection.synthetic)}))


if __name__ == '__main__': main()
