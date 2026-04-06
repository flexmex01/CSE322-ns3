#!/usr/bin/env python3
"""
Plot rLEDBAT metrics comparing independent runs (CADF only, ECS only, ATD only) vs Base
Single figure with 3 subplots (WLedbat, Queuing Delay, Base Delay)
Each subplot shows 4 lines: Base, CADF Only, ECS Only, ATD Only
"""

import os

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# Consistent figure size
FIGSIZE = (18, 14)

# Base scenario tag (nodes=60, flows=30, pps=300, speed=15)
BASE_TAG = "pass_base_nodes_60"

# Variants to plot
# Format: (tag, display_name, color, linestyle)
variants = [
    (BASE_TAG, "Base rLEDBAT", "blue", "-"),
    ("run_cadf_only", "CADF Only", "green", "--"),
    ("run_ecs_only", "ECS Only", "orange", "-."),
    ("run_atd_only", "ATD Only", "red", ":"),
]


def read_dat_file(filename):
    """Read a .dat file and return DataFrame with time and value columns"""
    if not os.path.exists(filename):
        print(f"Warning: {filename} not found")
        return None

    try:
        # Read the file, skipping comment lines
        df = pd.read_csv(
            filename,
            sep=r"\s+",
            comment="#",
            names=["time", "value"],
            header=None,
        )
        return df
    except Exception as e:
        print(f"Error reading {filename}: {e}")
        return None


def plot_independent_comparison():
    """
    Create a single figure with 3 subplots comparing all variants
    """
    fig, axes = plt.subplots(3, 1, figsize=FIGSIZE)
    fig.suptitle(
        "rLEDBAT Modification Comparison (Base vs CADF vs ECS vs ATD)",
        fontsize=18,
        fontweight="bold",
    )

    # Metric configurations
    metrics = [
        ("WLedbat (Congestion Window)", "final-wledbat-", "WLedbat (bytes)"),
        ("Queuing Delay", "final-qdelay-", "Queuing Delay (ms)"),
        ("Base Delay", "final-basedelay-", "Base Delay (ms)"),
    ]

    for idx, (metric_name, file_prefix, ylabel) in enumerate(metrics):
        ax = axes[idx]

        # Plot each variant
        for tag, display_name, color, linestyle in variants:
            filename = f"{file_prefix}{tag}.dat"
            df = read_dat_file(filename)

            if df is not None and len(df) > 0:
                ax.plot(
                    df["time"],
                    df["value"],
                    linestyle=linestyle,
                    linewidth=2.5,
                    color=color,
                    alpha=0.8,
                    label=display_name,
                )

        ax.set_xlabel("Time (s)", fontsize=13, fontweight="bold")
        ax.set_ylabel(ylabel, fontsize=13)
        ax.set_title(metric_name, fontsize=14, fontweight="bold")
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=11, loc="best")
        ax.tick_params(labelsize=11)

    plt.tight_layout()
    output_file = "graph_independent_comparison.png"
    plt.savefig(output_file, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_file}")
    plt.close()


# Generate the comparison graph
plot_independent_comparison()

print("\nIndependent modification comparison graph generated successfully!")
print("File created:")
print("  - graph_independent_comparison.png")
print("\nComparison includes:")
print("  - Base rLEDBAT (nodes=60)")
print("  - CADF Only modification")
print("  - ECS Only modification")
print("  - ATD Only modification")
