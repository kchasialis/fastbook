
import struct
import os


def _header(stock_locate: int, tracking: int, timestamp: int) -> bytes:
    ts = struct.pack(">Q", timestamp)[2:] 
    return struct.pack(">HH", stock_locate, tracking) + ts


def _frame(msg_type: str, payload: bytes) -> bytes:
    length = 1 + len(payload)
    return struct.pack(">H", length) + msg_type.encode() + payload


def add_order(oid: int, side: str, shares: int, price: int,
              stock: str = "AAPL    ", ts: int = 0) -> bytes:
    payload = (
        _header(1, 0, ts)
        + struct.pack(">Q", oid)
        + side.encode()
        + struct.pack(">I", shares)
        + stock.encode()[:8].ljust(8)
        + struct.pack(">I", price)
    )
    return _frame("A", payload)


def order_delete(oid: int, ts: int = 0) -> bytes:
    payload = _header(1, 0, ts) + struct.pack(">Q", oid)
    return _frame("D", payload)


def order_cancel(oid: int, cancelled_shares: int, ts: int = 0) -> bytes:
    payload = _header(1, 0, ts) + struct.pack(">Q", oid) + struct.pack(">I", cancelled_shares)
    return _frame("X", payload)


def order_executed(oid: int, executed_shares: int, match_num: int, ts: int = 0) -> bytes:
    payload = (
        _header(1, 0, ts)
        + struct.pack(">Q", oid)
        + struct.pack(">I", executed_shares)
        + struct.pack(">Q", match_num)
    )
    return _frame("E", payload)


def order_replace(orig_oid: int, new_oid: int, shares: int, price: int, ts: int = 0) -> bytes:
    payload = (
        _header(1, 0, ts)
        + struct.pack(">Q", orig_oid)
        + struct.pack(">Q", new_oid)
        + struct.pack(">I", shares)
        + struct.pack(">I", price)
    )
    return _frame("U", payload)


messages = [
    add_order(oid=1, side="S", shares=100, price=1500200, ts=1_000_000),
    add_order(oid=2, side="S", shares=300, price=1500300, ts=2_000_000),
    add_order(oid=3, side="B", shares=150, price=1499950, ts=3_000_000),
    add_order(oid=4, side="B", shares=100, price=1500200, ts=4_000_000),
    order_cancel(oid=2, cancelled_shares=100, ts=5_000_000),
    order_replace(orig_oid=3, new_oid=5, shares=150, price=1500100, ts=6_000_000),
    order_delete(oid=2, ts=7_000_000),
]

out = os.path.join(os.path.dirname(__file__), "sample.itch")
with open(out, "wb") as f:
    for msg in messages:
        f.write(msg)

total = sum(len(m) for m in messages)
print(f"Wrote {len(messages)} messages, {total} bytes → {out}")
