#!/usr/bin/env bash
set -euo pipefail

CAPTURE=${CAPTURE:-/root/captures/12302019.NASDAQ_ITCH50}
RATE=${RATE:-25000}
BUILD=${BUILD:-build/bench2}
REPLAY_CPU=${REPLAY_CPU:-1}
OUT=${OUT:-/root/results/tick_to_book/$(date +%Y%m%d-%H%M%S)-r$RATE}

mkdir -p "$OUT"
if [[ ! -f $BUILD/build.ninja ]]; then
    taskset -c 0-5 cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ -DFASTBOOK_N_SHARDS=2 \
        -DBUILD_TESTS=OFF -DBUILD_BENCH=OFF >/dev/null
fi
taskset -c 0-5 cmake --build "$BUILD" --target fastbook mold_replay -j6 >/dev/null
COMMIT=${COMMIT:-unknown} tools/benchbox/spec.sh "$BUILD" >"$OUT/spec.md"

(
    sleep 2
    taskset -c "$REPLAY_CPU" "$BUILD/bin/mold_replay" -f "$CAPTURE" \
        -g 239.1.1.1 -p 5000 -r 6000 -R "$RATE" -l 2 >"$OUT/replay.log" 2>&1
) &
"$BUILD/bin/fastbook" -e nasdaq -c config/benchbox.conf \
    -g 239.1.1.1 -p 5000 -P 6000 >"$OUT/fastbook.log" 2>&1
wait

grep -E "tick_to_book|msgs in|builder" "$OUT/fastbook.log"
tail -1 "$OUT/replay.log"
echo "results in $OUT"
