#!/usr/bin/env python3
"""
Hash function selection evidence.

Re-runs the full_hash_eval.py comparison and adds:
  - DJB2 16-bit: exact model of the firmware hash_phone()
  - paired datasets (every hash sees the same phone numbers)
  - ideal uniform-hash reference for every metric
  - load-factor sweep including firmware double-hashing probe counts
  - CSV output of all raw and summary data

Outputs are written to results/hash_selection/.
"""

import csv
import math
import random
import statistics
import time
from collections import Counter
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import Patch

from full_hash_eval import (
    HASH_TABLE_SIZE,
    NUM_SAMPLES,
    NUM_SIMULATIONS,
    LOAD_FACTOR,
    RANDOM_SEED,
    fnv1a,
    fnv1a_folded,
    djb2,
    sdbm,
    murmur_finalizer,
    crc32_hash,
    generate_phone_numbers,
    calculate_distribution_statistics,
)


# ============================================================
# Configuration
# ============================================================

OUTPUT_DIR = Path(__file__).parent / "results" / "hash_selection"

# Load factor sweep
SWEEP_LOADS = [0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90]
SWEEP_SIMULATIONS = 20

# Design max contacts (see contact capacity)
DESIGN_CONTACTS = 10_000

SELECTED = "DJB2 16-bit"


# ============================================================
# Firmware hash model
# ============================================================

def djb2_folded(phone: str) -> int:
    """
    Python equivalent of firmware hash_phone():

        uint32_t hash = 5381u;
        hash = ((hash << 5) + hash) + (uint8_t)*phone;
        return (uint16_t)hash ^ (hash >> 16);
    """

    hash_value = djb2(phone)

    return ((hash_value & 0xFFFF) ^ (hash_value >> 16)) & 0xFFFF


HASH_FUNCTIONS = {
    "DJB2 16-bit": djb2_folded,
    "DJB2": djb2,
    "FNV-1a 16-bit": fnv1a_folded,
    "FNV-1a": fnv1a,
    "SDBM": sdbm,
    "Murmur finalizer": murmur_finalizer,
    "CRC32": crc32_hash,
}


# ============================================================
# Ideal (uniform random hash) references
# ============================================================

def ideal_collisions(n, m):
    """Expected collisions when n keys are thrown uniformly into m buckets."""

    return n - m * (1 - (1 - 1 / m) ** n)


def poisson_pmf(k, lam):

    return math.exp(-lam) * lam ** k / math.factorial(k)


# ============================================================
# Firmware double hashing model (hash_table.c)
# ============================================================

def double_hash_probes(keys, capacity):
    """
    Insert 16-bit keys using the firmware probe sequence:

        h1 = key % capacity
        h2 = 1 + key % (capacity - 1)
        slot = (h1 + attempt * h2) % capacity

    Returns the probe count (slots examined) for each insert.
    """

    occupied = bytearray(capacity)
    probes = []

    for key in keys:

        key &= 0xFFFF
        h1 = key % capacity
        h2 = 1 + key % (capacity - 1)

        attempt = 0
        slot = h1

        while occupied[slot]:
            attempt += 1
            slot = (h1 + attempt * h2) % capacity

        occupied[slot] = 1
        probes.append(attempt + 1)

    return probes


def uniform_keys(count, rng):
    """16-bit keys from an ideal uniform hash."""

    return [rng.getrandbits(16) for _ in range(count)]


# ============================================================
# Experiments
# ============================================================

def run_distribution(phone_numbers):

    results = {}

    for name, function in HASH_FUNCTIONS.items():

        start = time.perf_counter()
        hash_values = [function(phone) for phone in phone_numbers]
        elapsed = time.perf_counter() - start

        stats = calculate_distribution_statistics(hash_values, HASH_TABLE_SIZE)
        stats["hash_time_us_per_key"] = elapsed / len(phone_numbers) * 1e6
        stats["unique_hashes"] = len(set(hash_values))
        results[name] = stats

    return results


