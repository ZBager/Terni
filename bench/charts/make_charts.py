# SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
# Copyright (C) 2026 Rafał Zając
"""Draws the charts in docs/benchmark.md from bench/charts/results.csv.

    python3 bench/charts/make_charts.py

Needs Python 3 and matplotlib; only for updating the documentation. Every chart is
written twice, for light and dark pages: docs/images/benchmark/<name>-light.svg and
<name>-dark.svg.
"""

import csv
import math
import os
from collections import defaultdict

import matplotlib

matplotlib.use("svg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, LogNorm  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Rectangle  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "docs", "images", "benchmark")

TIERS = ["tiny", "small", "medium", "large", "huge", "giant", "colossal"]
POPS = {"tiny": 1024, "small": 50_000, "medium": 1_000_000, "large": 6_000_000, "huge": 10_000_000,
        "giant": 50_000_000, "colossal": 100_000_000}
POP_LABEL = {"tiny": "1K", "small": "50K", "medium": "1M", "large": "6M", "huge": "10M", "giant": "50M",
             "colossal": "100M"}
MACHINES = ["RTX 4050 laptop", "RTX 3070 laptop", "Desktop", "Desktop, Windows", "Intel laptop", "Steam Deck"]
FULL = "RTX 4050 laptop"  # the machine with results at every size

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#898781", grid="#e1e0d9",
                  axis="#c3c2b7", mid="#f0efec", gpu="#2a78d6", cpu="#eb6834", igpu="#1baf7a",
                  lavapipe="#eda100", blue_dark="#184f95", red_dark="#a83232"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", muted="#898781", grid="#2c2c2a",
                 axis="#383835", mid="#383835", gpu="#3987e5", cpu="#d95926", igpu="#199e70",
                 lavapipe="#c98500", blue_dark="#6da7ec", red_dark="#e66767"),
}


def load():
    rows = []
    with open(os.path.join(HERE, "results.csv"), newline="") as f:
        for r in csv.DictReader(f):
            r["total_ms"] = float(r["total_ms"]) if r["total_ms"] else None
            r["compute_ms"] = float(r["compute_ms"]) if r["compute_ms"] else None
            rows.append(r)
    return rows


def find(rows, machine, kind, size, readback="pops"):
    """The run of one kind on one machine and size; CPU runs read nothing back, so any mode does."""
    for r in rows:
        if r["machine"] == machine and r["kind"] == kind and r["size"] == size and (
                r["readback"] == readback or kind.startswith("cpu")):
            return r
    return None


def style(fig, ax, t):
    fig.patch.set_facecolor(t["surface"])
    ax.set_facecolor(t["surface"])
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(t["axis"])
        ax.spines[side].set_linewidth(1)
    ax.tick_params(colors=t["muted"], labelcolor=t["ink2"], labelsize=9, length=0)
    ax.grid(True, which="major", color=t["grid"], linewidth=1, linestyle="-")
    ax.set_axisbelow(True)


def title(fig, t, text, sub):
    h = fig.get_figheight()  # place in inches, whatever the figure's height
    fig.text(0.012, 1 - 0.14 / h, text, ha="left", va="top", fontsize=13, fontweight="bold", color=t["ink"])
    fig.text(0.012, 1 - 0.45 / h, sub, ha="left", va="top", fontsize=9.5, color=t["ink2"])


def short(device):
    """A device name as short as the charts need it."""
    for prefix in ("NVIDIA GeForce ", "AMD "):
        if device.startswith(prefix):
            device = device[len(prefix):]
    device = device.replace("Intel(R) ", "").replace(" Laptop GPU", " Laptop")
    device = device.replace("Ryzen 7 7800X3D 8-Core Processor", "7800X3D Radeon")
    for cut in (" (RADV", " (KBL"):
        if cut in device:
            device = device[:device.index(cut)]
    if device == "Radeon(TM) Graphics":  # the 7800X3D's, as AMD's Windows driver names it
        device = "7800X3D Radeon"
    return device.replace("Custom GPU 0405", "Steam Deck GPU")


def ms_label(v):
    if v >= 1000:
        return f"{v:,.0f} ms"
    if v >= 100:
        return f"{v:.0f} ms"
    if v >= 10:
        return f"{v:.1f} ms"
    if v >= 1:
        return f"{v:.2f} ms"
    return f"{v:.3f} ms"


