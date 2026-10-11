#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build/baremetal-boot-trace
log_file=$artifact_dir/production-test-policy.log
hash_file=$artifact_dir/production-candidate.sha256
mkdir -p "$artifact_dir"

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/hobbyos-production-policy.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT
exec > >(tee "$log_file") 2>&1

fail_policy()
{
    echo "[BOOT][PRODUCTION_TEST_POLICY] FAIL $*" >&2
    exit 1
}

for tool in gcc strings grep cmp mcopy sha256sum; do
    command -v "$tool" >/dev/null || fail_policy "missing-tool=$tool"
done

for artifact in kernel.elf hobbyos.img; do
    [[ -s $artifact ]] || fail_policy "missing-artifact=$artifact"
done

boot_source=kernel/src/core/kernel_init.c
kernel_source=kernel/kernel.c
registry_source=kernel/src/shell/commands/registry.c
strings_file=$tmp_dir/kernel.strings
registry_pp=$tmp_dir/registry.production.i
kernel_pp=$tmp_dir/kernel.production.i
kernel_autorun_pp=$tmp_dir/kernel.autorun.i

strings kernel.elf > "$strings_file"

markers=(
    '[BOOTTRACE][BSP] 1 before-scheduler-start'
    '[BOOTTRACE][BSP] 2 after-scheduler-start'
    '[BOOTTRACE][BSP] 3 before-irq-enable'
    '[BOOTTRACE][BSP] 4 after-irq-enable'
    '[BOOTTRACE][BSP] 5 before-pci-init'
)

for marker in "${markers[@]}"; do
    binary_count=$(grep -Fxc -- "$marker" "$strings_file" || true)
    [[ $binary_count == 1 ]] ||
        fail_policy "breadcrumb-binary-count=$binary_count marker=$marker"

    source_call="    serial_write_all(\"${marker}\\n\");"
    source_count=$(grep -Fxc -- "$source_call" "$boot_source" || true)
    [[ $source_count == 1 ]] ||
        fail_policy "breadcrumb-source-count=$source_count marker=$marker"
done