def run_collision_simulations():
    """Paired: each simulation's dataset is hashed by every function."""

    num_entries = int(HASH_TABLE_SIZE * LOAD_FACTOR)
    rows = []

    for simulation in range(NUM_SIMULATIONS):

        phones = generate_phone_numbers(num_entries)

        for name, function in HASH_FUNCTIONS.items():

            stats = calculate_distribution_statistics(
                [function(phone) for phone in phones],
                HASH_TABLE_SIZE
            )

            rows.append({
                "simulation": simulation,
                "hash_function": name,
                "entries": num_entries,
                "collisions": stats["collisions"],
                "max_bucket_depth": stats["maximum_bucket_depth"],
            })

    return rows


def run_load_sweep(rng):

    rows = []

    for load in SWEEP_LOADS:

        num_entries = int(HASH_TABLE_SIZE * load)
        print(f"  load {load:.0%} ({num_entries} entries)")

        for simulation in range(SWEEP_SIMULATIONS):

            phones = generate_phone_numbers(num_entries)

            candidates = {
                name: [function(phone) for phone in phones]
                for name, function in HASH_FUNCTIONS.items()
            }
            candidates["Ideal uniform"] = uniform_keys(num_entries, rng)

            for name, hash_values in candidates.items():

                stats = calculate_distribution_statistics(
                    hash_values,
                    HASH_TABLE_SIZE
                )

                probes = double_hash_probes(hash_values, HASH_TABLE_SIZE)

                rows.append({
                    "load_factor": load,
                    "entries": num_entries,
                    "simulation": simulation,
                    "hash_function": name,
                    "collisions": stats["collisions"],
                    "collision_rate": stats["collision_rate"],
                    "max_bucket_depth": stats["maximum_bucket_depth"],
                    "mean_probes": statistics.mean(probes),
                    "max_probes": max(probes),
                })

    return rows


# ============================================================
# CSV output
# ============================================================

def write_csv(path, rows):

    with open(path, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"  wrote {path.relative_to(Path(__file__).parent)}")


def summarise(distribution, collision_rows, sweep_rows):

    num_entries = int(HASH_TABLE_SIZE * LOAD_FACTOR)
    design_load = min(SWEEP_LOADS, key=lambda l: abs(l - LOAD_FACTOR))

    summary = []

    for name, stats in distribution.items():

        sims = [r["collisions"] for r in collision_rows
                if r["hash_function"] == name]
        sweep = [r for r in sweep_rows
                 if r["hash_function"] == name
                 and r["load_factor"] == design_load]

        summary.append({
            "hash_function": name,
            "firmware": name == SELECTED,
            "n100k_unique_hashes": stats["unique_hashes"],
            "n100k_collisions": stats["collisions"],
            "n100k_collision_rate_pct": round(stats["collision_rate"] * 100, 4),
            "n100k_max_bucket_depth": stats["maximum_bucket_depth"],
            "n100k_bucket_std_dev": round(stats["std_deviation"], 4),
            "n100k_chi_squared": round(stats["chi_squared"], 2),
            "n100k_chi_squared_per_dof": round(
                stats["chi_squared"] / (HASH_TABLE_SIZE - 1), 4),
            "load70_mean_collisions": round(statistics.mean(sims), 2),
            "load70_std_collisions": round(statistics.stdev(sims), 2),
            "load70_min_collisions": min(sims),
            "load70_max_collisions": max(sims),
            "load70_ideal_collisions": round(
                ideal_collisions(num_entries, HASH_TABLE_SIZE), 2),
            "load70_mean_probes": round(
                statistics.mean(r["mean_probes"] for r in sweep), 4),
            "load70_worst_probes": max(r["max_probes"] for r in sweep),
            "python_time_us_per_key": round(stats["hash_time_us_per_key"], 4),
        })

    return summary