def x_label(v):
    return f"{v:.0f}x" if v >= 10 else f"{v:.1f}x"


def save(fig, name, mode):
    os.makedirs(OUT, exist_ok=True)
    fig.savefig(os.path.join(OUT, f"{name}-{mode}.svg"), facecolor=fig.get_facecolor(), metadata={"Date": None})
    plt.close(fig)


def spread(values, gap):
    """Positions for end labels (in log10 units) at least `gap` apart, as near their values as possible."""
    order = sorted(range(len(values)), key=lambda i: values[i])
    pos = [0.0] * len(values)
    last = -1e9
    for i in order:
        pos[i] = max(values[i], last + gap)
        last = pos[i]
    # pull the stack back down so it is centred on the values it labels
    shift = sum(pos[i] - values[i] for i in order) / len(order)
    for i in order:
        pos[i] -= max(0.0, shift)
    for a, b in zip(order, order[1:]):
        pos[b] = max(pos[b], pos[a] + gap)
    return pos


# ---------------------------------------------------------------------------------------
# 1. Every tier on the machine that has them all: time per tick
# ---------------------------------------------------------------------------------------
def tiers_time(rows, mode):
    t = THEMES[mode]
    fig, ax = plt.subplots(figsize=(9.6, 6.4), dpi=100)
    fig.subplots_adjust(left=0.085, right=0.68, top=0.84, bottom=0.21)
    style(fig, ax, t)
    title(fig, t, "Time per tick, from 1,000 to 100 million pops",
          "RTX 4050 laptop (Ryzen 5 7640HS, 12 threads). Lower is faster; the time axis is logarithmic.")

    series = [  # (label, kind, value, colour, filled)
        ("RTX 4050, compute", "discrete", "compute_ms", "gpu", True),
        ("RTX 4050, every pop read back", "discrete", "total_ms", "gpu", False),
        ("Radeon 760M, every pop read back", "integrated", "total_ms", "igpu", True),
        ("lavapipe, 12 threads", "lavapipe", "total_ms", "lavapipe", True),
        ("CPU reference, 12 threads", "cpu_all", "total_ms", "cpu", True),
        ("CPU reference, 1 thread", "cpu_1", "total_ms", "cpu", False),
    ]
    ends, handles, absent = [], [], []
    for label, kind, key, colour, filled in series:
        xs, ys = [], []
        for i, tier in enumerate(TIERS):
            r = find(rows, FULL, kind, tier)
            if r and r[key] is not None:
                xs.append(i)
                ys.append(r[key])
            else:
                absent.append((i, label))
        c = t[colour]
        ax.plot(xs, ys, color=c, linewidth=2, solid_capstyle="round", solid_joinstyle="round", zorder=3)
        ax.scatter(xs, ys, s=110, facecolor=t["surface"], edgecolor="none", zorder=3.5)  # surface ring
        ax.scatter(xs, ys, s=52, facecolor=c if filled else t["surface"], edgecolor=c, linewidth=2, zorder=4)
        handles.append(Line2D([], [], color=c, linewidth=2, marker="o", markersize=7,
                              markerfacecolor=c if filled else t["surface"], markeredgecolor=c, markeredgewidth=2,
                              label=label))
        last = "" if xs[-1] == len(TIERS) - 1 else f" at {TIERS[xs[-1]]}"
        ends.append((f"{label}: {ms_label(ys[-1])}{last}", xs[-1], ys[-1]))
    for i, label in absent:  # no result at this size
        ax.text(i, 0.014, f"{label.split(',')[0]}\nN/A", ha="center", va="bottom", fontsize=8, color=t["muted"])

    ax.set_yscale("log")
    ax.set_xticks(range(len(TIERS)))
    ax.set_xticklabels([f"{x}\n{POP_LABEL[x]} pops" for x in TIERS])
    ax.minorticks_off()
    ax.set_xlim(-0.3, len(TIERS) - 0.7)
    ax.set_ylim(0.01, 10000)
    ax.set_yticks([0.01, 0.1, 1, 10, 100, 1000, 10000])
    ax.set_yticklabels(["0.01 ms", "0.1 ms", "1 ms", "10 ms", "100 ms", "1 s", "10 s"])

    # direct labels at the right, spread apart and joined to their lines
    pos = spread([math.log10(y) for _, _, y in ends], 0.24)
    for (text, x, y), p in zip(ends, pos):
        ax.annotate(text, xy=(x, y), xytext=(len(TIERS) - 0.45, 10 ** p), textcoords="data", fontsize=8.5,
                    color=t["ink2"], va="center", ha="left", annotation_clip=False,
                    arrowprops=dict(arrowstyle="-", color=t["axis"], linewidth=1, shrinkA=0, shrinkB=6))
    fig.legend(handles=handles, loc="lower left", bbox_to_anchor=(0.012, 0.0), ncol=3, frameon=False,
               fontsize=8.5, labelcolor=t["ink2"], handlelength=2.2, columnspacing=1.4)
    save(fig, "tiers-time", mode)


