"""Explicit rolling scope rooted at the accepted pre-control-policy commit."""
import json
import re
import subprocess
from pathlib import Path
import memory_evidence as E

ROOT = Path(__file__).resolve().parent.parent


def instruction_sites(root=ROOT):
    """Inventory exact source instruction lines, without permitting entire files."""
    result=[]
    for path in sorted((root/'kernel').rglob('*')):
        if path.suffix not in ('.c','.h','.S'):continue
        source=re.sub(r'/\*.*?\*/|//[^\n]*','',path.read_text(),flags=re.S)
        if path.suffix != '.S':
            blocks=re.findall(r'\b(?:__asm__|__asm|asm)\s*(?:(?:volatile|__volatile__|__volatile)\s*)?\((?:"(?:\\.|[^"\\])*"|[^;])*?\);',source,re.S)
            selected=[('',' '.join(block.split())) for block in blocks
                      if re.search(r'\b(cpuid|wrmsr|rdmsr|wbinvd|invd|invlpg)\b|\bcr[034]\b',block)]
        else:
            label='';selected=[]
            for line in source.splitlines():
                match=re.fullmatch(r'([.\w]+):',line.strip())
                if match:label=match[1]
                if re.search(r'\b(cpuid|wrmsr|rdmsr|wbinvd|invd|invlpg|cr[034])\b',line):
                    selected.append((label,line.strip()))
        ordinal={}
        for label,text in selected:
            key=(label,text);ordinal[key]=ordinal.get(key,0)+1
            result.append({'path':path.relative_to(root).as_posix(),'label':label,
                           'instruction':text,'occurrence':ordinal[key]})
    return result


def audit_sites(root=ROOT):
    value=json.loads((root/'scripts/cpu-control-scope.json').read_text())
    E.require(instruction_sites(root)==value['instruction_sites'],'unauthorized architectural instruction site')
    return {'sites':len(value['instruction_sites']),'sha256':E.identity(value['instruction_sites'])}


def contract(root=ROOT):
    value = json.loads((root / 'scripts/cpu-control-scope.json').read_text())
    E.require(value['base_commit'] == '4dcbcd2a634c9b4ca864277af613434e7f9d7075' and
              value['base_tree'] == 'd0284110ab81d2ffd09583c5ff5d6bc55281ab77', 'control scope baseline')
    previous = value['previous_contract']
    E.require(E.sha(root / previous['path']) == previous['sha256'], 'historical scope modified')
    for name, digest in value['immutable_reviewed_anchors'].items():
        E.require(E.sha(root / name) == digest, 'control protected anchor drift: ' + name)
    return value


def scope(family):
    value = contract()
    paths = subprocess.check_output(['git', 'diff', '--name-only', '-z', value['base_commit']], cwd=ROOT).decode().split('\0')
    paths += subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
    changed = {p for p in paths if p and Path(p).suffix not in E.GENERATED_SUFFIXES}
    E.require(changed <= set(value['allowed_paths']), 'unrelated control source edits: ' + ', '.join(sorted(changed - set(value['allowed_paths']))))
    return {'schema': 1, 'status': 'PASS', 'family': family,
            'contract': value['name'], 'base_commit': value['base_commit'],
            'scope_contract_sha256': E.sha(ROOT / 'scripts/cpu-control-scope.json'),
            'changed_paths': sorted(changed), 'protected': value['immutable_reviewed_anchors'],
            'superseded_scope': 'approved control/bootstrap changes; unchanged native semantic validators'}
