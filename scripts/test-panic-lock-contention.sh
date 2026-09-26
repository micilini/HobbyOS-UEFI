#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

JOBS=${JOBS:-$(nproc)}
STAMP=$(date +%Y%m%d-%H%M%S%z)
EVIDENCE=${PANIC_EVIDENCE_DIR:-$ROOT/artifacts/build/panic-safety/$STAMP}
mkdir -p "$EVIDENCE" "$EVIDENCE/builds" "$EVIDENCE/runtime" \
  "$EVIDENCE/commands"
EVIDENCE=$(realpath "$EVIDENCE")
GATE_LOG="$EVIDENCE/panic-gate.log"
exec > >(tee -a "$GATE_LOG") 2>&1

INSTRUMENTED_DIR="$EVIDENCE/builds/instrumented-final"
NEGATIVE_DIR="$EVIDENCE/builds/adversarial-lock-wait"
PRODUCTION_DIR="$EVIDENCE/builds/production-final"

record_command() {
  local name=$1
  shift
  printf '%q ' "$@" > "$EVIDENCE/commands/$name.txt"
  printf '\n' >> "$EVIDENCE/commands/$name.txt"
}

run_logged() {
  local log=$1
  shift
  record_command "$(basename "$log" .log)" "$@"
  set +e
  "$@" 2>&1 | tee "$log"
  local status=${PIPESTATUS[0]}
  set -e
  return "$status"
}

preflight() {
  git rev-parse --show-toplevel > "$EVIDENCE/root.txt"
  git branch --show-current > "$EVIDENCE/branch.txt"
  git rev-parse HEAD > "$EVIDENCE/head-before.txt"
  git rev-parse HEAD^{tree} > "$EVIDENCE/tree-before.txt"
  git status --short > "$EVIDENCE/status-before.txt"
  {
    gcc --version | head -1
    ld --version | head -1
    make --version | head -1
    python3 --version
    qemu-system-x86_64 --version | head -1
    gdb --version | head -1
    mformat --version 2>&1 | head -1
  } > "$EVIDENCE/tool-versions.txt"
  {
    printf 'schedulable_cpus=%s\n' "$(nproc)"
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
      printf 'kvm=available\n'
    else
      printf 'kvm=unavailable\n'
    fi
  } > "$EVIDENCE/host-capabilities.txt"
  run_logged "$EVIDENCE/preflight-deps.log" make deps-check

  if run_logged "$EVIDENCE/preflight-incompatible-flags.log" \
      make kernel-check "JOBS=$JOBS" SELFTEST=0 SELFTEST_AUTORUN=0 \
      KERNEL_EXTRA_CFLAGS=-DHOBBYOS_PANIC_TEST; then
    echo "panic-gate: FAIL incompatible test flags built successfully"
    return 1
  fi
  echo "panic-gate: incompatible test flags rejected"
}

static_checks() {
  local forbidden='clock_monotonic|hpet_usleep|power_restart|power_shutdown|graphics_|g_panic_lock'
  if rg -n "$forbidden" kernel/src/core/panic.c > "$EVIDENCE/static-forbidden.log"; then
    echo "panic-gate: FAIL ordinary panic dependency remains"
    cat "$EVIDENCE/static-forbidden.log"
    return 1
  fi
  rg -n 'g_panic_owner|g_panic_in_progress|PANIC_COUNTDOWN_(POLL|STALL)_LIMIT|panic_halt_secondary' \
    kernel/src/core/panic.c > "$EVIDENCE/static-required.log"
  rg -n 'panic_current_cpu_is_owner|panic_halt_secondary|panic_test_timer_hook' \
    kernel/src/core/interrupts.c > "$EVIDENCE/static-interrupts.log"
  scripts/verify-panic-evidence.py fixtures \
    --output-dir "$EVIDENCE/oracle-fixtures" | \
    tee "$EVIDENCE/parser-negative-fixtures.log"
}