# ---------------------------------------------------------------------------------------
# 2. The same machine: speed-up over all its CPU threads
# ---------------------------------------------------------------------------------------
def rounded_bars(ax, fig, t, x, width, base, values, colour):
    """Columns from `base` to each value with a 4 px rounded data end, square at the base."""
    fig.canvas.draw()
    to_px = ax.transData.transform
    lo_lim, hi_lim = ax.get_ylim()
    for xi, v in zip(x, values):
        if v is None:
            continue
        v = min(max(v, lo_lim), hi_lim)  # a bar never leaves the plot
        (x0, y0), (x1, y1) = to_px((xi - width / 2, base)), to_px((xi + width / 2, v))
        lo, hi = min(y0, y1), max(y0, y1)
        r = min(4.0, (hi - lo) / 2, (x1 - x0) / 2)
        box = FancyBboxPatch((x0 + r, lo + r), (x1 - x0) - 2 * r, (hi - lo) - 2 * r,
                             boxstyle=f"round,pad={r}", transform=None, facecolor=t[colour], edgecolor="none",
                             zorder=3)
        fig.add_artist(box)
        # square the end at the base
        sq = Rectangle((x0, y0 if v >= base else y0 - r), x1 - x0, r, transform=None, facecolor=t[colour],
                       edgecolor="none", zorder=3)
        fig.add_artist(sq)


def tiers_speedup(rows, mode):
    t = THEMES[mode]
    fig, ax = plt.subplots(figsize=(9.6, 5.4), dpi=100)
    fig.subplots_adjust(left=0.085, right=0.985, top=0.83, bottom=0.21)
    style(fig, ax, t)
    ax.grid(False, axis="x")
    title(fig, t, "How many times faster than all 12 CPU threads",
          "RTX 4050 laptop, per tick. Above 1x the GPU is faster, below it the CPU reference is.")
    series = [
        ("RTX 4050, compute", "discrete", "compute_ms", "gpu"),
        ("RTX 4050, every pop read back", "discrete", "total_ms", "blue_dark"),
        ("Radeon 760M, every pop read back", "integrated", "total_ms", "igpu"),
        ("lavapipe, 12 threads", "lavapipe", "total_ms", "lavapipe"),
    ]
    ax.set_yscale("log")
    ax.set_ylim(1 / 32, 128)
    ax.set_xlim(-0.6, len(TIERS) - 0.4)
    ax.set_yticks([1 / 16, 1 / 4, 1, 4, 16, 64])
    ax.set_yticklabels(["1/16x", "1/4x", "1x", "4x", "16x", "64x"])
    ax.minorticks_off()
    ax.set_xticks(range(len(TIERS)))
    ax.set_xticklabels([f"{x}\n{POP_LABEL[x]} pops" for x in TIERS])
    ax.axhline(1, color=t["axis"], linewidth=1, zorder=2)
    width = 0.17
    handles = []
    for k, (label, kind, key, colour) in enumerate(series):
        xs = [i + (k - 1.5) * (width + 0.03) for i in range(len(TIERS))]
        vals = []
        for tier in TIERS:
            r, cpu = find(rows, FULL, kind, tier), find(rows, FULL, "cpu_all", tier)
            vals.append(cpu["total_ms"] / r[key] if r and cpu and r[key] else None)
        rounded_bars(ax, fig, t, xs, width, 1, vals, colour)
        for xi, v in zip(xs, vals):
            if v is None:
                ax.text(xi, 1.15, "N/A", rotation=90, ha="center", va="bottom", fontsize=8, color=t["muted"])
            elif k == 0:  # value labels on the lead series only; the table has the rest
                ax.text(xi, v * 1.12, x_label(v), ha="center", va="bottom", fontsize=8, color=t["ink2"])
        handles.append(Rectangle((0, 0), 1, 1, facecolor=t[colour], edgecolor="none", label=label))
    fig.legend(handles=handles, loc="lower left", bbox_to_anchor=(0.012, 0.0), ncol=4, frameon=False, fontsize=8.5,
               labelcolor=t["ink2"], handlelength=1.2, columnspacing=1.6)
    save(fig, "tiers-speedup", mode)


