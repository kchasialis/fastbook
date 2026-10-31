#!/usr/bin/env bash
set -euo pipefail

B=${B:-build/release/bin/fastbook_bench_hashtable}
CPU=${CPU:-3}
OUT=${OUT:-/root/results/hashtable/$(date +%Y%m%d-%H%M%S)}
TABLES=${TABLES:-"umap map lp lp-tomb swiss swiss-sse2 swiss-swar swiss-scalar"}
SIZES=${SIZES:-"1024 4096 16384 65536 262144 1048576 4194304 16777216"}
STEPS=${STEPS:-2000000}
TRACE=${TRACE:-/root/traces/itch_ops.bin}
WORKLOADS=${WORKLOADS:-"churn itch"}
BUDGET=${BUDGET:-60}
MODES=${MODES:-"tp lat"}

mkdir -p "$OUT"
COMMIT=${COMMIT:-unknown} tools/benchbox/spec.sh build/release >"$OUT/spec.md"
"$B" --header >"$OUT/results.csv"
echo "table,workload,live,event,value,accuracy" >"$OUT/perf.csv"

one() {
    local table=$1 workload=$2 live=$3
    local args=(--table "$table" --workload "$workload" --budget "$BUDGET")
    if [[ $workload == itch ]]; then
        args+=(--trace "$TRACE")
    else
        args+=(--live "$live" --steps "$STEPS")
    fi

    echo "$(date +%T) $table $workload $live" >&2
    for mode in $MODES; do
        local extra=()
        if [[ $mode == tp ]]; then
            extra=(--perf-csv "$OUT/perf.csv")
        fi
        taskset -c "$CPU" "$B" "${args[@]}" --mode "$mode" "${extra[@]}" >>"$OUT/results.csv" || true
    done
}

for workload in $WORKLOADS; do
    if [[ $workload == itch ]]; then
        for table in $TABLES; do
            one "$table" itch 0
        done
    else
        for live in $SIZES; do
            for table in $TABLES; do
                one "$table" churn "$live"
            done
        done
    fi
done

echo "results in $OUT" >&2