build_instrumented() {
  if [[ -f "$INSTRUMENTED_DIR/hobbyos.img" &&
        -f "$INSTRUMENTED_DIR/kernel.elf" ]]; then
    return
  fi
  run_logged "$EVIDENCE/builds/instrumented-build.log" make clean
  run_logged "$EVIDENCE/builds/instrumented-image.log" \
    make image "JOBS=$JOBS" SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_PANIC_TEST
  mkdir -p "$INSTRUMENTED_DIR"
  cp kernel.elf BOOTX64.EFI hobbyos.img "$INSTRUMENTED_DIR/"
  sha256sum "$INSTRUMENTED_DIR/kernel.elf" \
    "$INSTRUMENTED_DIR/BOOTX64.EFI" "$INSTRUMENTED_DIR/hobbyos.img" \
    > "$INSTRUMENTED_DIR/SHA256SUMS"
  nm -n -S "$INSTRUMENTED_DIR/kernel.elf" > \
    "$INSTRUMENTED_DIR/symbols.txt"
  for symbol in g_panic_test_state panic_test_full_ud2_site \
                panic_test_minimal_ud2_site panic_test_exception_ud2_site; do
    rg -q "[[:space:]]$symbol$" "$INSTRUMENTED_DIR/symbols.txt" || {
      echo "panic-gate: FAIL missing instrumented symbol $symbol"
      return 1
    }
  done
}

require_kvm() {
  if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    echo "panic-gate: KVM_UNAVAILABLE mandatory profile not executed"
    return 1
  fi
}

run_case() {
  local name=$1
  shift
  local run_dir="$EVIDENCE/runtime/$name"
  record_command "$name" python3 scripts/panic-qemu.py \
    --run-dir "$run_dir" --image "$INSTRUMENTED_DIR/hobbyos.img" \
    --elf "$INSTRUMENTED_DIR/kernel.elf" "$@"
  python3 scripts/panic-qemu.py --run-dir "$run_dir" \
    --image "$INSTRUMENTED_DIR/hobbyos.img" \
    --elf "$INSTRUMENTED_DIR/kernel.elf" "$@"
  scripts/verify-panic-evidence.py run "$run_dir"
}

common_tcg=(--machine q35 --accel tcg --memory 2G --boot-timeout 75
            --post-timeout 60 --countdown 1 --owner-slot 0 --peer-slot 1)
common_kvm=(--machine q35 --accel kvm --memory 2G --boot-timeout 45
            --post-timeout 25 --countdown 1 --owner-slot 0 --peer-slot 1)
common_kvm_smp24=(--machine q35 --accel kvm --memory 2G --boot-timeout 120
                  --boot-checkpoint 45 --post-timeout 25 --countdown 1
                  --owner-slot 0 --peer-slot 1)

selftest_cases() {
  build_instrumented
  run_case simple-tcg-smp1 --scenario simple --action halt --locks none \
    --clock-mode normal --machine q35 --accel tcg --memory 2G --smp 1 \
    --boot-timeout 75 --post-timeout 25 --countdown 1 --owner-slot 0 \
    --peer-slot 0
  run_case exception-tcg-smp4 --scenario exception --action halt --locks none \
    --clock-mode normal --smp 4 "${common_tcg[@]}"
}

contention_cases() {
  build_instrumented
  run_case console-lock-tcg-smp4 --scenario contention --action halt \
    --locks console --clock-mode normal --smp 4 "${common_tcg[@]}"
  run_case clock-lock-tcg-smp4 --scenario contention --action halt \
    --locks clock --clock-mode normal --smp 4 "${common_tcg[@]}"
  for attempt in 1 2 3; do
    run_case "joint-lock-tcg-smp4-$attempt" --scenario contention \
      --action halt --locks both --clock-mode normal --smp 4 \
      "${common_tcg[@]}"
  done

  require_kvm
  for attempt in 1 2 3; do
    run_case "joint-lock-kvm-smp4-$attempt" --scenario contention \
      --action halt --locks both --clock-mode normal --smp 4 \
      "${common_kvm[@]}"
  done
  run_case joint-lock-kvm-smp24 --scenario contention --action halt \
    --locks both --clock-mode normal --smp 24 "${common_kvm_smp24[@]}"
}

