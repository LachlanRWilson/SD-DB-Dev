#!/usr/bin/env python3
"""
Collision handling selection evidence.

Replaces collision_eval.py and cuckoo.py with a like-for-like comparison:
  - every method uses the firmware hash (DJB2 16-bit) and the same
    14,293-slot table (same RAM)
  - double hashing uses the firmware probe sequence (hash_table.c)
  - paired datasets (every method sees the same phone numbers)
  - successful and unsuccessful lookup cost, worst case, and SD reads
    caused by 16-bit hash collisions (firmware confirms a match by
    reading the stored phone number from the SD card)
  - cuckoo hashing in its most favourable form: a single table with two
    independent hashes (DJB2 16-bit and FNV-1a 16-bit)

Outputs are written to results/hash_selection/ with a collision_ prefix.
"""

import csv
import math
import random
import statistics
from pathlib import Path

import matplotlib.pyplot as plt

from full_hash_eval import fnv1a_folded, generate_phone_number
from hash_selection_eval import (
    HASH_TABLE_SIZE,
    RANDOM_SEED,
    DESIGN_CONTACTS,
    OUTPUT_DIR,
    HIGHLIGHT,
    OTHER,
    INK,
    INK_SECONDARY,
    djb2_folded,
)


# ============================================================
# Configuration
# ============================================================

SWEEP_LOADS = [round(0.05 * i, 2) for i in range(1, 19)]  # 5% .. 90%
SWEEP_SIMULATIONS = 20

DESIGN_LOAD = 0.70
DESIGN_SIMULATIONS = 100

# Absent numbers looked up per simulation (unsuccessful search)
NUM_MISSES = 2_000

# Cuckoo eviction limit before an insert is declared failed
CUCKOO_MAX_KICKS = 500

SELECTED = "Double hashing"
METHODS = ["Linear probing", "Double hashing", "Cuckoo hashing"]

# Secondary colour for the non-selected probing method
LINEAR_COLOUR = "#eb6834"


# ============================================================
# Datasets
# ============================================================

def unique_phones(count):
    """Unique numbers (a duplicate would be an update, not an insert)."""

    # dict keeps insertion order (a set's order changes every run)
    phones = {}

    while len(phones) < count:
        phones[generate_phone_number()] = None

    return list(phones)


def absent_phones(count, present):

    misses = {}

    while len(misses) < count:
        phone = generate_phone_number()
        if phone not in present:
            misses[phone] = None

    return list(misses)


# ============================================================
# Open addressing (linear probing / firmware double hashing)
# ============================================================

def probe_step(key, capacity, method):

    if method == "Linear probing":
        return 1

    # hash_secondary(): must never be 0
    return 1 + key % (capacity - 1)


def open_addressing(phones, misses, capacity, method):
    """
    Insert all phones, then look up every phone and every miss.

    Probes are slots examined. SD reads count slots whose stored 16-bit
    hash equals the target hash, as the firmware must read the phone
    number back from the SD card to confirm or reject the match.
    """

    slot_key = [None] * capacity
    slot_phone = [None] * capacity
    hit_probes = []
    hit_sd_reads = []

    for phone in phones:

        key = djb2_folded(phone)
        index = key % capacity
        step = probe_step(key, capacity, method)
        probes = 1
        sd_reads = 1  # the read that returns the contact

        while slot_key[index] is not None:
            if slot_key[index] == key:
                sd_reads += 1
            index = (index + step) % capacity
            probes += 1

        slot_key[index] = key
        slot_phone[index] = phone

        # No deletions, so a later lookup follows exactly this chain
        hit_probes.append(probes)
        hit_sd_reads.append(sd_reads)

    miss_probes = []
    miss_sd_reads = []

    for phone in misses:

        key = djb2_folded(phone)
        index = key % capacity
        step = probe_step(key, capacity, method)
        probes = 1
        sd_reads = 0

        while slot_key[index] is not None:
            if slot_key[index] == key:
                sd_reads += 1
            index = (index + step) % capacity
            probes += 1

        miss_probes.append(probes)
        miss_sd_reads.append(sd_reads)

    return {
        "inserted": len(phones),
        "insert_failures": 0,
        "hit_probes": hit_probes,
        "miss_probes": miss_probes,
        "hit_sd_reads": hit_sd_reads,
        "miss_sd_reads": miss_sd_reads,
        "relocations": [0],
    }


# ============================================================
# Cuckoo hashing (single table, two independent hashes)
# ============================================================