def occupancy_rows(distribution):

    lam = NUM_SAMPLES / HASH_TABLE_SIZE
    rows = []

    max_depth = max(s["maximum_bucket_depth"] for s in distribution.values())

    for depth in range(max_depth + 1):

        row = {
            "entries_in_bucket": depth,
            "ideal_poisson": round(HASH_TABLE_SIZE * poisson_pmf(depth, lam), 3),
        }

        for name, stats in distribution.items():
            row[name] = Counter(stats["bucket_counts"]).get(depth, 0)

        rows.append(row)

    return rows


# ============================================================
# Plotting
# ============================================================

HIGHLIGHT = "#2a78d6"
OTHER = "#b4b3ad"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#e4e3df"

plt.rcParams.update({
    "font.size": 10,
    "axes.edgecolor": INK_SECONDARY,
    "axes.labelcolor": INK,
    "axes.titlesize": 11,
    "axes.titleweight": "bold",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "xtick.color": INK_SECONDARY,
    "ytick.color": INK_SECONDARY,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "figure.dpi": 110,
    "savefig.dpi": 300,
    "savefig.bbox": "tight",
})


def colour(name):

    return HIGHLIGHT if name == SELECTED else OTHER


def save(fig, stem):

    for ext in ("png", "svg"):
        fig.savefig(OUTPUT_DIR / f"{stem}.{ext}")

    plt.close(fig)
    print(f"  wrote {stem}.png/.svg")


def plot_collisions_70(collision_rows):
    """Per-simulation collisions at 70% load, sorted by mean."""

    num_entries = int(HASH_TABLE_SIZE * LOAD_FACTOR)
    ideal = ideal_collisions(num_entries, HASH_TABLE_SIZE)

    data = {
        name: [r["collisions"] for r in collision_rows
               if r["hash_function"] == name]
        for name in HASH_FUNCTIONS
    }
    names = sorted(data, key=lambda n: statistics.mean(data[n]), reverse=True)

    fig, ax = plt.subplots(figsize=(8, 4.5))

    box = ax.boxplot(
        [data[n] for n in names],
        vert=False,
        tick_labels=names,
        widths=0.55,
        patch_artist=True,
        showfliers=True,
        medianprops={"color": INK, "linewidth": 1.5},
        whiskerprops={"color": INK_SECONDARY},
        capprops={"color": INK_SECONDARY},
        flierprops={"marker": "o", "markersize": 3,
                    "markerfacecolor": INK_SECONDARY,
                    "markeredgecolor": "none"},
    )

    for patch, name in zip(box["boxes"], names):
        patch.set_facecolor(colour(name))
        patch.set_edgecolor(INK_SECONDARY)

    ax.axvline(ideal, color=INK, linestyle="--", linewidth=1)
    ax.text(ideal, len(names) + 0.55, f"ideal uniform hash ≈ {ideal:,.0f}",
            ha="center", va="bottom", color=INK, fontsize=9)

    ax.set_xlabel("Home-bucket collisions")
    ax.set_title(
        f"Collisions at {LOAD_FACTOR:.0%} load "
        f"({num_entries:,} numbers, {NUM_SIMULATIONS} paired simulations)",
        loc="left", pad=18
    )
    ax.grid(axis="x")
    ax.set_axisbelow(True)

    for label in ax.get_yticklabels():
        if label.get_text() == SELECTED:
            label.set_fontweight("bold")
            label.set_color(INK)

    save(fig, "fig1_collisions_70pct_load")


