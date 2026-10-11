"""Bounded debugger capture at the private-root and retired-owner boundaries."""
import json
from pathlib import Path
import memory_evidence as E
import stack_current as STACK

NAMES={'g_bootstrap_tables','g_bootstrap_life','g_bootstrap_snapshot','g_bootstrap_freed',
    'g_bootstrap_free_before','g_bootstrap_free_after','g_observer_boot_root','g_kernel_root_physical',
    'paging_bootstrap_start','_trampoline_start','_trampoline_end','smp_trampoline_cr3',
    'smp_trampoline_stack','smp_trampoline_entry_ptr','smp_trampoline_gdtr','smp_trampoline_gdt_start',
    'ap_kernel_entry_asm','bsp_stack_bottom','bsp_stack_top','kernel_tlb_runtime_init',
    'g_bitmap_buffer','g_reserved_bitmap_buffer','g_free_frames'}
def collect(root,directory,runtime,candidate,result,run,before=False):
    phase='bootstrap-before' if before else 'bootstrap-after'
    if before:
        binary=directory/'low-layout'
        run('low-layout-build',['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
            '-DHOBBYOS_SELFTEST=1','scripts/low-readback-layout.c','-o',str(binary)])
        run('low-layout',[str(binary)])
    layout=json.loads((directory/'low-layout.stdout.log').read_text())
    symbols={s[2]:int(s[0],16) for line in (directory/'symbols.txt').read_text().splitlines()
        if len(s:=line.split())==3 and s[2] in NAMES}
    E.require(set(symbols)==NAMES,'bootstrap capture current ELF symbols')
    cfg={'output':str(directory),'symbols':symbols,'layout':layout,'phase':phase,
        'candidate_id':result['candidate_id'],'vm_id':result['vm_id'],'boot_id':result['boot_id']}
    E.write(directory/(phase+'-config.json'),cfg)
    source=directory/(phase+'-capture.py');source.write_bytes((root/'scripts/low-gdb-capture.py').read_bytes())
    script=directory/(phase+'.gdb')
    lines=['set pagination off','set confirm off','target remote '+str(runtime/'gdb.sock')]
    if before:lines += [f'hbreak *0x{symbols["paging_bootstrap_start"]:x}','continue']
    lines += [f'python exec(compile(open({json.dumps(str(source))}).read(), "low-capture", "exec"), dict(LOW_CAPTURE_CONFIG={json.dumps(str(directory/(phase+"-config.json")))}))']
    if before:
        retired=cfg|{'phase':'bootstrap-retired'}
        E.write(directory/'bootstrap-retired-config.json',retired)
        lines+=['delete breakpoints',f'hbreak *0x{symbols["kernel_tlb_runtime_init"]:x}','continue',
            f'python exec(compile(open({json.dumps(str(source))}).read(), "low-capture", "exec"), dict(LOW_CAPTURE_CONFIG={json.dumps(str(directory/"bootstrap-retired-config.json"))}))',
            'delete breakpoints']
        if STACK.enabled():
            import stack_readback
            lines+=stack_readback.prepare(root,directory,candidate,result,run)
    lines+=['detach','quit'];script.write_text('\n'.join(lines)+'\n')
    run(phase,['timeout','240','gdb','-q','-batch','-x',str(script),str(candidate/'kernel.elf')])
