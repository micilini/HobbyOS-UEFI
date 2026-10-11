#!/usr/bin/env python3
"""Reconciled source scan. Call parsing balances parentheses and excludes comments/definitions."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent


def code_only(text):
    def blank(match): return ''.join('\n' if c == '\n' else ' ' for c in match[0])
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', blank, text, flags=re.S)


def enclosing(code, offset):
    # Function bodies start at column zero in this tree; control statements do not.
    definitions = list(re.finditer(r'(?m)^[A-Za-z_][\w\s*]*(?:__attribute__\s*\(\([^\n]*\)\)\s*)?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', code[:offset]))
    return definitions[-1][1] if definitions else 'translation_unit'


def calls(text, names):
    code = code_only(text)
    for match in re.finditer(r'\b(' + '|'.join(names) + r')\s*\(', code):
        start, depth, i = match.end(), 1, match.end()
        while i < len(code) and depth:
            if code[i] == '(': depth += 1
            elif code[i] == ')': depth -= 1
            i += 1
        if depth: raise ValueError('unbalanced call')
        next_token = code[i:].lstrip()[:1]
        prefix = code[code.rfind('\n', 0, match.start()) + 1:match.start()]
        if next_token == '{' or re.fullmatch(r'\s*(?:extern\s+)?(?:void|uint64_t)\s*\*?\s*', prefix): continue
        args, cursor, nested = [], start, 0
        for at in range(start, i - 1):
            if code[at] == '(': nested += 1
            elif code[at] == ')': nested -= 1
            elif code[at] == ',' and not nested:
                args.append(' '.join(text[cursor:at].split())); cursor = at + 1
        args.append(' '.join(text[cursor:i - 1].split()))
        yield {'api': match[1], 'line': code.count('\n', 0, match.start()) + 1,
               'function': enclosing(code, match.start()), 'arguments': args,
               'anchor': ' '.join(text[match.start():i].split())}


def classify(path, row):
    api, function, args = row['api'], row['function'], row['arguments']
    if api == 'paging_get_physical_address':
        return 'Legacy mapping-query compatibility', 'Explicit virtual input; zero remains ambiguous for unmigrated API callers'
    if path.endswith('/xhci.c'):
        if function == 'xhci_init': return 'MMIO', 'PCI BAR plus checked register-window offset'
        if args[0].startswith(('out_phys', 'page')) and function != 'xhci_init':
            return 'DMA physical-pointer identity remap', 'DCBAA output-context physical address or validated owned context page'
        if args[0] in ('ring_virt', 'buf_virt'):
            return 'DMA PMM identity mapping', 'Physical allocation retained separately from checked CPU pointer'
        return 'DMA existing heap mapping reinstalled', 'Existing CPU pointer and checked whole-range physical translation'
    if path.endswith('/acpi_wdat.c'): return 'Firmware-described MMIO', 'Validated WDAT system-memory GAS; port addresses remain numeric'
    if path.endswith('/pci.c'):
        if 'mcfg' in args[0]: return 'Firmware table redundant identity remap', 'Checked ACPI MCFG CPU pointer and retained physical range'
        return 'MMIO configuration space', 'MCFG base plus unchanged bus/device/function arithmetic'
    return 'MMIO', 'MADT controller physical address and checked CPU register view' if '/apic/' in path else 'HPET ACPI GAS and checked CPU register view'


def integer_expression(expression, definitions):
    expression = re.sub(r'(?<=\d)(?:ULL|LLU|UL|LU|LL|U|L)\b', '', expression, flags=re.I)
    tree = ast.parse(expression, mode='eval')
    def value(node):
        if isinstance(node, ast.Expression): return value(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int): return node.value
        if isinstance(node, ast.Name) and node.id in definitions: return definitions[node.id]
        if isinstance(node, ast.BinOp):
            a, b = value(node.left), value(node.right)
            if isinstance(node.op, ast.BitOr): return a | b
            if isinstance(node.op, ast.LShift): return a << b
        raise ValueError('unresolved mapping flag expression: ' + expression)
    return value(tree)


def mapping_flags(expression, text, line):
    header = (ROOT / 'kernel/src/memory/paging.h').read_text()
    definitions = {}
    for name, body in re.findall(r'^#define (PAGE_\w+) ([^\n]+)$', header, re.M):
        definitions[name] = integer_expression(body, definitions)
    anchors = []
    prefix = '\n'.join(text.splitlines()[:line - 1])
    for name in re.findall(r'\b[a-z_]\w*\b', expression):
        declarations = list(re.finditer(r'const uint64_t ' + re.escape(name) + r'\s*=\s*([^;]+);', prefix))
        if declarations:
            declaration = declarations[-1]
            definitions[name] = integer_expression(declaration[1], definitions)
            anchors.append({'expression': declaration[0], 'line': prefix.count('\n', 0, declaration.start()) + 1})
    flags = integer_expression(expression, definitions)
    bit = lambda name: int(bool(flags & definitions[name]))
    pat, pcd, pwt = bit('PAGE_PAT'), bit('PAGE_PCD'), bit('PAGE_PWT')
    meaning = f"P={bit('PAGE_PRESENT')} RW={bit('PAGE_RW')} US={bit('PAGE_USER')} PCD={pcd} PWT={pwt} NX={bit('PAGE_NX')}; PAT bit={pat}; PAT slot index={(pat << 2) | (pcd << 1) | pwt}"
    return meaning, {'value': flags, 'definitions': definitions, 'local_anchors': anchors,
                     'header_sha256': hashlib.sha256(header.encode()).hexdigest()}


def scan():
    paths = sorted(p.relative_to(ROOT).as_posix()
                   for prefix in ('kernel', 'bootloader', 'shared')
                   for p in (ROOT / prefix).rglob('*')
                   if p.is_file() and p.suffix in ('.c', '.h', '.S', '.asm', '.ld'))
    files, mapping, allocations, casts, offsets = [], [], [], [], []
    for name in paths:
        data = (ROOT / name).read_bytes(); text = data.decode(); code = code_only(text)
        files.append({'path': name, 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)})
        if name.endswith('.c'):
            if not name.startswith('kernel/src/memory/'):
                for row in calls(text, ('paging_map', 'paging_get_physical_address')):
                    category, origin = classify(name, row)
                    row.update(path=name, category=category, address_origin=origin)
                    if row['api'] == 'paging_map':
                        flags = row['arguments'][2]
                        row['flag_meaning'], row['flag_proof'] = mapping_flags(flags, text, row['line'])
                    mapping.append(row)
            for row in calls(text, ('kmalloc_aligned', 'kmalloc', 'pmm_alloc_frame', 'pmm_alloc_contiguous_frames')):
                row['path'] = name; allocations.append(row)
        for match in re.finditer(r'\([^();\n]*\*\)\s*(?:\([^;\n]*\)|[A-Za-z_][\w.>\[\]-]*)', code):
            line = code.count('\n', 0, match.start()) + 1
            casts.append({'path': name, 'line': line, 'function': enclosing(code, match.start()),
                          'anchor': text.splitlines()[line - 1].strip()})
        for match in re.finditer(r'\b(?:HHDM_OFFSET|KERNEL_VIRT_OFFSET|VIRT_TO_PHYS_OFFSET)\b|0x[fF]{8}80000000', code):
            line = code.count('\n', 0, match.start()) + 1
            offsets.append({'path': name, 'line': line, 'function': enclosing(code, match.start()),
                            'anchor': text.splitlines()[line - 1].strip()})
    counts = {api: sum(r['api'] == api for r in mapping) for api in ('paging_map', 'paging_get_physical_address')}
    counts['xhci_paging_map'] = sum(r['api'] == 'paging_map' and r['path'].endswith('/xhci.c') for r in mapping)
    return {'schema': 1, 'base_revision': '0f6629ce6ee9c9e04fdde236ddac36f5b5531d49',
            'line_anchor': 'Per-file SHA-256 below; physical-boundary substitutions are enumerated in the current scope contract.',
            'method': 'Comment/string masking, balanced call parentheses, function/definition discrimination; broad pointer casts require semantic reconciliation in address-consumers.md.',
            'source_sha256': hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
            'files': files, 'counts': counts, 'external_calls': mapping,
            'allocations': allocations, 'pointer_cast_candidates': casts, 'offset_uses': offsets}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    result = scan()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'status': 'PASS', 'counts': result['counts'], 'output': str(a.output)}))


if __name__ == '__main__': main()