reentry_cases() {
  build_instrumented
  for attempt in 1 2 3; do
    run_case "reentry-tcg-smp4-$attempt" --scenario reentry --action halt \
      --locks none --clock-mode normal --smp 4 "${common_tcg[@]}"
  done
  for attempt in 1 2 3; do
    run_case "second-reentry-tcg-smp4-$attempt" --scenario second-reentry \
      --action halt --locks none --clock-mode normal --smp 4 \
      "${common_tcg[@]}"
  done

  require_kvm
  for attempt in 1 2 3; do
    run_case "reentry-kvm-smp4-$attempt" --scenario reentry --action halt \
      --locks none --clock-mode normal --smp 4 "${common_kvm[@]}"
  done
  for attempt in 1 2 3; do
    run_case "second-reentry-kvm-smp4-$attempt" \
      --scenario second-reentry --action halt --locks none \
      --clock-mode normal --smp 4 "${common_kvm[@]}"
  done
  run_case second-reentry-kvm-smp24 --scenario second-reentry --action halt \
    --locks none --clock-mode normal --smp 24 "${common_kvm_smp24[@]}"
}

stalled_clock_cases() {
  build_instrumented
  for attempt in 1 2 3; do
    run_case "constant-clock-tcg-smp4-$attempt" --scenario clock-stalled \
      --action halt --locks none --clock-mode constant --smp 4 \
      "${common_tcg[@]}"
  done
  run_case invalid-clock-tcg-smp4 --scenario clock-invalid --action halt \
    --locks none --clock-mode invalid --smp 4 "${common_tcg[@]}"
  run_case regressing-clock-tcg-smp4 --scenario clock-stalled --action halt \
    --locks none --clock-mode regressing --smp 4 "${common_tcg[@]}"
  run_case intermittent-clock-tcg-smp4 --scenario clock-stalled --action halt \
    --locks none --clock-mode intermittent --smp 4 "${common_tcg[@]}"
  run_case hpet-frozen-tcg-smp4 --scenario clock-stalled --action halt \
    --locks none --clock-mode normal --freeze-hpet --smp 4 \
    "${common_tcg[@]}"

  require_kvm
  for attempt in 1 2 3; do
    run_case "constant-clock-kvm-smp4-$attempt" --scenario clock-stalled \
      --action halt --locks none --clock-mode constant --smp 4 \
      "${common_kvm[@]}"
  done
}

action_cases() {
  build_instrumented
  run_case restart-tcg-smp4 --scenario simple --action restart --locks none \
    --clock-mode normal --smp 4 "${common_tcg[@]}"
  run_case shutdown-tcg-smp4 --scenario simple --action shutdown --locks none \
    --clock-mode normal --smp 4 "${common_tcg[@]}"
  run_case uart-unresponsive-tcg-smp4 --scenario uart-unresponsive \
    --action halt --locks none --clock-mode normal --smp 4 \
    "${common_tcg[@]}"
}

negative_cases() {
  run_logged "$EVIDENCE/builds/negative-clean.log" make clean
  run_logged "$EVIDENCE/builds/negative-image.log" \
    make image "JOBS=$JOBS" SELFTEST=1 SELFTEST_AUTORUN=0 \
    'KERNEL_EXTRA_CFLAGS=-DHOBBYOS_PANIC_TEST -DHOBBYOS_PANIC_NEGATIVE_LOCK_WAIT'
  mkdir -p "$NEGATIVE_DIR"
  cp kernel.elf BOOTX64.EFI hobbyos.img "$NEGATIVE_DIR/"
  sha256sum "$NEGATIVE_DIR/kernel.elf" "$NEGATIVE_DIR/BOOTX64.EFI" \
    "$NEGATIVE_DIR/hobbyos.img" > "$NEGATIVE_DIR/SHA256SUMS"
  local run_dir="$EVIDENCE/runtime/adversarial-lock-wait-tcg-smp4"
  record_command adversarial-lock-wait-tcg-smp4 \
    python3 scripts/panic-qemu.py --negative-control --run-dir "$run_dir"
  python3 scripts/panic-qemu.py --run-dir "$run_dir" \
    --image "$NEGATIVE_DIR/hobbyos.img" --elf "$NEGATIVE_DIR/kernel.elf" \
    --scenario contention --action halt --locks both --clock-mode normal \
    --negative-control --smp 4 --machine q35 --accel tcg --memory 2G \
    --boot-timeout 75 --post-timeout 4 --countdown 1 \
    --owner-slot 0 --peer-slot 1
  scripts/verify-panic-evidence.py run "$run_dir"
}