def cuckoo(phones, misses, capacity):
    """
    Insert until the first failure (the table would then need a full
    rehash), then look up every inserted phone and every miss.
    """

    def positions(phone):
        return (djb2_folded(phone) % capacity,
                fnv1a_folded(phone) % capacity)

    table = [None] * capacity
    relocations = []
    inserted = []

    for phone in phones:

        current = phone
        p1, p2 = positions(current)

        if table[p1] is None:
            table[p1] = current
        elif table[p2] is None:
            table[p2] = current
        else:
            position = p1
            kicks = 0

            while kicks < CUCKOO_MAX_KICKS:
                current, table[position] = table[position], current
                kicks += 1
                a, b = positions(current)
                position = b if position == a else a
                if table[position] is None:
                    table[position] = current
                    current = None
                    break

            if current is not None:
                # Failed: an evicted key is left without a slot
                break

            relocations.append(kicks)
            inserted.append(phone)
            continue

        relocations.append(0)
        inserted.append(phone)

    def lookup(phone):
        p1, p2 = positions(phone)
        if table[p1] == phone or p1 == p2:
            return 1
        return 2

    hit_probes = [lookup(phone) for phone in inserted]
    miss_probes = [lookup(phone) for phone in misses]

    return {
        "inserted": len(inserted),
        "insert_failures": int(len(inserted) < len(phones)),
        "hit_probes": hit_probes or [0],
        "miss_probes": miss_probes,
        # Cuckoo positions use two hashes, so a 16-bit match in a probed
        # slot is rare; not modelled
        "hit_sd_reads": [1],
        "miss_sd_reads": [0],
        "relocations": relocations or [0],
    }


# ============================================================
# Theory (uniform hashing, Knuth)
# ============================================================

def theory(method, load, successful):

    if method == "Linear probing":
        if successful:
            return 0.5 * (1 + 1 / (1 - load))
        return 0.5 * (1 + 1 / (1 - load) ** 2)

    if successful:
        return (1 / load) * math.log(1 / (1 - load))
    return 1 / (1 - load)


# ============================================================
# Experiments
# ============================================================

def percentile(values, pct):

    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(pct / 100 * len(ordered)))]


def run_one(load, simulation):

    entries = int(HASH_TABLE_SIZE * load)
    phones = unique_phones(entries)
    misses = absent_phones(NUM_MISSES, set(phones))

    rows = []

    for method in METHODS:

        if method == "Cuckoo hashing":
            result = cuckoo(phones, misses, HASH_TABLE_SIZE)
        else:
            result = open_addressing(phones, misses, HASH_TABLE_SIZE, method)

        rows.append({
            "load_factor": load,
            "entries": entries,
            "simulation": simulation,
            "method": method,
            "inserted": result["inserted"],
            "insert_failed": result["insert_failures"],
            "hit_mean_probes": statistics.mean(result["hit_probes"]),
            "hit_p99_probes": percentile(result["hit_probes"], 99),
            "hit_max_probes": max(result["hit_probes"]),
            "miss_mean_probes": statistics.mean(result["miss_probes"]),
            "miss_max_probes": max(result["miss_probes"]),
            "hit_mean_sd_reads": statistics.mean(result["hit_sd_reads"]),
            "miss_mean_sd_reads": statistics.mean(result["miss_sd_reads"]),
            "mean_relocations": statistics.mean(result["relocations"]),
            "max_relocations": max(result["relocations"]),
        })

    return rows


def run_sweep():

    rows = []

    for load in SWEEP_LOADS:
        print(f"  load {load:.0%}")
        for simulation in range(SWEEP_SIMULATIONS):
            rows.extend(run_one(load, simulation))

    return rows


def run_design():

    rows = []

    for simulation in range(DESIGN_SIMULATIONS):
        rows.extend(run_one(DESIGN_LOAD, simulation))

    return rows


# ============================================================
# CSV output
# ============================================================

def write_csv(path, rows):

    with open(path, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"  wrote {path.name}")


def summarise(design_rows):

    summary = []

    for method in METHODS:

        rows = [r for r in design_rows if r["method"] == method]

        def mean(field):
            return round(statistics.mean(r[field] for r in rows), 4)

        summary.append({
            "method": method,
            "firmware": method == SELECTED,
            "load_factor": DESIGN_LOAD,
            "entries": rows[0]["entries"],
            "simulations": len(rows),
            "sims_all_inserted_pct": round(
                100 * sum(1 - r["insert_failed"] for r in rows) / len(rows), 1),
            "mean_inserted": mean("inserted"),
            "hit_mean_probes": mean("hit_mean_probes"),
            "hit_mean_p99_probes": mean("hit_p99_probes"),
            "hit_mean_max_probes": mean("hit_max_probes"),
            "hit_worst_probes": max(r["hit_max_probes"] for r in rows),
            "miss_mean_probes": mean("miss_mean_probes"),
            "miss_worst_probes": max(r["miss_max_probes"] for r in rows),
            "hit_mean_sd_reads": mean("hit_mean_sd_reads"),
            "miss_mean_sd_reads": mean("miss_mean_sd_reads"),
            "mean_relocations": mean("mean_relocations"),
            "worst_relocations": max(r["max_relocations"] for r in rows),
            "theory_hit_probes": (
                round(theory(method, DESIGN_LOAD, True), 4)
                if method != "Cuckoo hashing" else ""),
            "theory_miss_probes": (
                round(theory(method, DESIGN_LOAD, False), 4)
                if method != "Cuckoo hashing" else ""),
        })

    return summary