def plot_load_sweep(sweep_rows):
    """Collisions relative to ideal, and firmware probe counts, vs load."""

    names = list(HASH_FUNCTIONS) + ["Ideal uniform"]

    def mean_by_load(name, field):
        return [
            statistics.mean(
                r[field] for r in sweep_rows
                if r["hash_function"] == name and r["load_factor"] == load
            )
            for load in SWEEP_LOADS
        ]

    ideal = mean_by_load("Ideal uniform", "collisions")
    loads_pct = [l * 100 for l in SWEEP_LOADS]

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.6))

    # --- Collisions as % of ideal ----------------------------------------
    ax = axes[0]

    for name in names:
        if name == "Ideal uniform":
            continue
        values = mean_by_load(name, "collisions")
        ratio = [100 * v / i for v, i in zip(values, ideal)]
        is_sel = name == SELECTED
        ax.plot(loads_pct, ratio, color=colour(name),
                linewidth=2.4 if is_sel else 1.3,
                marker="o", markersize=5 if is_sel else 3,
                zorder=3 if is_sel else 2)
        if is_sel:
            ax.annotate(name, (loads_pct[-1], ratio[-1]),
                        xytext=(6, 0), textcoords="offset points",
                        va="center", fontsize=8, color=INK,
                        fontweight="bold")

    ax.axhline(100, color=INK, linestyle="--", linewidth=1)
    ax.text(loads_pct[0], 100, "ideal uniform 16-bit hash", va="bottom",
            fontsize=8, color=INK)
    ax.set_xlabel("Load factor (%)")
    ax.set_ylabel("Collisions relative to ideal (%)")
    ax.set_title("Collisions vs ideal (grey = alternative hashes)",
                 loc="left")
    ax.grid(True)
    ax.set_axisbelow(True)

    # --- Mean probes (firmware double hashing) ---------------------------
    ax = axes[1]

    for name in names:
        probes = mean_by_load(name, "mean_probes")
        is_sel = name == SELECTED
        is_ideal = name == "Ideal uniform"
        ax.plot(loads_pct, probes,
                color=INK if is_ideal else colour(name),
                linestyle="--" if is_ideal else "-",
                linewidth=2.4 if is_sel else (1 if is_ideal else 1.3),
                marker=None if is_ideal else "o",
                markersize=5 if is_sel else 3,
                zorder=3 if is_sel else 2)

    design_load = 100 * DESIGN_CONTACTS / HASH_TABLE_SIZE
    ax.axvline(design_load, color=INK_SECONDARY, linewidth=0.8, linestyle=":")
    ax.text(design_load, ax.get_ylim()[1], f" {DESIGN_CONTACTS:,} contacts",
            va="top", fontsize=8, color=INK_SECONDARY)

    ax.annotate("ideal", (loads_pct[-1], probes[-1]),
                xytext=(6, -10), textcoords="offset points",
                fontsize=8, color=INK)

    sdbm_probes = mean_by_load("SDBM", "mean_probes")
    ax.annotate("SDBM", (loads_pct[-2], sdbm_probes[-2]),
                xytext=(-8, 4), textcoords="offset points", ha="right",
                fontsize=8, color=INK_SECONDARY)

    sel = mean_by_load(SELECTED, "mean_probes")
    ax.annotate(SELECTED, (loads_pct[-1], sel[-1]),
                xytext=(6, 0), textcoords="offset points",
                va="center", fontsize=8, color=INK, fontweight="bold")

    ax.set_xlabel("Load factor (%)")
    ax.set_ylabel("Mean probes per insert")
    ax.set_title("Firmware double hashing: mean probes per insert",
                 loc="left")
    ax.grid(True)
    ax.set_axisbelow(True)

    fig.tight_layout()
    save(fig, "fig2_load_factor_sweep")


