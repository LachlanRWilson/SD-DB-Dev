#!/usr/bin/env python3
"""Markdown tables and crossover observations from one benchmark CSV.

Usage: summarize.py <results.csv> [--dist random] [--mode warm] [--dcache on|off|n/a]

Prints, per operation, median and max for each implementation across N, and
the smallest N (if any) at which the AVL median drops below the sorted array
median. Standard library only.
"""
import argparse
import csv
from collections import defaultdict

IMPLS = ["sorted_array", "sorted_array_wordcopy", "avl_tree"]
SHORT = {"sorted_array": "array", "sorted_array_wordcopy": "array_wc", "avl_tree": "avl"}
OPS = ["bulk_insert", "bulk_rebuild", "insert_random", "insert_front", "insert_end", "remove_random", "remove_first",
       "remove_last", "find_hit", "find_miss", "update_message", "traverse_all", "traverse_per_entry", "page_rank20",
       "page_name20"]


def load(path, dist, mode, dcache):
    rows = defaultdict(dict)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["distribution"] != dist or r["cache_mode"] != mode or r["dcache"] != dcache:
                continue
            rows[r["operation"]][(r["impl"], int(r["N"]))] = r
    return rows


def crossover(by_key, sizes):
    """Smallest N from which the AVL median stays below the array median for
    every larger N in the sweep, or None."""
    both = [n for n in sizes if ("sorted_array", n) in by_key and ("avl_tree", n) in by_key]
    found = None
    for n in reversed(both):
        if int(by_key[("avl_tree", n)]["cycles_median"]) < int(by_key[("sorted_array", n)]["cycles_median"]):
            found = n
        else:
            break
    return found


def close_points(by_key, sizes, noise):
    """N values where the two medians differ by less than 10% of the larger
    one or by less than the median cost of an empty timed region."""
    out = []
    for n in sizes:
        a, t = by_key.get(("sorted_array", n)), by_key.get(("avl_tree", n))
        if not (a and t):
            continue
        ma, mt = int(a["cycles_median"]), int(t["cycles_median"])
        if abs(ma - mt) < max(noise, 0.10 * max(ma, mt)):
            out.append(n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--dist", default="random")
    ap.add_argument("--mode", default="warm")
    ap.add_argument("--dcache", default=None)
    ap.add_argument("--ops", default=",".join(OPS))
    args = ap.parse_args()

    dcache = args.dcache
    if dcache is None:
        with open(args.csv, newline="") as f:
            dcache = "n/a" if any(r["dcache"] == "n/a" for r in csv.DictReader(f)) else "on"
    rows = load(args.csv, args.dist, args.mode, dcache)
    timer = load(args.csv, "n/a", args.mode, dcache).get("empty_timed_region", {}).get(("timer", 0))
    noise = int(timer["cycles_median"]) if timer else 0

    for op in args.ops.split(","):
        by_key = rows.get(op)
        if not by_key:
            continue
        impls = [i for i in IMPLS if any(k[0] == i for k in by_key)]
        sizes = sorted({k[1] for k in by_key})
        head = " | ".join(f"{SHORT[i]} med | {SHORT[i]} max" for i in impls)
        print(f"\n**{op}** ({args.dist}, {args.mode}, dcache {dcache})\n")
        print(f"| N | {head} | tb avg (array/avl) |")
        print("|" + "---:|" * (1 + 2 * len(impls) + 1))
        for n in sizes:
            cells = []
            for i in impls:
                r = by_key.get((i, n))
                cells += [r["cycles_median"], r["cycles_max"]] if r else ["-", "-"]
            tb = "/".join(by_key[(i, n)]["tiebreak_calls_avg"] for i in ("sorted_array", "avl_tree") if (i, n) in by_key)
            print(f"| {n} | " + " | ".join(cells) + f" | {tb} |")
        x = crossover(by_key, sizes)
        msg = f"from N = {x} the AVL median is below the array median for every larger N in the sweep." if x else "none (the AVL median is not below the array median at the largest N)."
        close = close_points(by_key, sizes, noise)
        if close:
            msg += f" Medians within 10% (or within the {noise} timer overhead) at N = {', '.join(map(str, close))}: too close to call there."
        print(f"\nCrossover: {msg}")


if __name__ == "__main__":
    main()
