import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

plt.rcParams["font.family"] = "Inter"

NAMES = {
    "swiss-sse2": "SwissTable (SSE2)",
    "lp": "LinearProbing",
    "swiss": "SwissTable (AVX2)",
    "umap": "std::unordered_map",
    "map": "std::map",
    "swiss-scalar": "SwissTable (scalar)",
    "swiss-swar": "SwissTable (SWAR)",
}
COLORS = {
    "swiss-sse2": "#2a78d6",
    "lp": "#eb6834",
    "swiss": "#1baf7a",
    "umap": "#eda100",
    "map": "#e87ba4",
    "swiss-scalar": "#008300",
    "swiss-swar": "#6250d6",
}
SWEEP = ["swiss-sse2", "lp", "swiss", "umap", "map", "swiss-scalar"]
REAL_DAY = ["swiss-sse2", "lp", "swiss", "swiss-swar", "swiss-scalar", "umap", "map"]

SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_2 = "#52514e"
GRID = "#e6e5e0"
BYTES_PER_LIVE = 32
CACHES = [("L1d 32 KiB", 32 << 10), ("L2 512 KiB", 512 << 10), ("L3 16 MiB", 16 << 20)]

FILLS = {"amd-l2-fills": "L2", "amd-l3-fills": "L3", "amd-dram-fills": "DRAM"}


def read_results(path):
    return [r for r in csv.DictReader(open(path)) if r["status"] == "ok"]


def read_perf(path):
    out = defaultdict(dict)
    for r in csv.DictReader(open(path)):
        try:
            out[r["table"]][r["event"]] = float(r["value"])
        except ValueError:
            pass
    return out


def style(ax):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(TEXT_2)
        ax.spines[side].set_linewidth(0.6)
    ax.tick_params(colors=TEXT_2, labelsize=9, width=0.6)
    ax.xaxis.label.set_color(TEXT_2)
    ax.yaxis.label.set_color(TEXT_2)


def real_day_chart(itch, out):
    tp = {r["table"]: float(r["mean_ns"]) for r in itch if r["mode"] == "tp"}
    rows = sorted((t for t in REAL_DAY if t in tp), key=lambda t: tp[t], reverse=True)

    fig, ax = plt.subplots(figsize=(8, 4.2), facecolor=SURFACE)
    style(ax)
    ys = range(len(rows))
    ax.barh(ys, [tp[t] for t in rows], height=0.62, color="#2a78d6")
    for y, t in zip(ys, rows):
        ax.text(tp[t] + 3, y, f"{tp[t]:.0f} ns", va="center", fontsize=9.5, color=TEXT,
                fontweight=600)
    ax.set_yticks(list(ys), [NAMES[t] for t in rows], fontsize=10, color=TEXT, fontweight=600)
    ax.tick_params(axis="y", length=0)
    ax.set_xlim(0, max(tp.values()) * 1.12)
    ax.set_xlabel("mean ns per operation")
    ax.xaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.spines["left"].set_visible(False)
    fig.suptitle("NASDAQ Trading Day Replay: 289M Inserts, Finds and Erases on Order IDs",
                 x=0.02, ha="left", fontsize=12, color=TEXT)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out, dpi=200, facecolor=SURFACE)
    plt.close(fig)


def orders_label(x, _):
    if x >= 1 << 20:
        return f"{int(x) >> 20}M"
    if x >= 1 << 10:
        return f"{int(x) >> 10}K"
    return f"{int(x)}"


def sweep_chart(churn, out):
    tp = [r for r in churn if r["mode"] == "tp"]
    fig, ax = plt.subplots(figsize=(8, 5.4), facecolor=SURFACE)
    style(ax)
    for t in SWEEP:
        pts = sorted((int(r["live"]), float(r["mean_ns"])) for r in tp if r["table"] == t)
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, color=COLORS[t], linewidth=1.6, marker="o", markersize=4.5,
                    markeredgecolor=SURFACE, markeredgewidth=1.2, label=NAMES[t])
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    sizes = sorted({int(r["live"]) for r in tp})
    ax.set_xticks(sizes)
    ax.xaxis.set_major_formatter(FuncFormatter(orders_label))
    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for label, size in CACHES:
        x = size / BYTES_PER_LIVE
        ax.axvline(x, color=TEXT_2, linewidth=0.7, linestyle=(0, (2, 3)))
        ax.text(x, 1.01, label, transform=ax.get_xaxis_transform(), ha="center",
                fontsize=8.5, color=TEXT_2)
    ax.set_xlabel("live orders in the table")
    ax.set_ylabel("mean ns per operation (log scale)")
    ax.legend(fontsize=8.5, frameon=False, labelcolor=TEXT, loc="upper center",
              bbox_to_anchor=(0.5, -0.17), ncol=3)
    fig.suptitle("Find, erase, insert on random live orders, as the table grows",
                 x=0.02, ha="left", fontsize=12, color=TEXT)
    fig.text(0.02, 0.915, "Dotted lines: where the open-addressing table (32 bytes per live order) "
             "outgrows each cache", fontsize=9, color=TEXT_2)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(out, dpi=200, facecolor=SURFACE)
    plt.close(fig)


def perf_table(itch, perf):
    count = {r["table"]: int(r["count"]) for r in itch if r["mode"] == "tp"}
    order = sorted((t for t in REAL_DAY if t in count and t in perf),
                   key=lambda t: perf[t]["cycles"] / count[t])
    head = ["Container", "cycles/op", "instructions/op"] + \
        [f"loads from {v}/op" for v in FILLS.values()] + ["dTLB misses/op", "page faults"]
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for t in order:
        ev, n = perf[t], count[t]
        cells = [NAMES[t], f"{ev['cycles'] / n:.0f}", f"{ev['instructions'] / n:.0f}"]
        cells += [f"{ev.get(k, 0) / n:.2f}" for k in FILLS]
        cells += [f"{ev.get('amd-dtlb-misses', 0) / n:.2f}", f"{ev.get('page-faults', 0):,.0f}"]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def tail_table(itch):
    lat = {r["table"]: r for r in itch if r["mode"] == "lat" and r["op"] == "all"}
    order = sorted((t for t in REAL_DAY if t in lat), key=lambda t: float(lat[t]["p50_ns"]))
    lines = ["| Container | p50 | p99 | p99.9 |", "|---|---|---|---|"]
    for t in order:
        r = lat[t]
        lines.append(f"| {NAMES[t]} | {float(r['p50_ns']):.0f} ns | {float(r['p99_ns']):.0f} ns "
                     f"| {float(r['p999_ns']):.0f} ns |")
    return "\n".join(lines)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    itch = read_results(src / "itch" / "results.csv")
    churn = read_results(src / "churn" / "results.csv")
    perf = read_perf(src / "itch" / "perf.csv")

    real_day_chart(itch, out / "hashtable_real_day.png")
    sweep_chart(churn, out / "hashtable_size_sweep.png")
    md = [
        "## Hardware counters per operation, real day (operations back to back)\n",
        perf_table(itch, perf),
        "\n## Latency of a single operation, real day (each operation timed on its own)\n",
        tail_table(itch),
    ]
    (out / "hashtable_tables.md").write_text("\n".join(md) + "\n")


if __name__ == "__main__":
    main()