get_unique_line()
{
    local pattern=$1
    local file=$2
    local -a matches=()
    mapfile -t matches < <(grep -Fn -- "$pattern" "$file" || true)
    ((${#matches[@]} == 1)) ||
        fail_policy "source-order-count=${#matches[@]} pattern=$pattern"
    UNIQUE_LINE=${matches[0]%%:*}
}

require_only_blank_between()
{
    local first=$1
    local second=$2
    local label=$3
    ((first < second)) || fail_policy "source-order=$label"
    if ((first + 1 < second)) &&
        sed -n "$((first + 1)),$((second - 1))p" "$boot_source" |
            grep -q '[^[:space:]]'; then
        fail_policy "source-order-nonblank-between=$label"
    fi
}

get_unique_line 'kpanic("Task identity validation failed");' "$boot_source"
identity_panic_line=$UNIQUE_LINE
get_unique_line "${markers[0]}" "$boot_source"
marker_1_line=$UNIQUE_LINE
get_unique_line 'if (scheduler_start() != SCHED_BOOT_OK)' "$boot_source"
scheduler_line=$UNIQUE_LINE
get_unique_line 'kpanic("Scheduler start readiness validation failed");' "$boot_source"
scheduler_panic_line=$UNIQUE_LINE
get_unique_line "${markers[1]}" "$boot_source"
marker_2_line=$UNIQUE_LINE
get_unique_line "${markers[2]}" "$boot_source"
marker_3_line=$UNIQUE_LINE
get_unique_line 'if (!irq_bootstrap_verify_bsp_lapic(250000u))' "$boot_source"
bsp_lapic_probe_line=$UNIQUE_LINE
get_unique_line 'irq_enable();' "$boot_source"
irq_line=$UNIQUE_LINE
get_unique_line "${markers[3]}" "$boot_source"
marker_4_line=$UNIQUE_LINE
get_unique_line 'if (!irq_bootstrap_wait_all_runtime_ready(10000000u))' "$boot_source"
runtime_ready_wait_line=$UNIQUE_LINE
get_unique_line 'console_begin_batch();' "$boot_source"
console_line=$UNIQUE_LINE
get_unique_line "${markers[4]}" "$boot_source"
marker_5_line=$UNIQUE_LINE
get_unique_line 'pci_init();' "$boot_source"
pci_line=$UNIQUE_LINE

require_only_blank_between "$identity_panic_line" "$marker_1_line" \
    identity-to-marker-1
((marker_1_line < scheduler_line && scheduler_line < scheduler_panic_line)) ||
    fail_policy source-order=marker-1-scheduler
require_only_blank_between "$scheduler_panic_line" "$marker_2_line" \
    scheduler-to-marker-2
((marker_2_line < marker_3_line &&
   marker_3_line + 1 == bsp_lapic_probe_line &&
   bsp_lapic_probe_line < irq_line &&
   irq_line + 1 == marker_4_line)) ||
    fail_policy source-order=irq-bootstrap-breadcrumbs
if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == owned-device-mapping || ${HOBBYOS_VALIDATION_CONTRACT:-} == verified-ram-direct-map || ${HOBBYOS_VALIDATION_CONTRACT:-} == low-address-isolation || ${HOBBYOS_VALIDATION_CONTRACT:-} == kernel-image-protection || ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
    ((marker_4_line + 1 == runtime_ready_wait_line)) ||
        fail_policy source-order=irq-release-to-device-worker
    python3 -B scripts/device-mapping-order.py --source "$boot_source" ||
        fail_policy source-order=device-worker
else
    ((marker_4_line + 1 == runtime_ready_wait_line &&
       runtime_ready_wait_line < console_line)) ||
        fail_policy source-order=irq-release-to-console
    ((console_line + 1 == marker_5_line && marker_5_line + 1 == pci_line)) ||
        fail_policy source-order=console-marker-5-pci
fi

for marker in \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][AUTORUN] PASS' \
    '[SELFTEST][AUTORUN] FAIL'; do
    ! grep -Fq -- "$marker" "$strings_file" ||
        fail_policy "autorun-marker-present=$marker"
done

gcc -E -P -ffreestanding -std=gnu11 "$registry_source" > "$registry_pp"
tasktest_count=$(grep -Ec '\.name[[:space:]]*=[[:space:]]*"tasktest"' \
    "$registry_pp" || true)
[[ $tasktest_count == 0 ]] || fail_policy tasktest-registered-in-production
locktest_count=$(grep -Ec '\.name[[:space:]]*=[[:space:]]*"locktest"' \
    "$registry_pp" || true)
[[ $locktest_count == 0 ]] || fail_policy locktest-registered-in-production
alloctest_count=$(grep -Ec '\.name[[:space:]]*=[[:space:]]*"alloctest"' \
    "$registry_pp" || true)
[[ $alloctest_count == 0 ]] || fail_policy alloctest-registered-in-production
profiletest_count=$(grep -Ec '\.name[[:space:]]*=[[:space:]]*"profiletest"' \
    "$registry_pp" || true)
[[ $profiletest_count == 0 ]] ||
    fail_policy profiletest-registered-in-production

if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == physical-translation ||
      ${HOBBYOS_VALIDATION_CONTRACT:-} == owned-device-mapping || ${HOBBYOS_VALIDATION_CONTRACT:-} == verified-ram-direct-map || ${HOBBYOS_VALIDATION_CONTRACT:-} == low-address-isolation || ${HOBBYOS_VALIDATION_CONTRACT:-} == kernel-image-protection || ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
    nm -an kernel.elf > "$tmp_dir/physical-symbols.txt"
    for symbol in physical_boot_adapt physical_pointer_range physical_address_range \
        bitmap_find_zero_run_below paging_kernel_root_physical paging_query_range; do
        grep -Eq "[[:space:]]${symbol}$" "$tmp_dir/physical-symbols.txt" ||
            fail_policy "physical-enforcement-missing=$symbol"
    done
    for command in phystest ptinspect cpustate memmap tlbtest; do
        ! grep -Eq "\\.name[[:space:]]*=[[:space:]]*\"${command}\"" "$registry_pp" ||
            fail_policy "diagnostic-registered-in-production=$command"
    done
    ! grep -Eq '[[:space:]](cmd_phystest|physical_fixture_run|physical_host_backing)$' \
        "$tmp_dir/physical-symbols.txt" || fail_policy physical-test-symbol-present
    ! grep -Eq '\[PHYSICAL\]\[(SEARCH|POINTER|CPU|RESULT)\]' "$strings_file" ||
        fail_policy physical-test-marker-present
    grep -Fq '[PHYSICAL][BOOT] READY' "$strings_file" || fail_policy physical-boot-proof-missing
    if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == owned-device-mapping || ${HOBBYOS_VALIDATION_CONTRACT:-} == verified-ram-direct-map || ${HOBBYOS_VALIDATION_CONTRACT:-} == low-address-isolation || ${HOBBYOS_VALIDATION_CONTRACT:-} == kernel-image-protection || ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
        for symbol in mmio_mapping_acquire firmware_mapping_acquire owned_mapping_release \
            dma_buffer_allocate pmm_alloc_constrained_frames kernel_cache_writeback cpu_request_mapping_run; do
            grep -Eq "[[:space:]]${symbol}$" "$tmp_dir/physical-symbols.txt" ||
                fail_policy "owned-memory-enforcement-missing=$symbol"
        done
    fi
fi

if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == verified-ram-direct-map || ${HOBBYOS_VALIDATION_CONTRACT:-} == low-address-isolation || ${HOBBYOS_VALIDATION_CONTRACT:-} == kernel-image-protection || ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
    for symbol in paging_build_create paging_build_verify paging_build_activate ram_map_plan_verify pmm_direct_map_activate; do
        grep -Eq "[[:space:]]${symbol}$" "$tmp_dir/physical-symbols.txt" || fail_policy "ram-enforcement-missing=$symbol"
    done
    ! grep -Eq '[[:space:]](cmd_rammap|ram_map_fixture_run|pmm_test_alloc_range|paging_ram_scratch_table)$' "$tmp_dir/physical-symbols.txt" || fail_policy ram-test-symbol-present
    ! grep -Eq '\[RAM_MAP\]\[(BEGIN|HIGH|HEAP|BITMAP|LEAVES)\]' "$strings_file" || fail_policy ram-test-marker-present
    grep -Fq '[RAM_MAP] PRELOAD' "$strings_file" || fail_policy ram-preload-proof-missing
    grep -Fq '[RAM_MAP] ACTIVE' "$strings_file" || fail_policy ram-activation-proof-missing
fi

if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == low-address-isolation || ${HOBBYOS_VALIDATION_CONTRACT:-} == kernel-image-protection || ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
    for symbol in paging_low_half_empty paging_bootstrap_retire bootstrap_lifetime_depart bootstrap_tables_verify low_fault_description; do
        grep -Eq "[[:space:]]${symbol}$" "$tmp_dir/physical-symbols.txt" || fail_policy "low-root-enforcement-missing=$symbol"
    done
    ! grep -Eq '(cmd_lowmap|null_probe|null_destructive|g_bootstrap_snapshot)' "$tmp_dir/physical-symbols.txt" || fail_policy null-test-symbol-present
    ! grep -Eq '\[LOW_ROOT\]\[(NULL|DESTRUCTIVE|BOOT_SNAPSHOT)\]' "$strings_file" || fail_policy null-test-marker-present
    grep -Fq 'Page Fault: NULL-address access' "$strings_file" || fail_policy null-diagnostic-missing
fi

if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == kernel-image-protection || ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
    for symbol in image_permissions_from_linker image_permissions_check_leaf image_walk_main paging_image_boot_check; do
        grep -Eq "[[:space:]]${symbol}$" "$tmp_dir/physical-symbols.txt" || fail_policy "image-enforcement-missing=$symbol"
    done
    ! grep -Eq '(cmd_wxtest|protection_probe|protection_store|protection_fetch|protection_text_target|protection_rodata_target|protection_data_target)' "$tmp_dir/physical-symbols.txt" || fail_policy image-test-symbol-present
    ! grep -Eq '\[IMAGE_PROTECTION\]\[(FAULT|CPU|BEGIN|UNSUPPORTED)\]' "$strings_file" || fail_policy image-test-marker-present
    ! grep -Eq '\.name[[:space:]]*=[[:space:]]*"wxtest"' "$registry_pp" || fail_policy image-test-command-present
    gcc -E -P -ffreestanding -std=gnu11 kernel/src/smp/cpu_request_state.c > "$tmp_dir/request-production.c"
    ! grep -q CPU_REQUEST_PROTECTION "$tmp_dir/request-production.c" || fail_policy image-test-operation-present
    grep -Fq '[IMAGE_PROTECTION] PRELOAD' "$strings_file" || fail_policy image-preload-proof-missing
fi

development_commands=(
    panic irq usbdiag smpstress taskdiag tasktest schedtest synctest accounttest
    killtest reaptest inputtest modaltest taskmantest diagnosticresulttest
    locktest alloctest profiletest ptinspect tlbtest mappingtest lowmap wxtest
    stacktest phystest rammap cpustate memmap irqlatency
)
if [[ ${HOBBYOS_VALIDATION_CONTRACT:-} == guarded-kernel-stacks ]]; then
    for symbol in kernel_stack_acquire kernel_stack_retire stack_owner_guard_lookup stack_fault_classify paging_stack_install; do
        grep -Eq "[[:space:]]${symbol}$" "$tmp_dir/physical-symbols.txt" || fail_policy "stack-enforcement-missing=$symbol"
    done
    ! grep -Eq '(cmd_stacktest|guard_probe|guard_overflow|stack_test_)' "$tmp_dir/physical-symbols.txt" || fail_policy stack-test-symbol-present
    ! grep -Fq '[STACK_GUARD]' "$strings_file" || fail_policy stack-test-marker-present
    ! grep -Eq '\.name[[:space:]]*=[[:space:]]*"stacktest"' "$registry_pp" || fail_policy stack-test-command-present
    grep -Fq '[STACK_FAULT] classification=' "$strings_file" || fail_policy stack-panic-diagnosis-missing
fi
nm -an kernel.elf > "$tmp_dir/production-symbols.txt"
for command in "${development_commands[@]}"; do
    ! grep -Eq "\\.name[[:space:]]*=[[:space:]]*\"${command}\"" "$registry_pp" ||
        fail_policy "development-command-present=$command"
    ! grep -Eq "[[:space:]]cmd_${command}$" "$tmp_dir/production-symbols.txt" ||
        fail_policy "development-handler-present=$command"
done
for command in help clear version mem shutdown restart cpu pci acpi echo ps kill taskman; do
    command_count=$(grep -Ec "\\.name[[:space:]]*=[[:space:]]*\"${command}\"" "$registry_pp" || true)
    [[ $command_count == 1 ]] || fail_policy "production-command-count=$command_count command=$command"
done
! grep -Eq '[[:space:]](selftest_run_all|selftest_run_suite|tlb_proof_begin|irq_latency_test_begin|killtest_cli_telemetry_enabled|reaptest_zombie_mem_observe_ps)$' \
    "$tmp_dir/production-symbols.txt" || fail_policy development-payload-present

boot_handlers=(
    cmd_schedtest cmd_synctest cmd_accounttest cmd_killtest cmd_reaptest
    cmd_inputtest cmd_modaltest cmd_taskmantest cmd_tasktest cmd_locktest
    cmd_alloctest cmd_profiletest smpstress
)
for handler in "${boot_handlers[@]}"; do
    if grep -En "${handler}[[:space:]]*\\(" "$kernel_source" "$boot_source"; then
        fail_policy "boot-handler-invocation=$handler"
    fi
done

gcc -E -P -ffreestanding -std=gnu11 \
    -DHOBBYOS_KERNEL_SERIAL_DEV_PORTS=1 "$kernel_source" > "$kernel_pp"
if grep -Eq '=[[:space:]]*selftest_autorun_if_enabled[[:space:]]*\(' \
    "$kernel_pp"; then
    fail_policy production-autorun-call-present
fi

gcc -E -P -ffreestanding -std=gnu11 \
    -DHOBBYOS_KERNEL_SERIAL_DEV_PORTS=1 \
    -DHOBBYOS_SELFTEST=1 -DHOBBYOS_SELFTEST_AUTORUN=1 \
    "$kernel_source" > "$kernel_autorun_pp"
autorun_call_count=$(grep -Ec \
    '=[[:space:]]*selftest_autorun_if_enabled[[:space:]]*\(' \
    "$kernel_autorun_pp" || true)
[[ $autorun_call_count == 1 ]] ||
    fail_policy "ci-autorun-call-count=$autorun_call_count"

dead_residues=(
    serial_write_dec_all test_smp_task '[SMPTEST]' busy_hlt_delay
    delay_seconds_for_reading
)
for residue in "${dead_residues[@]}"; do
    if grep -Fn -- "$residue" "$kernel_source" "$boot_source"; then
        fail_policy "dead-source-residue=$residue"
    fi
    if grep -Fq -- "$residue" "$strings_file"; then
        fail_policy "dead-binary-residue=$residue"
    fi
done

unexpected_negative=$(grep -E '\[NEGATIVE[^]]*\]' "$strings_file" |
    grep -Fvx \
        -e '[REAPTEST][NEGATIVE_AGE] FAIL build_hook_inactive' \
        -e '[TASK][STACK][NEGATIVE] STACK_GUARD_CORRUPTION_DETECTED' || true)
[[ -z $unexpected_negative ]] || {
    printf '%s\n' "$unexpected_negative" >&2
    fail_policy negative-build-marker-present
}

lapic_count=$(grep -Fh 'lapic_timer_calibrate(1000' \
    kernel/src/core/kernel_init.c kernel/src/smp/smp_boot.c | wc -l)
[[ $lapic_count == 2 ]] || fail_policy "lapic-1000us-count=$lapic_count"

protected_manifest=${HOBBYOS_PRODUCTION_PROTECTED_MANIFEST:-}
if [[ -n $protected_manifest ]]; then
    [[ -s $protected_manifest ]] ||
        fail_policy "missing-protected-manifest=$protected_manifest"
    cut -d' ' -f3- "$protected_manifest" |
        while IFS= read -r protected_file; do
            [[ -f $protected_file ]] || fail_policy "missing-protected=$protected_file"
            sha256sum "$protected_file"
        done > "$tmp_dir/protected-current.sha256"
    cmp -s "$protected_manifest" \
        "$tmp_dir/protected-current.sha256" ||
        fail_policy protected-runtime-scope-drift
fi

mcopy -i hobbyos.img ::/kernel.elf "$tmp_dir/image-kernel.elf"
cmp -s kernel.elf "$tmp_dir/image-kernel.elf" ||
    fail_policy image-kernel-payload-mismatch

hash_artifacts=(kernel.elf hobbyos.img)
[[ ! -s BOOTX64.EFI ]] || hash_artifacts+=(BOOTX64.EFI)
sha256sum "${hash_artifacts[@]}" > "$hash_file"
cat "$hash_file"

echo '[BOOT][PRODUCTION_TEST_POLICY] PASS autorun=0 tasktest=0 development_commands=0 production_commands=13 breadcrumbs=5'