# ---------------------------------------------------------------------------------------
# 3. Every GPU and every tier: speed-up over its own machine's CPU threads
# ---------------------------------------------------------------------------------------
GPUS = [  # (row label, machine, kind)
    ("RTX 4050 Laptop", "RTX 4050 laptop", "discrete"),
    ("Radeon 760M (integrated)", "RTX 4050 laptop", "integrated"),
    ("RTX 3070 Laptop", "RTX 3070 laptop", "discrete"),
    ("RTX 4070 Ti", "Desktop", "discrete"),
    ("7800X3D Radeon (integrated)", "Desktop", "integrated"),
    ("Intel HD Graphics 620", "Intel laptop", "integrated"),
    ("Steam Deck GPU", "Steam Deck", "integrated"),
]


def speedup_map(rows, mode):
    t = THEMES[mode]
    fig, axes = plt.subplots(1, 2, figsize=(11.0, 4.9), dpi=100, sharey=True)
    fig.patch.set_facecolor(t["surface"])
    fig.subplots_adjust(left=0.185, right=0.99, top=0.76, bottom=0.25, wspace=0.05)
    title(fig, t, "GPU against its own machine's CPU, every tier",
          "How many times faster than the CPU reference on all threads of the same machine. "
          "N/A: not measured there.")
    cmap = LinearSegmentedColormap.from_list("div", [t["red_dark"], t["mid"], t["blue_dark"]])
    norm = LogNorm(vmin=1 / 32, vmax=32)
    for ax, (panel, key) in zip(axes, [("Compute only", "compute_ms"), ("Every pop read back", "total_ms")]):
        ax.set_facecolor(t["surface"])
        for side in ax.spines.values():
            side.set_visible(False)
        ax.set_title(panel, fontsize=10.5, color=t["ink"], loc="left", pad=8)
        for row, (name, machine, kind) in enumerate(GPUS):
            for col, tier in enumerate(TIERS):
                r, cpu = find(rows, machine, kind, tier), find(rows, machine, "cpu_all", tier)
                v = cpu["total_ms"] / r[key] if r and cpu and r[key] else None
                if v is None:
                    ax.add_patch(Rectangle((col + 0.04, row + 0.06), 0.92, 0.88, facecolor=t["surface"],
                                           edgecolor="none"))
                    ax.text(col + 0.5, row + 0.5, "N/A", ha="center", va="center", fontsize=8.5, color=t["muted"])
                    continue
                fill = cmap(norm(min(max(v, 1 / 32), 32)))
                ax.add_patch(Rectangle((col + 0.04, row + 0.06), 0.92, 0.88, facecolor=fill, edgecolor="none"))
                lum = 0.2126 * fill[0] + 0.7152 * fill[1] + 0.0722 * fill[2]
                ax.text(col + 0.5, row + 0.5, x_label(v), ha="center", va="center", fontsize=8.5,
                        color="#ffffff" if lum < 0.5 else "#0b0b0b")
        ax.set_xlim(0, len(TIERS))
        ax.set_ylim(len(GPUS), 0)
        ax.set_xticks([i + 0.5 for i in range(len(TIERS))])
        ax.set_xticklabels([f"{x}\n{POP_LABEL[x]}" for x in TIERS], fontsize=8.5, color=t["ink2"])
        ax.set_yticks([i + 0.5 for i in range(len(GPUS))])
        ax.set_yticklabels([g[0] for g in GPUS], fontsize=9, color=t["ink2"])
        ax.tick_params(length=0)
    # scale
    cax = fig.add_axes([0.185, 0.07, 0.40, 0.035])
    import numpy as np
    grad = np.logspace(math.log10(1 / 32), math.log10(32), 256)[None, :]
    cax.imshow(grad, aspect="auto", cmap=cmap, norm=norm, extent=[math.log10(1 / 32), math.log10(32), 0, 1])
    cax.set_yticks([])
    ticks = [1 / 32, 1 / 8, 1 / 2, 1, 2, 8, 32]
    cax.set_xticks([math.log10(v) for v in ticks])
    cax.set_xticklabels(["1/32x", "1/8x", "1/2x", "1x", "2x", "8x", "32x"], fontsize=8, color=t["ink2"])
    cax.tick_params(length=0)
    for s in cax.spines.values():
        s.set_visible(False)
    fig.text(0.60, 0.085, "red: the CPU is faster   grey: even   blue: the GPU is faster", fontsize=8.5,
             color=t["ink2"], va="center")
    save(fig, "speedup-map", mode)