# ============================================================
# Plotting
# ============================================================

COLOURS = {
    "Double hashing": HIGHLIGHT,
    "Linear probing": LINEAR_COLOUR,
    "Cuckoo hashing": OTHER,
}


def save(fig, stem):

    for ext in ("png", "svg"):
        fig.savefig(OUTPUT_DIR / f"{stem}.{ext}")

    plt.close(fig)
    print(f"  wrote {stem}.png/.svg")


def mark_design(ax):

    design = 100 * DESIGN_CONTACTS / HASH_TABLE_SIZE
    ax.axvline(design, color=INK_SECONDARY, linewidth=0.8, linestyle=":")
    ax.text(design, ax.get_ylim()[1], f" {DESIGN_CONTACTS:,} contacts",
            va="top", fontsize=8, color=INK_SECONDARY)


def sweep_mean(rows, method, field, loads):

    return [
        statistics.mean(r[field] for r in rows
                        if r["method"] == method and r["load_factor"] == load)
        for load in loads
    ]


def label_end(ax, x, y, text, colour, bold=False, dy=0):

    ax.annotate(text, (x, y), xytext=(6, dy), textcoords="offset points",
                va="center", fontsize=8, color=colour if bold else INK,
                fontweight="bold" if bold else "normal")


def plot_probes_vs_load(rows):
    """Successful and unsuccessful lookup cost vs load, with theory."""

    loads = SWEEP_LOADS
    pct = [100 * l for l in loads]

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.6), sharey=True)

    for ax, field, successful, title in [
        (axes[0], "hit_mean_probes", True, "Successful lookup"),
        (axes[1], "miss_mean_probes", False, "Unsuccessful lookup"),
    ]:

        for method in ["Linear probing", "Double hashing"]:
            ax.plot(pct, [theory(method, l, successful) for l in loads],
                    color=COLOURS[method], linewidth=1, linestyle="--",
                    alpha=0.7)

        for method in METHODS:

            # Cuckoo is only plotted while every insert succeeds
            values = sweep_mean(rows, method, field, loads)
            ok = [all(r["insert_failed"] == 0 for r in rows
                      if r["method"] == method and r["load_factor"] == l)
                  for l in loads]
            xs = [x for x, k in zip(pct, ok) if k]
            ys = [y for y, k in zip(values, ok) if k]

            is_sel = method == SELECTED
            ax.plot(xs, ys, color=COLOURS[method],
                    linewidth=2.4 if is_sel else 1.6,
                    marker="o", markersize=4 if is_sel else 3,
                    zorder=3 if is_sel else 2, label=method)

        ax.set_ylim(0, 12)
        ax.set_xlim(0, 105)
        ax.set_xlabel("Load factor (%)")
        ax.set_title(title, loc="left")
        ax.grid(True)
        ax.set_axisbelow(True)
        mark_design(ax)

    axes[0].set_ylabel("Mean probes per lookup")
    axes[0].legend(loc="upper left", frameon=False, fontsize=8)
    axes[1].text(6, 11.4, "dashed = theory (uniform hashing)\n"
                 "cuckoo shown only while all inserts succeed",
                 fontsize=8, color=INK_SECONDARY, va="top")

    fig.tight_layout()
    save(fig, "collision_fig1_probes_vs_load")


def plot_worst_case(design_rows):
    """Distribution of the longest probe chain at 70% load."""

    methods = ["Linear probing", "Double hashing"]

    fig, axes = plt.subplots(1, 2, figsize=(11, 3.8))

    for ax, field, title in [
        (axes[0], "hit_max_probes", "Longest successful lookup"),
        (axes[1], "miss_max_probes", "Longest unsuccessful lookup"),
    ]:

        data = [[r[field] for r in design_rows if r["method"] == m]
                for m in methods]

        box = ax.boxplot(
            data, vert=False, tick_labels=methods, widths=0.5,
            patch_artist=True,
            medianprops={"color": INK, "linewidth": 1.5},
            whiskerprops={"color": INK_SECONDARY},
            capprops={"color": INK_SECONDARY},
            flierprops={"marker": "o", "markersize": 3,
                        "markerfacecolor": INK_SECONDARY,
                        "markeredgecolor": "none"},
        )

        for patch, method in zip(box["boxes"], methods):
            patch.set_facecolor(COLOURS[method])
            patch.set_edgecolor(INK_SECONDARY)

        for i, values in enumerate(data, start=1):
            ax.text(max(values) + 4, i,
                    f"median {statistics.median(values):.0f}, "
                    f"max {max(values)}",
                    va="center", fontsize=8, color=INK_SECONDARY)

        ax.set_xlim(0, max(max(v) for v in data) * 1.35)
        ax.set_xlabel("Probes")
        ax.set_title(title, loc="left")
        ax.grid(axis="x")
        ax.set_axisbelow(True)

    fig.suptitle(
        f"Worst case per simulation at {DESIGN_LOAD:.0%} load "
        f"({DESIGN_SIMULATIONS} simulations)",
        x=0.01, ha="left", fontsize=11, color=INK_SECONDARY
    )
    fig.tight_layout()
    save(fig, "collision_fig2_worst_case")


