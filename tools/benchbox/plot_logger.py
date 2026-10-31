import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

from plot_hashtable import GRID, SURFACE, TEXT, TEXT_2, style

NAMES = {
    "logger": "fastbook logger",
    "format_to": "std::format_to (no I/O)",
    "fprintf": "fprintf",
    "ofstream": "std::ofstream",
    "write": "write(2)",
}
COMPARE = ["logger", "format_to", "fprintf", "ofstream", "write"]
PCTS = [("p50_ticks", "p50", "#9ec5f4", "o"), ("p99_ticks", "p99", "#2a78d6", "s"),
        ("p999_ticks", "p99.9", "#0d3b7a", "D")]
PLACEMENT = {"4": ("Background thread on cpu 4 (same L3)", "#2a78d6"),
             "1": ("Background thread on cpu 1 (other L3)", "#eb6834")}
NEAR = "4"


def read_csv(path):
    return list(csv.DictReader(open(path)))


def read_perf(path):
    out = defaultdict(dict)
    for r in read_csv(path):
        out[(r["mode"], r["contender"], r["log_cpu"], r["offered_rate"])][r["event"]] = \
            float(r["value"])
    return out


def written(r):
    return float(r["achieved_rate"]) * (1 - int(r["drops"]) / int(r["calls"]))


def count_label(x, _):
    if x >= 1e6:
        return f"{x / 1e6:g}M"
    if x >= 1e3:
        return f"{x / 1e3:g}K"
    return f"{x:g}"


def compare_chart(rows, out):
    by = {r["contender"]: r for r in rows if r["mode"] == "compare" and r["log_cpu"] == NEAR}
    order = [c for c in COMPARE if c in by]

    fig, ax = plt.subplots(figsize=(8, 4.2), facecolor=SURFACE)
    style(ax)
    for y, c in enumerate(order):
        vals = [float(by[c][k]) for k, *_ in PCTS]
        ax.plot([min(vals), max(vals)], [y, y], color=GRID, linewidth=2.2, zorder=1)
        for z, ((k, label, color, marker), v) in enumerate(zip(PCTS, vals)):
            ax.scatter(v, y, s=60, color=color, marker=marker, edgecolors=SURFACE,
                       linewidths=1.2, zorder=5 - z, label=label if y == 0 else None)
        ax.text(vals[0] / 1.15, y, f"{vals[0]:,.0f}", va="center", ha="right",
                fontsize=9.5, color=TEXT, fontweight=600)
    ax.set_xscale("log")
    ax.set_xlim(10, 300_000)
    ax.xaxis.set_major_locator(LogLocator(base=10))
    ax.xaxis.set_major_formatter(FuncFormatter(count_label))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.set_yticks(range(len(order)), [NAMES[c] for c in order], fontsize=10, color=TEXT,
                  fontweight=600)
    ax.tick_params(axis="y", length=0)
    ax.set_ylim(len(order) - 0.5, -0.5)
    ax.spines["left"].set_visible(False)
    ax.xaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.set_xlabel("cycles per call (log scale)")
    ax.legend(fontsize=9, frameon=False, labelcolor=TEXT, loc="upper center",
              bbox_to_anchor=(0.5, -0.16), ncol=3)
    fig.suptitle("Cost of One Log Call on the Hot Thread", x=0.02, ha="left", fontsize=12,
                 color=TEXT)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out, dpi=200, facecolor=SURFACE)
    plt.close(fig)


def rate_rows(rows, cpu):
    return sorted((r for r in rows if r["mode"] == "rate" and r["log_cpu"] == cpu and (
        r["offered_rate"] == "0" or float(r["offered_rate"]) < 1.05 * float(r["achieved_rate"]))),
        key=lambda r: float(r["achieved_rate"]))


def saturation_chart(rows, out):
    rate = rate_rows(rows, NEAR)
    xs = [float(r["achieved_rate"]) for r in rate]
    ys = [written(r) for r in rate]
    lo, hi, cap = 5e4, max(xs) * 1.5, max(ys)

    fig, ax = plt.subplots(figsize=(8, 4.8), facecolor=SURFACE)
    style(ax)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.axvspan(cap, hi, color=GRID, alpha=0.5, linewidth=0)
    ax.text(cap * 1.08, lo * 1.3, "ring full:\nextra calls\nare dropped", fontsize=8.5,
            color=TEXT_2, va="bottom")
    ax.plot([lo, hi], [lo, hi], color=TEXT_2, linewidth=0.8, linestyle=(0, (2, 3)))
    ax.text(1.3e5, 1.3e5 * 1.4, "every call written", rotation=31, fontsize=8.5, color=TEXT_2)
    ax.plot(xs, ys, color=PLACEMENT[NEAR][1], linewidth=1.6, marker="o", markersize=4.5,
            markeredgecolor=SURFACE, markeredgewidth=1.2)
    last = rate[-1]
    ax.text(xs[-1], ys[-1] * 1.25, f"{ys[-1] / 1e6:.1f}M written,\n"
            f"{int(last['drops']) / int(last['calls']):.0%} dropped", ha="center", va="bottom",
            fontsize=9, color=TEXT, fontweight=600)
    ax.set_xlim(lo, hi)
    ax.set_ylim(lo, hi)
    for axis in (ax.xaxis, ax.yaxis):
        axis.set_major_formatter(FuncFormatter(count_label))
        axis.set_minor_formatter(NullFormatter())
    ax.set_xlabel("log calls per second on the hot thread (log scale)")
    ax.set_ylabel("calls written per second (log scale)")
    fig.suptitle("Logging Faster Than the Background Thread Can Write", x=0.02, ha="left",
                 fontsize=12, color=TEXT)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out, dpi=200, facecolor=SURFACE)
    plt.close(fig)


