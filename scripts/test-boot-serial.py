#!/usr/bin/env python3
"""Execute the real boot UART writers and GOP formatter with hosted port seams."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--source', type=Path)
parser.add_argument('--expect-fragment-reset', action='store_true')
args = parser.parse_args()
if args.expect_fragment_reset and not args.source:
    parser.error('The before witness requires an explicit immutable source path')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
source_path = (args.source or ROOT / 'bootloader/src/serial.c').resolve()
source = source_path.read_text()
original = source
old_out = '''static inline void outb_u8(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}'''
old_in = '''static inline uint8_t inb_u8(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}'''
for old, new in [(old_out, 'static inline void outb_u8(uint16_t p, uint8_t v) { boot_serial_host_out(p,v); }'),
                 (old_in, 'static inline uint8_t inb_u8(uint16_t p) { return boot_serial_host_in(p); }')]:
    if source.count(old) != 1:
        raise ValueError('Privileged seam does not match the actual source')
    source = source.replace(old, new)
actual = out / 'serial.actual.c'
actual.write_text(source)
main = (ROOT / 'bootloader/main.c').read_text()
assert main.index('serial_broadcast_boot_banner(&boot_info);') < main.index('get_framebuffer(&boot_info)')
executions = []
for mode, flags in [('normal', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all']),
                    ('asan', ['-fsanitize=address', '-fno-omit-frame-pointer'])]:
    binary = out / mode
    command = ['gcc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-fshort-wchar', '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections', '-DEFI_FUNCTION_WRAPPER',
               '-I/usr/include/efi', '-I/usr/include/efi/x86_64', '-I/usr/include/efi/protocol',
               '-I' + str(ROOT / 'bootloader/src'),
               '-DBOOT_SERIAL_ACTUAL_SOURCE="' + str(actual) + '"',
               '-DBOOT_GOP_ACTUAL_SOURCE="' + str(ROOT / 'bootloader/src/gop.c') + '"',
               *flags, *(['-DEXPECT_FRAGMENT_RESET=1'] if args.expect_fragment_reset else []),
               str(ROOT / 'scripts/boot-serial-host.c'), '-o', str(binary)]
    for phase, cmd in [('compile', command), ('execute', [str(binary)])]:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        row = dict(mode=mode, phase=phase, command=cmd, exit=result.returncode,
                   stdout=result.stdout, stderr=result.stderr)
        executions.append(row)
        (out / 'commands.json').write_text(json.dumps(executions, indent=2) + '\n')
        if result.returncode:
            raise SystemExit(json.dumps(row))
receipt = dict(status='INCOMING_FRAGMENT_RESET_REPRODUCED' if args.expect_fragment_reset else 'PASS',
               actual_executions=3, actual_serial_source=str(source_path),
               source_sha256=hashlib.sha256(original.encode()).hexdigest(),
               gop_sha256=hashlib.sha256((ROOT / 'bootloader/src/gop.c').read_bytes()).hexdigest(),
               commands=executions, authority='Actual boot functions with substituted UART hardware; not electrical delivery proof')
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({k: v for k, v in receipt.items() if k != 'commands'}))
