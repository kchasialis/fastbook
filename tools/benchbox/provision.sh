#!/usr/bin/env bash
set -euo pipefail

ISOLATED=${ISOLATED:-1-5,7-11}
HOUSEKEEPING=${HOUSEKEEPING:-0}
NIC=${NIC:-$(ip -o route get 1.1.1.1 | awk '{for (i = 1; i < NF; i++) if ($i == "dev") print $(i + 1)}')}
NIC_IRQ_CPU=${NIC_IRQ_CPU:-$HOUSEKEEPING}
HUGEPAGES=${HUGEPAGES:-512}

boot() {
    cat >/etc/default/grub.d/99-fastbook.cfg <<EOF
GRUB_CMDLINE_LINUX="\$GRUB_CMDLINE_LINUX isolcpus=managed_irq,domain,$ISOLATED nohz_full=$ISOLATED rcu_nocbs=$ISOLATED irqaffinity=$HOUSEKEEPING nmi_watchdog=0"
EOF
    update-grub
    echo "now reboot, then run: $0 tune"
}

tune() {
    echo off >/sys/devices/system/cpu/smt/control

    for c in /sys/devices/system/cpu/cpu[0-9]*; do
        if [[ -f $c/online && $(cat "$c/online") == 0 ]]; then
            continue
        fi
        echo performance >"$c/cpufreq/scaling_governor"
        echo 0 >"$c/cpufreq/boost" 2>/dev/null || true
        for s in "$c"/cpuidle/state[1-9]; do
            echo 1 >"$s/disable"
        done
        if grep -q AuthenticAMD /proc/cpuinfo; then
            modprobe msr
            n=${c##*/cpu}
            v=$(rdmsr -p "$n" -c 0xc0010015)
            wrmsr -p "$n" 0xc0010015 "$(printf '0x%x' $((v | (1 << 25))))"
        fi
    done
    echo 0 >/sys/devices/system/cpu/cpufreq/boost 2>/dev/null || true

    systemctl stop irqbalance 2>/dev/null || true
    for a in /proc/irq/*/smp_affinity_list; do
        echo "$HOUSEKEEPING" >"$a" 2>/dev/null || true
    done
    nic_irqs

    echo "$HUGEPAGES" >/proc/sys/vm/nr_hugepages
    echo 1 >/sys/devices/virtual/workqueue/cpumask

    sysctl -qw \
        kernel.perf_event_paranoid=-1 \
        kernel.kptr_restrict=0 \
        kernel.timer_migration=0 \
        kernel.sched_rt_runtime_us=-1 \
        vm.stat_interval=120 \
        net.core.rmem_max=67108864
}

nic_irqs() {
    for irq in $(grep -E "$NIC" /proc/interrupts | cut -d: -f1 | tr -d ' '); do
        echo "$NIC_IRQ_CPU" >"/proc/irq/$irq/smp_affinity_list"
    done
}

case "${1:-}" in
boot) boot ;;
tune) tune ;;
nic-irq) nic_irqs ;;
*)
    echo "usage: $0 boot | tune | nic-irq   (env: ISOLATED HOUSEKEEPING NIC NIC_IRQ_CPU HUGEPAGES)" >&2
    exit 1
    ;;
esac