def plot_cuckoo_capacity(rows):
    """Share of simulations where cuckoo hashing stored every number."""

    loads = SWEEP_LOADS
    pct = [100 * l for l in loads]

    success = [
        100 * statistics.mean(1 - r["insert_failed"] for r in rows
                              if r["method"] == "Cuckoo hashing"
                              and r["load_factor"] == l)
        for l in loads
    ]
    relocations = sweep_mean(rows, "Cuckoo hashing", "mean_relocations", loads)

    fig, axes = plt.subplots(1, 2, figsize=(12, 4))

    ax = axes[0]
    ax.plot(pct, success, color=INK_SECONDARY, linewidth=1.8,
            marker="o", markersize=4)
    ax.set_ylim(-5, 105)
    ax.set_xlabel("Load factor (%)")
    ax.set_ylabel("Simulations with all numbers stored (%)")
    ax.set_title("Cuckoo hashing: inserts without failure", loc="left")
    ax.grid(True)
    ax.set_axisbelow(True)
    mark_design(ax)

    ax = axes[1]
    ok = [s == 100 for s in success]
    ax.plot([x for x, k in zip(pct, ok) if k],
            [y for y, k in zip(relocations, ok) if k],
            color=INK_SECONDARY, linewidth=1.8, marker="o", markersize=4)
    ax.set_xlim(0, 95)
    ax.set_xlabel("Load factor (%)")
    ax.set_ylabel("Mean relocations per insert")
    ax.set_title("Cuckoo hashing: relocations (while all inserts succeed)",
                 loc="left")
    ax.grid(True)
    ax.set_axisbelow(True)
    mark_design(ax)

    fig.tight_layout()
    save(fig, "collision_fig3_cuckoo_capacity")


def plot_selection(design_rows):
    """Simple summary for the design section: linear vs double hashing."""

    fig, ax = plt.subplots(figsize=(6.5, 3.8))

    methods = ["Linear probing", "Double hashing"]
    groups = [("Mean lookup", "hit_mean_probes"),
              ("Longest lookup", "hit_max_probes")]
    width = 0.36

    for i, method in enumerate(methods):
        rows = [r for r in design_rows if r["method"] == method]
        values = [statistics.mean(r[field] for r in rows)
                  for _, field in groups]
        xs = [g + (i - 0.5) * width for g in range(len(groups))]
        bars = ax.bar(xs, values, width=width, color=COLOURS[method],
                      edgecolor="white", linewidth=2, label=method)
        for bar, value in zip(bars, values):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                    f"{value:.1f}", ha="center", va="bottom", fontsize=8,
                    color=INK_SECONDARY)

    ax.set_xticks(range(len(groups)), [g for g, _ in groups])
    ax.set_ylabel("Probes per lookup")
    ax.set_title(f"Lookup cost at {DESIGN_CONTACTS:,} contacts "
                 f"({DESIGN_LOAD:.0%} load)", loc="left")
    ax.legend(loc="upper left", frameon=False, fontsize=8)
    ax.grid(axis="y")
    ax.set_axisbelow(True)

    fig.tight_layout()
    save(fig, "collision_fig0_selection")


# ============================================================
# Main
# ============================================================

def main():

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    random.seed(RANDOM_SEED)

    print(f"Load sweep ({SWEEP_SIMULATIONS} sims per load)...")
    sweep_rows = run_sweep()

    print(f"Design load ({DESIGN_SIMULATIONS} sims at {DESIGN_LOAD:.0%})...")
    design_rows = run_design()

    print("Writing CSV...")
    summary = summarise(design_rows)
    write_csv(OUTPUT_DIR / "collision_summary.csv", summary)
    write_csv(OUTPUT_DIR / "collision_70pct_load.csv", design_rows)
    write_csv(OUTPUT_DIR / "collision_load_sweep.csv", sweep_rows)

    print("Plotting...")
    plot_selection(design_rows)
    plot_probes_vs_load(sweep_rows)
    plot_worst_case(design_rows)
    plot_cuckoo_capacity(sweep_rows)

    print()
    for row in summary:
        print(row)


if __name__ == "__main__":
    main()
