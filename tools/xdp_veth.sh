#!/usr/bin/env bash
set -euo pipefail

NS=fbsrc
RX=fb-rx
TX=fb-tx

case "${1:-}" in
up)
    ip netns add "$NS"
    ip link add "$RX" type veth peer name "$TX" netns "$NS"
    ip addr add 10.77.0.2/24 dev "$RX"
    ip link set "$RX" up
    ip -n "$NS" addr add 10.77.0.1/24 dev "$TX"
    ip -n "$NS" link set "$TX" up
    ip -n "$NS" link set lo up
    ip netns exec "$NS" ethtool -K "$TX" tx off >/dev/null
    echo "receiver: $RX 10.77.0.2   sender: ip netns exec $NS ... -> 10.77.0.2"
    ;;
down)
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$RX" 2>/dev/null || true
    ;;
*)
    echo "usage: $0 up|down" >&2
    exit 1
    ;;
esac
