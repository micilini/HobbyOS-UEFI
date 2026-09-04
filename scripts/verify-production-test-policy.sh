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
((marker_4_line + 1 == runtime_ready_wait_line &&
   runtime_ready_wait_line < console_line)) ||
    fail_policy source-order=irq-release-to-console
((console_line + 1 == marker_5_line && marker_5_line + 1 == pci_line)) ||
    fail_policy source-order=console-marker-5-pci

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

manual_diagnostics=(
    schedtest synctest accounttest killtest reaptest inputtest modaltest taskmantest
)
for command in "${manual_diagnostics[@]}"; do
    command_count=$(grep -Ec \
        "\\.name[[:space:]]*=[[:space:]]*\"${command}\"" \
        "$registry_pp" || true)
    [[ $command_count == 1 ]] ||
        fail_policy "manual-diagnostic-count=$command_count command=$command"
done

smpstress_count=$(grep -Ec '\.name[[:space:]]*=[[:space:]]*"smpstress"' \
    "$registry_pp" || true)
[[ $smpstress_count == 1 ]] ||
    fail_policy "manual-diagnostic-count=$smpstress_count command=smpstress"

boot_handlers=(
    cmd_schedtest cmd_synctest cmd_accounttest cmd_killtest cmd_reaptest
    cmd_inputtest cmd_modaltest cmd_taskmantest cmd_tasktest smpstress
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

echo '[BOOT][PRODUCTION_TEST_POLICY] PASS autorun=0 tasktest=0 manual_diagnostics=1 breadcrumbs=5'
