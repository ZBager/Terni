# SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
# Copyright (C) 2026 Rafał Zając
"""Adds terni_bench JSON results of one machine to bench/charts/results.csv.

    python3 bench/charts/add_results.py "RTX 4050 laptop" medium.json large.json ...

Rows of that machine with the same size, kind and read-back mode are replaced. Each
run becomes one row: kind discrete, integrated or lavapipe for Vulkan devices, cpu_1
and cpu_all for the CPU reference on one thread and on the most threads measured. Runs
beside the synthetic renderer (--frame-load) are left out.
"""

import csv
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "results.csv")
FIELDS = ["machine", "kind", "device", "size", "readback", "total_ms", "compute_ms"]


def rows_of(machine, path):
    with open(path) as f:
        d = json.load(f)
    size, readback, out = d["world"]["size"], d.get("readback", "ui"), []
    cpu = [r for r in d["runs"] if r["kind"] == "cpu reference" and "total_ms" in r]
    most = max((r["threads"] for r in cpu), default=0)
    for r in d["runs"]:
        if "total_ms" not in r:
            continue  # skipped or failed: no result, shown as N/A
        if r.get("frame_load_percent", 0) > 0:
            continue  # beside a synthetic renderer (--frame-load): not comparable with the charts
        if r["kind"] == "cpu reference":
            if r["threads"] not in (1, most):
                continue
            kinds = ["cpu_1"] if r["threads"] == 1 else []
            kinds += ["cpu_all"] if r["threads"] == most else []
            for k in kinds:
                out.append([machine, k, f"CPU reference, {r['threads']} thread{'' if r['threads'] == 1 else 's'}", size, readback,
                            r["total_ms"]["median"], r["total_ms"]["median"]])
            continue
        kind = {"discrete": "discrete", "integrated": "integrated"}.get(r["device_type"], "lavapipe")
        out.append([machine, kind, r["device"], size, readback, r["total_ms"]["median"],
                    r["compute_ms"]["median"]])
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    machine, new = sys.argv[1], []
    for path in sys.argv[2:]:
        new += rows_of(machine, path)
    keys = {(r[0], r[1], r[3], r[4]) for r in new}
    old = []
    if os.path.exists(CSV):
        with open(CSV, newline="") as f:
            old = [r for r in csv.reader(f)][1:]
    kept = [r for r in old if (r[0], r[1], r[3], r[4]) not in keys]
    with open(CSV, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(FIELDS)
        w.writerows(kept + new)
    print(f"{len(new)} rows for {machine}; {len(kept) + len(new)} in all")
    return 0


if __name__ == "__main__":
    sys.exit(main())
