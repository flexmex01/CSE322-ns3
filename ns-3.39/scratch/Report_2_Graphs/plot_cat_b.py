#!/usr/bin/env python3
"""
Plot Category B metrics: rLEDBAT-Specific Time-Series Metrics
- WLedbat (Congestion Window) Over Time
- Queuing Delay Over Time
- Base Delay Over Time

For representative scenarios, comparing base vs modified
"""

import glob
import os

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# Consistent figure size
FIGSIZE = (18, 14)

# Representative scenarios to plot (pick meaningful test points)
# Format: (sweep_type, sweep_value, description)
representative_scenarios = [
    ("nodes", 60, "nodes=60"),
    ("flows", 30, "flows=30"),
    ("pps", 300, "pps=300"),
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


def plot_time_series_comparison(metric_name, metric_file_prefix, ylabel, filename):
    """
    Create time-series plots comparing base vs modified for representative scenarios

    metric_name: e.g., 'WLedbat', 'Queuing Delay', 'Base Delay'
    metric_file_prefix: e.g., 'final-wledbat-', 'final-qdelay-', 'final-basedelay-'
    ylabel: Y-axis label
    filename: Output filename
    """
    num_scenarios = len(representative_scenarios)

    # Create subplots - one per scenario
    fig, axes = plt.subplots(num_scenarios, 1, figsize=FIGSIZE)
    if num_scenarios == 1:
        axes = [axes]

    fig.suptitle(
        f"{metric_name} Over Time (Base vs Modified)", fontsize=18, fontweight="bold"
    )

    for idx, (sweep_type, sweep_value, description) in enumerate(
        representative_scenarios
    ):
        ax = axes[idx]

        # Construct filenames for base and modified
        tag_base = f"pass_base_{sweep_type}_{int(sweep_value) if isinstance(sweep_value, (int, float)) else sweep_value}"
        tag_modified = f"pass_modified_{sweep_type}_{int(sweep_value) if isinstance(sweep_value, (int, float)) else sweep_value}"

        file_base = f"{metric_file_prefix}{tag_base}.dat"
        file_modified = f"{metric_file_prefix}{tag_modified}.dat"

        # Read data
        df_base = read_dat_file("dat/" + file_base)
        df_modified = read_dat_file("dat/" + file_modified)

        # Plot base
        if df_base is not None and len(df_base) > 0:
            ax.plot(
                df_base["time"],
                df_base["value"],
                linestyle="-",
                linewidth=2,
                color="blue",
                label="Base rLEDBAT",
                alpha=0.8,
            )

        # Plot modified
        if df_modified is not None and len(df_modified) > 0:
            ax.plot(
                df_modified["time"],
                df_modified["value"],
                linestyle="--",
                linewidth=2,
                color="green",
                label="Modified rLEDBAT (CADF+ECS+ATD)",
                alpha=0.8,
            )

        ax.set_xlabel("Time (s)", fontsize=13, fontweight="bold")
        ax.set_ylabel(ylabel, fontsize=13)
        ax.set_title(f"{metric_name} - {description}", fontsize=14, fontweight="bold")
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=11, loc="best")
        ax.tick_params(labelsize=11)

    plt.tight_layout()
    plt.savefig(filename, dpi=300, bbox_inches="tight")
    print(f"Saved: {filename}")
    plt.close()


# 1. WLedbat (Congestion Window) Over Time
plot_time_series_comparison(
    "WLedbat (Congestion Window)",
    "final-wledbat-",
    "WLedbat (bytes)",
    "graph_b_wledbat_timeseries.png",
)

# 2. Queuing Delay Over Time
plot_time_series_comparison(
    "Queuing Delay",
    "final-qdelay-",
    "Queuing Delay (ms)",
    "graph_b_qdelay_timeseries.png",
)

# 3. Base Delay Over Time
plot_time_series_comparison(
    "Base Delay",
    "final-basedelay-",
    "Base Delay (ms)",
    "graph_b_basedelay_timeseries.png",
)

print("\nAll Category B graphs generated successfully!")
print("Files created:")
print("  - graph_b_wledbat_timeseries.png")
print("  - graph_b_qdelay_timeseries.png")
print("  - graph_b_basedelay_timeseries.png")
print("\nNote: Graphs show time-series for representative scenarios:")
for sweep_type, sweep_value, description in representative_scenarios:
    print(f"  - {description}")
