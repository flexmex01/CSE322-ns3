#!/usr/bin/env python3
"""
Plot rLEDBAT metrics for independent runs (CADF only, ECS only, ATD only)
Each independent run gets one figure with 3 subplots (WLedbat, Queuing Delay, Base Delay)
"""

import os

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# Consistent figure size
FIGSIZE = (18, 14)

# Independent runs to plot
# Format: (sweep_type, tag, display_name)
independent_runs = [
    ("cadf_only", "run_cadf_only", "CADF Only"),
    ("ecs_only", "run_ecs_only", "ECS Only"),
    ("atd_only", "run_atd_only", "ATD Only"),
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
            delim_whitespace=True,
            comment="#",
            names=["time", "value"],
            header=None,
        )
        return df
    except Exception as e:
        print(f"Error reading {filename}: {e}")
        return None


def plot_independent_run(sweep_type, tag, display_name, output_filename):
    """
    Create a figure with 3 subplots for one independent run

    sweep_type: e.g., 'cadf_only', 'ecs_only', 'atd_only'
    tag: tag used in filename, e.g., 'run_cadf_only'
    display_name: display name for title, e.g., 'CADF Only'
    output_filename: output PNG filename
    """
    fig, axes = plt.subplots(3, 1, figsize=FIGSIZE)
    fig.suptitle(f"rLEDBAT Metrics - {display_name}", fontsize=18, fontweight="bold")

    # Metric configurations
    metrics = [
        ("WLedbat (Congestion Window)", "final-wledbat-", "WLedbat (bytes)"),
        ("Queuing Delay", "final-qdelay-", "Queuing Delay (ms)"),
        ("Base Delay", "final-basedelay-", "Base Delay (ms)"),
    ]

    for idx, (metric_name, file_prefix, ylabel) in enumerate(metrics):
        ax = axes[idx]

        # Construct filename
        filename = f"{file_prefix}{tag}.dat"

        # Read data
        df = read_dat_file("dat/" + filename)

        if df is not None and len(df) > 0:
            ax.plot(
                df["time"],
                df["value"],
                linestyle="-",
                linewidth=2.5,
                color="purple",
                alpha=0.8,
                label=display_name,
            )

            ax.set_xlabel("Time (s)", fontsize=13, fontweight="bold")
            ax.set_ylabel(ylabel, fontsize=13)
            ax.set_title(metric_name, fontsize=14, fontweight="bold")
            ax.grid(True, alpha=0.3)
            ax.legend(fontsize=11, loc="best")
            ax.tick_params(labelsize=11)
        else:
            # No data available
            ax.text(
                0.5,
                0.5,
                f"No data available for {metric_name}",
                ha="center",
                va="center",
                fontsize=12,
                transform=ax.transAxes,
            )
            ax.set_xlabel("Time (s)", fontsize=13, fontweight="bold")
            ax.set_ylabel(ylabel, fontsize=13)
            ax.set_title(metric_name, fontsize=14, fontweight="bold")
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(output_filename, dpi=300, bbox_inches="tight")
    print(f"Saved: {output_filename}")
    plt.close()


# Generate graphs for each independent run
for sweep_type, tag, display_name in independent_runs:
    output_file = f"graph_independent_{sweep_type}.png"
    plot_independent_run(sweep_type, tag, display_name, output_file)

print("\nAll independent run graphs generated successfully!")
print("Files created:")
for sweep_type, tag, display_name in independent_runs:
    print(f"  - graph_independent_{sweep_type}.png ({display_name})")
