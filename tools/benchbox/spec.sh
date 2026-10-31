#!/usr/bin/env bash
set -uo pipefail

BUILD=${1:-build/release}
NIC=${NIC:-$(ip -o route get 1.1.1.1 | awk '{for (i = 1; i < NF; i++) if ($i == "dev") print $(i + 1)}')}
CPU=/sys/devices/system/cpu

cache_var() { grep -E "^$1:" "$BUILD/CMakeCache.txt" 2>/dev/null | cut -d= -f2-; }

idle_states() {
    for s in "$CPU"/cpu1/cpuidle/state*; do
        [[ $(cat "$s/disable") == 0 ]] && printf '%s ' "$(cat "$s/name")"
    done
}

nic_irq_cpus() {
    for irq in $(grep -E "$NIC" /proc/interrupts | cut -d: -f1 | tr -d ' '); do
        printf 'irq %s -> cpu %s; ' "$irq" "$(cat /proc/irq/"$irq"/smp_affinity_list)"
    done
}

measured_mhz() {
    taskset -c "${MEASURE_CPU:-3}" perf stat -e cycles,task-clock -x, -- \
        sh -c 'i=0; while [ $i -lt 3000000 ]; do i=$((i + 1)); done' 2>&1 |
        awk -F, '/cycles/ {c = $1} /task-clock/ {t = $1} END {printf "%.0f", c / (t * 1000)}'
}

mitigations() {
    for f in "$CPU"/vulnerabilities/*; do
        v=$(cat "$f")
        [[ $v == "Not affected" ]] || printf '%s: %s | ' "${f##*/}" "$v"
    done
}

cxx=$(cache_var CMAKE_CXX_COMPILER)
type=$(cache_var CMAKE_BUILD_TYPE)
flags=$(python3 -c '
import json, sys
for e in json.load(open(sys.argv[1])):
    if e["file"].endswith("src/main.cpp"):
        args = e.get("arguments") or e["command"].split()
        print(" ".join(a for a in args[1:] if a.startswith(("-O", "-m", "-f", "-std", "-g", "-DNDEBUG", "-DFASTBOOK"))))
' "$BUILD/compile_commands.json" 2>/dev/null)

cat <<EOF
### Machine
- Host: $(cat /sys/class/dmi/id/sys_vendor 2>/dev/null) $(cat /sys/class/dmi/id/board_name 2>/dev/null), bare metal: $(systemd-detect-virt 2>/dev/null)
- CPU: $(lscpu | sed -n 's/^Model name: *//p'), microcode $(grep -m1 microcode /proc/cpuinfo | awk '{print $3}')
- CPUs online: $(cat $CPU/online) (SMT $(cat $CPU/smt/control)), caches: $(lscpu | awk -F: '/L1d|L2|L3/ {gsub(/^ +/, "", $2); printf "%s %s; ", $1, $2}')
- RAM: $(free -g | awk '/Mem:/ {print $2 " GB"}'), $(dmidecode -t memory 2>/dev/null | grep -m1 'Configured Memory Speed' | cut -d: -f2 | xargs)
- NUMA nodes: $(lscpu | sed -n 's/^NUMA node(s): *//p'), NIC $NIC on node $(cat /sys/class/net/"$NIC"/device/numa_node 2>/dev/null)
- NIC: $(ethtool -i "$NIC" | awk -F': ' '/^driver|^firmware-version/ {printf "%s %s; ", $1, $2}')

### OS
- Kernel: $(uname -r), $(grep PRETTY_NAME /etc/os-release | cut -d'"' -f2)
- Boot line: $(cat /proc/cmdline)
- Isolated CPUs: $(cat $CPU/isolated), nohz_full: $(cat $CPU/nohz_full)
- Frequency: driver $(cat $CPU/cpu1/cpufreq/scaling_driver), governor $(cat $CPU/cpu1/cpufreq/scaling_governor), measured $(measured_mhz) MHz under load on cpu ${MEASURE_CPU:-3}
- Idle states enabled: $(idle_states)
- NIC IRQs: $(nic_irq_cpus)
- Huge pages: $(awk '/HugePages_Total/ {print $2}' /proc/meminfo) x $(awk '/Hugepagesize/ {print $2 $3}' /proc/meminfo), THP $(cat /sys/kernel/mm/transparent_hugepage/enabled)
- Clock source: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)
- Mitigations: $(mitigations)

### Build
- Commit: ${COMMIT:-unknown}
- Compiler: $($cxx --version 2>/dev/null | head -1)
- Build type: $type, flags: $flags
EOF