def plot_uniformity(distribution):
    """χ²/dof and max bucket depth at N = 100k."""

    lam = NUM_SAMPLES / HASH_TABLE_SIZE

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.2))

    metrics = [
        ("chi_squared", "χ² / degrees of freedom",
         "Uniformity (χ² / dof, 1.0 = ideal)", 1.0, "ideal = 1.0",
         lambda s: s["chi_squared"] / (HASH_TABLE_SIZE - 1)),
        ("maximum_bucket_depth", "Entries in deepest bucket",
         "Deepest bucket", None, None,
         lambda s: s["maximum_bucket_depth"]),
    ]

    for ax, (key, xlabel, title, ref, ref_label, fn) in zip(axes, metrics):

        names = sorted(distribution, key=lambda n: fn(distribution[n]),
                       reverse=True)
        values = [fn(distribution[n]) for n in names]

        bars = ax.barh(names, values, color=[colour(n) for n in names],
                       height=0.6, edgecolor="white", linewidth=2)

        for bar, value in zip(bars, values):
            ax.text(bar.get_width(), bar.get_y() + bar.get_height() / 2,
                    f" {value:.2f}" if isinstance(value, float)
                    else f" {value}",
                    va="center", fontsize=8, color=INK_SECONDARY)

        if ref is not None:
            ax.axvline(ref, color=INK, linestyle="--", linewidth=1)

        if key == "chi_squared":
            ax.set_xlim(0.9, max(values) * 1.03)
        ax.set_xlabel(xlabel)
        ax.set_title(title, loc="left")
        ax.grid(axis="x")
        ax.set_axisbelow(True)

        for label in ax.get_yticklabels():
            if label.get_text() == SELECTED:
                label.set_fontweight("bold")
                label.set_color(INK)

    fig.suptitle(
        f"Bucket distribution of {NUM_SAMPLES:,} phone numbers "
        f"into {HASH_TABLE_SIZE:,} buckets (mean {lam:.2f} per bucket)",
        x=0.01, ha="left", fontsize=11, color=INK_SECONDARY
    )
    fig.tight_layout()
    save(fig, "fig3_uniformity")


def plot_occupancy(occupancy, distribution):
    """Small multiples: bucket occupancy histogram vs Poisson ideal."""

    names = list(HASH_FUNCTIONS)
    depths = [r["entries_in_bucket"] for r in occupancy]
    ideal = [r["ideal_poisson"] for r in occupancy]
    x_max = 20

    fig, axes = plt.subplots(2, 4, figsize=(14, 6), sharex=True, sharey=True)
    axes = axes.flatten()

    for ax, name in zip(axes, names):

        counts = [r[name] for r in occupancy]

        ax.bar(depths, counts, width=0.8, color=colour(name),
               edgecolor="white", linewidth=1)
        ax.plot(depths, ideal, color=INK, linewidth=1.2,
                marker="o", markersize=2.5)

        chi = distribution[name]["chi_squared"] / (HASH_TABLE_SIZE - 1)
        ax.set_title(f"{name}\nχ²/dof = {chi:.2f}", loc="left",
                     color=INK, fontsize=10,
                     fontweight="bold" if name == SELECTED else "normal")
        ax.set_xlim(-0.6, x_max)
        ax.grid(axis="y")
        ax.set_axisbelow(True)

    legend_ax = axes[-1]
    legend_ax.axis("off")
    legend_ax.plot([], [], color=INK, marker="o", markersize=3,
                   label="Ideal (Poisson)")
    handles, _ = legend_ax.get_legend_handles_labels()
    handles += [
        Patch(color=HIGHLIGHT, label="Firmware hash (DJB2 16-bit)"),
        Patch(color=OTHER, label="Alternative hash"),
    ]
    legend_ax.legend(handles=handles, loc="center", frameon=False)

    for ax in axes[4:7]:
        ax.set_xlabel("Entries in bucket")
    axes[0].set_ylabel("Number of buckets")
    axes[4].set_ylabel("Number of buckets")

    fig.suptitle(
        f"Bucket occupancy, {NUM_SAMPLES:,} phone numbers "
        f"(bars clipped at {x_max})",
        x=0.01, ha="left", fontsize=11, color=INK_SECONDARY
    )
    fig.tight_layout()
    save(fig, "fig4_bucket_occupancy")


