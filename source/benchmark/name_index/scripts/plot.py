#!/usr/bin/env python3
"""Log-log SVG plots of median and max cost versus N, one file per operation.

Usage: plot.py <results.csv> [--dist random] [--mode warm] [--dcache on|off|n/a] [--out plots]

Solid lines are medians, dashed lines are maxima. Standard library only
(no matplotlib), so it runs on the lab machine and in CI.
"""
import argparse
import csv
import math
import os
from collections import defaultdict

COLOURS = {"sorted_array": "#1f77b4", "sorted_array_wordcopy": "#2ca02c", "avl_tree": "#d62728"}
W, H, L, R, T, B = 640, 420, 70, 170, 30, 50


def log_ticks(lo, hi):
    return [10 ** e for e in range(math.floor(math.log10(lo)), math.ceil(math.log10(hi)) + 1)]


def plot(op, series, unit, title, path):
    xs = [n for pts in series.values() for n, _, _ in pts]
    ys = [v for pts in series.values() for _, med, mx in pts for v in (med, mx) if v > 0]
    if not xs or not ys:
        return
    x0, x1 = math.log10(min(xs)), math.log10(max(xs))
    y0, y1 = math.log10(min(ys)), math.log10(max(ys))
    y0, y1 = math.floor(y0), math.ceil(y1) if y1 > y0 else y0 + 1
    sx = lambda n: L + (math.log10(n) - x0) / max(x1 - x0, 1e-9) * (W - L - R)
    sy = lambda v: T + (1 - (math.log10(v) - y0) / (y1 - y0)) * (H - T - B)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="sans-serif" font-size="11">',
           f'<rect width="{W}" height="{H}" fill="white"/>',
           f'<text x="{L}" y="18" font-size="13">{title}</text>',
           f'<rect x="{L}" y="{T}" width="{W - L - R}" height="{H - T - B}" fill="none" stroke="#888"/>']
    for v in log_ticks(10 ** y0, 10 ** y1):
        if 10 ** y0 <= v <= 10 ** y1:
            y = sy(v)
            out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{W - R}" y2="{y:.1f}" stroke="#eee"/>')
            out.append(f'<text x="{L - 6}" y="{y + 4:.1f}" text-anchor="end">{v:g}</text>')
    for n in sorted(set(xs)):
        x = sx(n)
        out.append(f'<line x1="{x:.1f}" y1="{H - B}" x2="{x:.1f}" y2="{H - B + 4}" stroke="#888"/>')
        out.append(f'<text x="{x:.1f}" y="{H - B + 16}" text-anchor="middle" font-size="9">{n}</text>')
    out.append(f'<text x="{(L + W - R) / 2}" y="{H - 10}" text-anchor="middle">N (entries)</text>')
    out.append(f'<text x="14" y="{(T + H - B) / 2}" transform="rotate(-90 14 {(T + H - B) / 2})" text-anchor="middle">{unit}</text>')

    for k, (impl, pts) in enumerate(sorted(series.items())):
        pts = sorted(pts)
        c = COLOURS.get(impl, "#555")
        for idx, dash in ((1, ""), (2, ' stroke-dasharray="5,3"')):
            d = " ".join(f"{sx(p[0]):.1f},{sy(max(p[idx], 1)):.1f}" for p in pts)
            out.append(f'<polyline points="{d}" fill="none" stroke="{c}" stroke-width="1.6"{dash}/>')
        y = T + 14 + 30 * k
        out.append(f'<line x1="{W - R + 10}" y1="{y}" x2="{W - R + 30}" y2="{y}" stroke="{c}" stroke-width="1.6"/>')
        out.append(f'<text x="{W - R + 34}" y="{y + 4}">{impl} median</text>')
        out.append(f'<line x1="{W - R + 10}" y1="{y + 13}" x2="{W - R + 30}" y2="{y + 13}" stroke="{c}" stroke-width="1.6" stroke-dasharray="5,3"/>')
        out.append(f'<text x="{W - R + 34}" y="{y + 17}">{impl} max</text>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--dist", default="random")
    ap.add_argument("--mode", default="warm")
    ap.add_argument("--dcache", default=None)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    with open(args.csv, newline="") as f:
        rows = list(csv.DictReader(f))
    host = any(r["dcache"] == "n/a" for r in rows)
    dcache = args.dcache or ("n/a" if host else "on")
    unit = "ns (host, non-authoritative)" if host else "cycles"
    out_dir = args.out or os.path.join(os.path.dirname(os.path.abspath(args.csv)), "..", "plots")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(args.csv))[0]

    by_op = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if r["distribution"] == args.dist and r["cache_mode"] == args.mode and r["dcache"] == dcache and r["impl"] in COLOURS:
            by_op[r["operation"]][r["impl"]].append((int(r["N"]), int(r["cycles_median"]), int(r["cycles_max"])))
    for op, series in by_op.items():
        name = f"{stem}_{args.dist}_{args.mode}_{op}.svg"
        plot(op, series, unit, f"{op}: {args.dist}, {args.mode}, dcache {dcache} ({stem})", os.path.join(out_dir, name))
        print(os.path.join(out_dir, name))


if __name__ == "__main__":
    main()