# ---------------------------------------------------------------------------------------
# 4. The medium baseline on every machine
# ---------------------------------------------------------------------------------------
KINDS = [  # (row label, kind, colour)
    ("Discrete GPU", "discrete", "gpu"),
    ("Integrated GPU", "integrated", "igpu"),
    ("lavapipe, all threads", "lavapipe", "lavapipe"),
    ("CPU reference, all threads", "cpu_all", "cpu"),
    ("CPU reference, 1 thread", "cpu_1", "cpu"),
]


def medium_machines(rows, mode):
    t = THEMES[mode]
    n = len(MACHINES) * (len(KINDS) + 1)
    fig, ax = plt.subplots(figsize=(9.6, 0.24 * n + 1.9), dpi=100)
    fig.subplots_adjust(left=0.30, right=0.84, top=1 - 1.05 / (0.24 * n + 1.9), bottom=0.55 / (0.24 * n + 1.9))
    style(fig, ax, t)
    ax.grid(False, axis="y")
    for side in ("left",):
        ax.spines[side].set_visible(False)
    title(fig, t, "The medium world (1 million pops) on every machine",
          "Time per tick, logarithmic. Filled dot: compute only. Ring: with every pop read back. "
          "N/A: no such device.")
    ax.set_xscale("log")
    ax.set_xlim(0.1, 400)
    ax.set_xticks([0.1, 1, 10, 100])
    ax.set_xticklabels(["0.1 ms", "1 ms", "10 ms", "100 ms"])
    ax.minorticks_off()
    labels, y = [], 0
    for machine in MACHINES:
        labels.append((y, machine, True))
        y += 1
        for name, kind, colour in KINDS:
            r = find(rows, machine, kind, "medium")
            c = t[colour]
            if r is None:
                labels.append((y, name, False))
                ax.text(0.115, y, "N/A", va="center", ha="left", fontsize=8.5, color=t["muted"])
                y += 1
                continue
            device = short(r["device"])
            labels.append((y, f"{name}: {device}" if kind in ("discrete", "integrated") else name, False))
            total, compute = r["total_ms"], r["compute_ms"]
            if kind.startswith("cpu"):
                ax.scatter([total], [y], s=58, facecolor=c, edgecolor=t["surface"], linewidth=2, zorder=4)
                ax.text(total * 1.18, y, ms_label(total), va="center", fontsize=8.5, color=t["ink2"])
            else:
                ax.plot([compute, total], [y, y], color=c, linewidth=2, solid_capstyle="round", zorder=3)
                ax.scatter([compute], [y], s=58, facecolor=c, edgecolor=t["surface"], linewidth=2, zorder=4)
                ax.scatter([total], [y], s=58, facecolor=t["surface"], edgecolor=c, linewidth=2, zorder=4)
                ax.text(total * 1.18, y, f"{ms_label(total)}  (compute {ms_label(compute)})", va="center",
                        fontsize=8.5, color=t["ink2"])
            y += 1
    ax.set_ylim(y - 0.4, -0.6)
    ax.set_yticks([p for p, _, _ in labels])
    ax.set_yticklabels([s for _, s, _ in labels], fontsize=8.5)
    for tick, (_, _, header) in zip(ax.get_yticklabels(), labels):
        tick.set_color(t["ink"] if header else t["ink2"])
        tick.set_fontweight("bold" if header else "normal")
    save(fig, "medium-machines", mode)


def main():
    rows = load()
    plt.rcParams.update({"svg.fonttype": "path", "font.family": "DejaVu Sans", "svg.hashsalt": "terni"})
    for mode in ("light", "dark"):
        tiers_time(rows, mode)
        tiers_speedup(rows, mode)
        speedup_map(rows, mode)
        medium_machines(rows, mode)


if __name__ == "__main__":
    main()
