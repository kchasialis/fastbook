#!/usr/bin/env bash
set -euo pipefail

B=${B:-build/release/bin/fastbook_bench_logger}
HOT=${HOT:-3}
LOG_CPUS=${LOG_CPUS:-"4 1"}
OUT=${OUT:-/root/results/logger/$(date +%Y%m%d-%H%M%S)}
LOG_FILE=${LOG_FILE:-/root/bench_log.txt}
RATES=${RATES:-"100000 250000 500000 1000000 2000000 4000000 8000000 0"}
DURATION=${DURATION:-2}
SAMPLES=${SAMPLES:-1000000}
MODES=${MODES:-"compare rate"}

mkdir -p "$OUT"
COMMIT=${COMMIT:-unknown} tools/benchbox/spec.sh build/release >"$OUT/spec.md"
echo "- Log file: $LOG_FILE ($(df -T "$(dirname "$LOG_FILE")" | awk 'NR == 2 {print $2}'))" >>"$OUT/spec.md"
"$B" --header >"$OUT/results.csv"
echo "mode,contender,log_cpu,offered_rate,calls,event,value,exact" >"$OUT/perf.csv"

run() {
    "$B" --hot-cpu "$HOT" --log-file "$LOG_FILE" --perf-csv "$OUT/perf.csv" "$@" >>"$OUT/results.csv"
}

for log_cpu in $LOG_CPUS; do
    for mode in $MODES; do
        if [[ $mode == compare ]]; then
            echo "$(date +%T) compare, logger thread on cpu $log_cpu" >&2
            run --mode compare --log-cpu "$log_cpu" --samples "$SAMPLES"
        else
            for rate in $RATES; do
                echo "$(date +%T) rate $rate, logger thread on cpu $log_cpu" >&2
                run --mode rate --log-cpu "$log_cpu" --rate "$rate" --seconds "$DURATION"
            done
        fi
    done
done

echo "results in $OUT" >&2
