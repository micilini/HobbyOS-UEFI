"""Local read-only candidate locations; portable proof keeps ordinary paths.

Only metadata is copied into a successor campaign. The full immutable binaries
stay at one verified canonical location; writable guest images remain independent.
"""
import json
from pathlib import Path
import shutil
import memory_evidence as E

LOCATION = 'candidate-location.json'


def resolve(directory):
    directory = Path(directory)
    path = directory / LOCATION
    if not path.exists(): return directory
    E.require(path.is_file() and not path.is_symlink(), 'candidate location type')
    row = json.loads(path.read_text())
    E.require(row.get('schema') == 1 and row.get('build_input_sha256') == E.identity(E.manifest()),
              'stale candidate location source')
    E.safe(row['relative_path'])
    E.require(row['relative_path'].startswith('backups/'), 'candidate location outside artifacts')
    source = E.ROOT / row['relative_path']
    E.require(source.resolve() == source and source.is_dir() and not (source / LOCATION).exists(),
              'candidate location cycle or symlink')
    candidate = json.loads((source / 'candidate.json').read_text())
    E.require(E.identity(candidate) == row['candidate_id'] and
              E.sha(source / 'candidate.json') == E.sha(directory / 'candidate.json') and
              E.sha(source / 'build-receipt.json') == E.sha(directory / 'build-receipt.json'),
              'candidate location identity differs')
    E.verify_candidate(candidate, json.loads((source / 'build-receipt.json').read_text()))
    return source


def artifact(path):
    path = Path(path)
    return resolve(path.parent) / path.name


def adopt(source, destination, verify):
    source, destination = resolve(source).resolve(), Path(destination)
    identity = verify(source)
    E.require(source.is_relative_to(E.ROOT / 'backups') and not destination.exists(),
              'candidate adoption ownership')
    candidate = json.loads((source / 'candidate.json').read_text())
    destination.mkdir(parents=True)
    binary_names = set(candidate['artifacts'])
    for path in source.rglob('*'):
        E.require(not path.is_symlink(), 'candidate symlink')
        if not path.is_file(): continue
        name = path.relative_to(source)
        if str(name) in binary_names: continue
        target = destination / name; target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        E.require(E.sha(path) == E.sha(target), 'candidate metadata copy differs')
    E.write(destination / LOCATION, {'schema': 1, 'relative_path': source.relative_to(E.ROOT).as_posix(),
            'candidate_id': identity, 'build_input_sha256': candidate['source_sha256'],
            'policy': 'Canonical full binaries are read-only inputs; per-run images are independent.'})
    E.require(verify(destination) == identity, 'candidate resolver verification differs')
    return identity
