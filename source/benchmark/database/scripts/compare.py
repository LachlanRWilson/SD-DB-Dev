#!/usr/bin/env python3
"""Compare two database benchmark CSVs of the same benchmark (standard library only).

Usage: scripts/compare.py <base.csv> <new.csv> [--stat median|mean|p99]

Prints new vs base per (operation, N) and the change in percent.
"""
import argparse
import csv


def key(r):
    return (r["operation"], int(r["n_contacts"]), int(r["n_chats"]), int(r["msgs_per_chat"]), int(r["history_len"]))


def ms(ns):
    return f"{ns / 1e6:.2f}" if ns >= 10_000 else f"{ns / 1e3:.3f}µs"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("new")
    ap.add_argument("--stat", default="median", choices=["min", "median", "p99", "max", "mean"])
    a = ap.parse_args()
    base = {key(r): r for r in csv.DictReader(open(a.base))}
    new = list(csv.DictReader(open(a.new)))
    col = f"ns_{a.stat}"
    print(f"| operation | N | base {a.stat} (ms) | new {a.stat} (ms) | change |")
    print("|---|---:|---:|---:|---:|")
    for r in new:
        b = base.get(key(r))
        if b is None:
            continue
        x, y = int(b[col]), int(r[col])
        print(f"| {r['operation']} | {int(r['n_contacts']):,} | {ms(x)} | {ms(y)} | {100.0 * (y - x) / x:+.1f}% |")


if __name__ == "__main__":
    main()
