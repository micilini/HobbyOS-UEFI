#!/usr/bin/env python3
"""Run literal inherited/additive interrupt checks on disposable negative copies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parent.parent
    gate=(root/'scripts/test-assert-list.sh').read_text()
    start=gate.index('interrupts = Path("kernel/src/core/interrupts.c").read_bytes()')
    end=gate.index('rows.append({', start)
    checks=gate[start:end]
    prefix='import hashlib\nfrom pathlib import Path\n'
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        contract=device_current.contract()
        # The extracted checks need the same fixed successor input as the gate.
        if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
            fixed={'promoted_sources':{'kernel/src/core/interrupts.c':contract['promoted_sources']['kernel/src/core/interrupts.c']}}
        elif os.environ.get('HOBBYOS_VALIDATION_CONTRACT')=='verified-ram-direct-map':
            fixed={'immutable_reviewed_anchors':{'kernel/src/core/interrupts.c':
                contract['immutable_reviewed_anchors']['kernel/src/core/interrupts.c']}}
        else:
            fixed={'interrupt_dispatch_sha256':contract['interrupt_dispatch_sha256']}
        prefix+='contract='+repr(fixed)+'\n'
    a.output.mkdir(parents=True,exist_ok=False)
    (a.output/'checks.py').write_text(prefix+checks)
    original=(root/'kernel/src/core/interrupts.c').read_text()
    cases={'positive':original,
           'unrelated-edit':original+'\n/* unrelated source change */\n',
           'removed-eoi':original.replace('    cpu_control_snapshot_irq();\n#endif\n    lapic_eoi();','    cpu_control_snapshot_irq();\n#endif',1),
           'duplicate-eoi':original.replace('    cpu_control_snapshot_irq();\n#endif\n    lapic_eoi();','    cpu_control_snapshot_irq();\n#endif\n    lapic_eoi();\n    lapic_eoi();',1),
           'missing-legacy-hook':original.replace('arch_test_timer_hook','missing_timer_hook'),
           'gp-cardinality':original.replace('arch_selftest_record_unhandled_gp(frame->rip, error_code, frame->cs);',';',1),
           'ungated-hook':original.replace('#ifdef HOBBYOS_SELFTEST\n    cpu_control_snapshot_irq();\n#endif','    cpu_control_snapshot_irq();',1)}
    ledger=[]
    for name,source in cases.items():
        directory=a.output/name;(directory/'kernel/src/core').mkdir(parents=True)
        (directory/'kernel/src/core/interrupts.c').write_text(source)
        command=[sys.executable,str((a.output/'checks.py').resolve())]
        result=subprocess.run(command,cwd=directory,text=True,capture_output=True)
        (directory/'stdout.log').write_text(result.stdout+result.stderr)
        expected=0 if name=='positive' else 1
        if (result.returncode==0)!=(expected==0):raise ValueError('unexpected fixed-check result: '+name)
        row={'name':name,'command':command,'expected_exit':expected,'actual_exit':result.returncode,'sha256':hashlib.sha256(source.encode()).hexdigest()}
        # Exercise the additive wake-body oracle independently of the fixed hash.
        if name in ('positive','removed-eoi','duplicate-eoi','ungated-hook'):
            wake=prefix+'interrupts=Path("kernel/src/core/interrupts.c").read_bytes()\n'+checks[checks.index('wake_start ='):]
            (directory/'wake-check.py').write_text(wake)
            w=subprocess.run([sys.executable,'wake-check.py'],cwd=directory,text=True,capture_output=True)
            (directory/'wake.log').write_text(w.stdout+w.stderr)
            if (w.returncode==0)!=(expected==0):raise ValueError('unexpected wake-check result: '+name)
            row['wake_exit']=w.returncode
        ledger.append(row)
    (a.output/'result.json').write_text(json.dumps({'status':'PASS','positive':1,'negative':6,'cases':ledger},indent=2)+'\n')
    print(json.dumps({'status':'PASS','positive':1,'negative':6}))


if __name__=='__main__':main()
