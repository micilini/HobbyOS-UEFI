#!/usr/bin/env bash
# Called after the existing machine, accelerator and control-profile checks.
configure_ram_profile() {
    local default_smp=${1:?default SMP required}
    machine_arg=$machine
    case ${QEMU_RAM_PROFILE:-normal} in
        normal) ;;
        high|high-fallback)
            [[ $machine == q35 && ${SMP:-$default_smp} == 4 && ${MEM:-2G} == 2G &&
               ${QEMU_CONTROL_PROFILE:-normal} == normal && ${QEMU_DEVICE_PROFILE:-direct} == direct ]] || {
                echo 'High RAM requires the named Q35/SMP4/2G direct-keyboard profile' >&2; return 2;
            }
            if [[ $QEMU_RAM_PROFILE == high ]]; then
                [[ $accel == kvm && $cpu == host ]] || { echo 'High RAM requires KVM host' >&2; return 2; }
            else
                [[ $accel == tcg && $cpu == max ]] || { echo 'RAM fallback requires TCG max' >&2; return 2; }
                cpu=max,-pdpe1gb
            fi
            machine_arg=q35,max-ram-below-4g=1G ;;
        *) echo 'Unsupported RAM profile' >&2; return 2 ;;
    esac
}
