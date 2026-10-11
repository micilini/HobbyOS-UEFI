#!/usr/bin/env python3
"""Compile and exercise the actual shell registry for each supported build role."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NORMAL = "help clear version mem shutdown restart cpu pci acpi echo ps kill taskman".split()
DEVELOPMENT = "panic irq usbdiag smpstress taskdiag tasktest schedtest synctest accounttest killtest reaptest inputtest modaltest taskmantest diagnosticresulttest locktest alloctest profiletest ptinspect tlbtest mappingtest lowmap wxtest stacktest phystest rammap cpustate memmap".split()
ALIASES = {"int": "irq", "pnc": "panic", "usb": "usbdiag", "stress": "smpstress", "td": "taskdiag", "tt": "tasktest", "st": "schedtest", "kt": "killtest", "rt": "reaptest", "it": "inputtest", "mt": "modaltest", "tmt": "taskmantest", "accttest": "accounttest", "sync-test": "synctest"}


def main():
    headers = '#include <assert.h>\n#include <string.h>\n#include "kernel/src/shell/commands/registry.h"\n'
    stubs = "\n".join(f"int cmd_{name}(int argc, char **argv) {{ (void)argc; (void)argv; return 7; }}" for name in NORMAL + DEVELOPMENT + ["irqlatency"])
    source = headers + stubs + '\nint diagnostic_result_complete(const char *name, int rc) { assert(name && *name); return rc; }\nint main(void) {\nuint64_t violations=99; uint32_t count=0; assert(shell_registry_validate(&violations) && violations==0); assert(shell_registry_get_all(&count));\n'
    source += 'assert(count == 13 + (DEVELOPMENT_ENABLED ? 28 : 0) + (LATENCY_ENABLED ? 1 : 0));\n'
    source += 'assert(!shell_registry_find(NULL)); assert(!shell_registry_find("")); assert(!shell_registry_find("unknown-command"));\n'
    for name in NORMAL:
        source += f'assert(shell_registry_find("{name}"));\n'
    for name in DEVELOPMENT:
        source += f'assert(!!shell_registry_find("{name}") == DEVELOPMENT_ENABLED);\n'
    source += 'assert(!!shell_registry_find("irqlatency") == LATENCY_ENABLED);\n'
    for alias, name in ALIASES.items():
        source += f'assert(shell_registry_find("{alias}") == shell_registry_find("{name}"));\n'
    source += 'assert(shell_registry_find("top") == shell_registry_find("taskman"));\n'
    source += 'for (unsigned i=0;i<count;i++) { const ShellCommand *c=shell_registry_get_all(NULL)+i; char *argv[]={(char *)c->name,"check"}; assert(shell_registry_find(c->name)==c); assert(c->handler(2,argv)==7); if(c->aliases) for(const char *const *a=c->aliases;*a;a++) assert(shell_registry_find(*a)==c); }\nreturn 0; }\n'
    roles = {"production": [], "debug": ["-DHOBBYOS_DEBUG_ASSERT=1"], "selftest": ["-DHOBBYOS_SELFTEST=1"], "latency": ["-DHOBBYOS_SELFTEST=1", "-DHOBBYOS_IRQ_LATENCY=1"]}
    executions = 0
    with tempfile.TemporaryDirectory(prefix="hobbyos-command-policy-") as tmp:
        path = Path(tmp)
        test = path / "registry-test.c"
        test.write_text(source)
        for role, flags in roles.items():
            for sanitizer in ("normal", "undefined", "address"):
                output = path / f"{role}-{sanitizer}"
                cmd = [os.environ.get("CC", "gcc"), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), "-DHOBBYOS_IRQ_LATENCY=0"]
                if role == "latency":
                    cmd.remove("-DHOBBYOS_IRQ_LATENCY=0")
                cmd += [f"-DDEVELOPMENT_ENABLED={int(role in ('selftest', 'latency'))}", f"-DLATENCY_ENABLED={int(role == 'latency')}"] + flags
                if sanitizer != "normal":
                    cmd += [f"-fsanitize={sanitizer}", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie"]
                cmd += [str(ROOT / "kernel/src/shell/commands/registry.c"), str(test), "-o", str(output)]
                subprocess.run(cmd, check=True)
                subprocess.run([str(output)], check=True)
                executions += 1
                print(f"command-policy: PASS role={role} mode={sanitizer}", flush=True)
    print(f"command-policy: PASS actual_registry_executions={executions}")


if __name__ == "__main__":
    main()
