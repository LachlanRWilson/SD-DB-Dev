#!/usr/bin/env python3
"""Markdown tables from a database benchmark CSV (standard library only).

Usage: scripts/summarize.py results/<file>.csv [--stat median|mean|p99|max|min] [--long]

Contacts: one table of the chosen time statistic (operation x N) and one of
sectors read / written. Messages (or --long): one row per CSV row with the
database state it ran against, median, p99 and sectors.
"""
import argparse
import csv
from collections import OrderedDict


def fmt_ns(ns):
    if ns >= 1_000_000:
        return f"{ns / 1e6:.2f} ms"
    if ns >= 1_000:
        return f"{ns / 1e3:.1f} µs"
    return f"{ns} ns"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--stat", default="median", choices=["min", "median", "p99", "max", "mean"])
    ap.add_argument("--long", action="store_true")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.csv)))
    if args.long or rows[0]["bench"] != "contacts":
        print(f"({rows[0]['platform']}, {rows[0]['opt_level']})\n")
        print("| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in rows:
            m = r["msgs_per_chat"] if r["msgs_per_chat"] != "0" else "–"
            print(f"| {r['operation']} | {int(r['n_contacts']):,} | {int(r['n_chats']):,} | {m} | {int(r['history_len']):,} | "
                  f"{r['reps']} | {fmt_ns(int(r['ns_median']))} | {fmt_ns(int(r['ns_p99']))} | "
                  f"{float(r['sectors_read_avg']):.2f} / {float(r['sectors_written_avg']):.2f} |")
        return
    ops = list(OrderedDict.fromkeys(r["operation"] for r in rows))
    ns = sorted({int(r["n_contacts"]) for r in rows})
    cell = {(r["operation"], int(r["n_contacts"])): r for r in rows}

    print(f"**{args.stat} time** ({rows[0]['platform']}, {rows[0]['opt_level']})\n")
    print("| operation | " + " | ".join(f"N={n:,}" for n in ns) + " |")
    print("|---|" + "---:|" * len(ns))
    for op in ops:
        vals = [fmt_ns(int(cell[(op, n)][f"ns_{args.stat}"])) if (op, n) in cell else "–" for n in ns]
        print(f"| {op} | " + " | ".join(vals) + " |")

    print("\n**sectors per call** (read / written)\n")
    print("| operation | " + " | ".join(f"N={n:,}" for n in ns) + " |")
    print("|---|" + "---:|" * len(ns))
    for op in ops:
        vals = []
        for n in ns:
            r = cell.get((op, n))
            vals.append(f"{float(r['sectors_read_avg']):.2f} / {float(r['sectors_written_avg']):.2f}" if r else "–")
        print(f"| {op} | " + " | ".join(vals) + " |")


if __name__ == "__main__":
    main()