def placement_chart(rows, out, rates=("100000", "250000", "500000", "1000000")):
    light = {"4": "#9ec5f4", "1": "#f6b994"}
    short = {"4": "same L3", "1": "other L3"}
    bars = [(cpu, k, pct, light[cpu] if pct == "p50" else color)
            for cpu, (_, color) in PLACEMENT.items()
            for k, pct in (("p50_ticks", "p50"), ("p99_ticks", "p99"))]

    fig, ax = plt.subplots(figsize=(8, 4.6), facecolor=SURFACE)
    style(ax)
    width = 0.19
    for i, (cpu, k, pct, color) in enumerate(bars):
        by = {r["offered_rate"]: r for r in rows if r["mode"] == "rate" and r["log_cpu"] == cpu}
        vals = [float(by[rt][k]) for rt in rates]
        xs = [g + (i - 1.5) * (width + 0.02) for g in range(len(rates))]
        ax.bar(xs, vals, width=width, color=color,
               label=f"{pct}, background thread on cpu {cpu} ({short[cpu]})")
        for x, v in zip(xs, vals):
            ax.text(x, v + 8, f"{v:.0f}", ha="center", va="bottom", fontsize=8.5, color=TEXT,
                    fontweight=600)
    ax.set_xticks(range(len(rates)), [f"{count_label(int(rt), 0)} calls/s" for rt in rates],
                  fontsize=10, color=TEXT)
    ax.tick_params(axis="x", length=0)
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.set_ylabel("cycles per call")
    ax.legend(fontsize=8.5, frameon=False, labelcolor=TEXT, loc="upper center",
              bbox_to_anchor=(0.5, -0.1), ncol=2)
    fig.suptitle("Hot Thread p50 and p99, by Where the Background Thread Runs", x=0.02,
                 ha="left", fontsize=12, color=TEXT)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out, dpi=200, facecolor=SURFACE)
    plt.close(fig)


def per_call(perf, key, base, event, calls):
    return (perf[key].get(event, 0) - perf[base].get(event, 0)) / calls


def compare_table(rows, perf):
    by = {r["contender"]: r for r in rows if r["mode"] == "compare" and r["log_cpu"] == NEAR}
    base = ("compare", "empty", NEAR, "0")
    lines = ["| Contender | mean | p50 | p99 | p99.9 | max | user-mode cycles | in the kernel "
             "| instructions |", "|---|---|---|---|---|---|---|---|---|"]
    for c in COMPARE:
        if c not in by:
            continue
        r = by[c]
        n = int(r["calls"])
        key = ("compare", c, NEAR, "0")
        mean = float(r["mean_ticks"])
        user = per_call(perf, key, base, "cycles", n)
        instr = per_call(perf, key, base, "instructions", n)
        kernel = max(0.0, 1 - user / mean)
        lines.append(f"| {NAMES[c]} | {mean:,.0f} | {float(r['p50_ticks']):,.0f} "
                     f"| {float(r['p99_ticks']):,.0f} | {float(r['p999_ticks']):,.0f} "
                     f"| {float(r['max_ticks']):,.0f} | {user:,.0f} | {kernel:.0%} | {instr:,.0f} |")
    return "\n".join(lines)


def placement_table(rows, perf, low="100000", mid="1000000"):
    def rate_row(cpu, rate):
        return next(r for r in rows if r["mode"] == "rate" and r["log_cpu"] == cpu
                    and r["offered_rate"] == rate)

    lines = [f"| Background thread | p99 at {count_label(int(low), 0)} calls/s "
             f"| p99 at {count_label(int(mid), 0)} calls/s "
             f"| fills from L3 or another core per call at {count_label(int(mid), 0)}/s "
             "| most calls written per second |", "|---|---|---|---|---|"]
    for cpu, (label, _) in PLACEMENT.items():
        lo, m = rate_row(cpu, low), rate_row(cpu, mid)
        fills = perf[("rate", "logger", cpu, mid)].get("amd-l3-fills", 0) / int(m["calls"])
        best = max(written(r) for r in rows if r["mode"] == "rate" and r["log_cpu"] == cpu)
        lines.append(f"| {label} | {float(lo['p99_ticks']):.0f} | {float(m['p99_ticks']):.0f} "
                     f"| {fills:.2f} | {best / 1e6:.2f}M |")
    return "\n".join(lines)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    rows = read_csv(src / "results.csv")
    perf = read_perf(src / "perf.csv")

    compare_chart(rows, out / "logger_compare.png")
    saturation_chart(rows, out / "logger_saturation.png")
    placement_chart(rows, out / "logger_placement.png")
    md = [
        "## One call at a time, ring never full (cycles per call)\n",
        compare_table(rows, perf),
        "\n## Where the background thread runs (cycles per call)\n",
        placement_table(rows, perf),
    ]
    (out / "logger_tables.md").write_text("\n".join(md) + "\n")


if __name__ == "__main__":
    main()