def plot_probes_design_load(sweep_rows):
    """Simple summary: mean probes per insert at the design load."""

    load = min(SWEEP_LOADS, key=lambda l: abs(l - LOAD_FACTOR))
    names = list(HASH_FUNCTIONS)

    means = {
        name: statistics.mean(
            r["mean_probes"] for r in sweep_rows
            if r["hash_function"] == name and r["load_factor"] == load
        )
        for name in names + ["Ideal uniform"]
    }
    names = sorted(names, key=lambda n: means[n], reverse=True)
    ideal = means["Ideal uniform"]

    fig, ax = plt.subplots(figsize=(7, 3.6))

    bars = ax.barh(names, [means[n] for n in names],
                   color=[colour(n) for n in names],
                   height=0.6, edgecolor="white", linewidth=2)

    for bar, name in zip(bars, names):
        ax.text(bar.get_width(), bar.get_y() + bar.get_height() / 2,
                f" {means[name]:.2f}", va="center", fontsize=8,
                color=INK_SECONDARY)

    ax.axvline(ideal, color=INK, linestyle="--", linewidth=1)
    ax.text(ideal, len(names) - 0.4, f" ideal = {ideal:.2f}",
            va="bottom", fontsize=8, color=INK)

    ax.set_xlim(0, 2.1)
    ax.set_xlabel("Mean probes per insert")
    ax.set_title(
        f"Mean probes per insert at {load:.0%} load "
        f"({int(HASH_TABLE_SIZE * load):,} contacts)",
        loc="left", pad=14
    )
    ax.grid(axis="x")
    ax.set_axisbelow(True)

    for label in ax.get_yticklabels():
        if label.get_text() == SELECTED:
            label.set_fontweight("bold")
            label.set_color(INK)

    save(fig, "fig5_probes_design_load")


# ============================================================
# Main
# ============================================================

def main():

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    random.seed(RANDOM_SEED)
    rng = random.Random(RANDOM_SEED + 1)

    print(f"Table size {HASH_TABLE_SIZE:,}, samples {NUM_SAMPLES:,}, "
          f"seed {RANDOM_SEED}")

    print("Distribution test...")
    phone_numbers = generate_phone_numbers(NUM_SAMPLES)
    distribution = run_distribution(phone_numbers)

    print(f"Collision simulations ({NUM_SIMULATIONS} at {LOAD_FACTOR:.0%})...")
    collision_rows = run_collision_simulations()

    print(f"Load sweep ({SWEEP_SIMULATIONS} sims per load)...")
    sweep_rows = run_load_sweep(rng)

    print("Writing CSV...")
    summary = summarise(distribution, collision_rows, sweep_rows)
    occupancy = occupancy_rows(distribution)

    write_csv(OUTPUT_DIR / "summary.csv", summary)
    write_csv(OUTPUT_DIR / "collisions_70pct_load.csv", collision_rows)
    write_csv(OUTPUT_DIR / "load_factor_sweep.csv", sweep_rows)
    write_csv(OUTPUT_DIR / "bucket_occupancy.csv", occupancy)
    write_csv(OUTPUT_DIR / "bucket_counts_100k.csv", [
        {"bucket": b, **{n: distribution[n]["bucket_counts"][b]
                         for n in HASH_FUNCTIONS}}
        for b in range(HASH_TABLE_SIZE)
    ])

    print("Plotting...")
    plot_collisions_70(collision_rows)
    plot_load_sweep(sweep_rows)
    plot_uniformity(distribution)
    plot_occupancy(occupancy, distribution)
    plot_probes_design_load(sweep_rows)

    print()
    print(f"{'Hash':<18}{'Coll@100k':>11}{'χ²/dof':>9}{'MaxB':>6}"
          f"{'Coll@70%':>10}{'Probes@70%':>12}")
    for row in sorted(summary, key=lambda r: r["load70_mean_collisions"]):
        print(f"{row['hash_function']:<18}{row['n100k_collisions']:>11,}"
              f"{row['n100k_chi_squared_per_dof']:>9.3f}"
              f"{row['n100k_max_bucket_depth']:>6}"
              f"{row['load70_mean_collisions']:>10.1f}"
              f"{row['load70_mean_probes']:>12.3f}")
    print(f"{'Ideal (70%)':<18}{'':>11}{'1.000':>9}{'':>6}"
          f"{summary[0]['load70_ideal_collisions']:>10.1f}")


if __name__ == "__main__":
    main()