production_case() {
  run_logged "$EVIDENCE/builds/production-image.log" \
    make production-image "JOBS=$JOBS"
  mkdir -p "$PRODUCTION_DIR"
  cp kernel.elf BOOTX64.EFI hobbyos.img "$PRODUCTION_DIR/"
  sha256sum "$PRODUCTION_DIR/kernel.elf" "$PRODUCTION_DIR/BOOTX64.EFI" \
    "$PRODUCTION_DIR/hobbyos.img" > "$PRODUCTION_DIR/SHA256SUMS"
  nm -n -S "$PRODUCTION_DIR/kernel.elf" > "$PRODUCTION_DIR/symbols.txt"
  if rg -n 'g_panic_test_state|panic_test_.*ud2|g_panic_test_uart' \
      "$PRODUCTION_DIR/symbols.txt" > "$PRODUCTION_DIR/forbidden-symbols.txt"; then
    echo "panic-gate: FAIL test injection remains in production ELF"
    return 1
  fi
  nm -u "$PRODUCTION_DIR/kernel.elf" > "$PRODUCTION_DIR/undefined.txt"
  if rg -n '__atomic|libatomic' "$PRODUCTION_DIR/undefined.txt"; then
    echo "panic-gate: FAIL production requires external atomic support"
    return 1
  fi
  local run_dir="$EVIDENCE/runtime/production-simple-tcg-smp1"
  record_command production-simple-tcg-smp1 python3 scripts/panic-qemu.py \
    --production --run-dir "$run_dir"
  python3 scripts/panic-qemu.py --run-dir "$run_dir" \
    --image "$PRODUCTION_DIR/hobbyos.img" --elf "$PRODUCTION_DIR/kernel.elf" \
    --production-preflight-command 'irq check' \
    --production-preflight-marker '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC period_us=1000 hpet_timer0=QUIESCENT non_bsp_ticks=0 stray_hpet=0' \
    --production --production-command 'panic normal-image-smoke' \
    --scenario simple --action restart --locks none --clock-mode normal \
    --smp 1 --machine q35 --accel tcg --memory 2G --boot-timeout 75 \
    --post-timeout 30 --countdown 1 --owner-slot 0 --peer-slot 0
  scripts/verify-panic-evidence.py run "$run_dir"
}

all_cases() {
  preflight
  static_checks
  selftest_cases
  contention_cases
  reentry_cases
  stalled_clock_cases
  action_cases
  negative_cases
  production_case
  scripts/verify-panic-evidence.py tree "$EVIDENCE/runtime" | \
    tee "$EVIDENCE/verification-summary.log"
  git status --short > "$EVIDENCE/status-after.log"
  echo "panic-gate: PASS evidence=$EVIDENCE"
}

case "${1:-}" in
  preflight) preflight ;;
  static) static_checks ;;
  selftest) selftest_cases ;;
  contention) contention_cases ;;
  reentry) reentry_cases ;;
  stalled-clock) stalled_clock_cases ;;
  actions) action_cases ;;
  negatives) negative_cases ;;
  production) production_case ;;
  all) all_cases ;;
  *)
    echo "usage: $0 {preflight|static|selftest|contention|reentry|stalled-clock|actions|negatives|production|all}" >&2
    exit 2
    ;;
esac
