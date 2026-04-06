#!/usr/bin/env python3
"""
Plot Category A metrics: Network Throughput, E2E Delay, PDR, Drop Ratio, Jitter, Energy Consumption
Each metric gets one figure with 4 subplots (nodes, flows, pps, speed)
"""

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# Read CSV
df = pd.read_csv("parsed_results.csv")

# Filter only sweep runs
df_sweep = df[df["run_type"] == "sweep"].copy()

# Parameters to sweep
sweep_params = ["nodes", "flows", "pps", "speed"]


# Create figure for each metric
def plot_metric_grid(
    metric_name, metric_cols, ylabel, filename, has_class_breakdown=True
):
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle(f"{metric_name} vs Parameters", fontweight="bold")

    axes = axes.flatten()

    # 🔥 Compute global Y range for consistency
    y_min, y_max = None, None
    for col in metric_cols.values():
        vals = df_sweep[col].dropna()
        if len(vals) > 0:
            if y_min is None:
                y_min, y_max = vals.min(), vals.max()
            else:
                y_min = min(y_min, vals.min())
                y_max = max(y_max, vals.max())

    for idx, param in enumerate(sweep_params):
        ax = axes[idx]
        df_param = df_sweep[df_sweep["sweep_type"] == param].sort_values("sweep_value")

        for label, col in metric_cols.items():
            pass_type = "base" if "Base" in label else "modified"
            df_plot = df_param[df_param["pass"] == pass_type]

            if len(df_plot) > 0:
                linestyle = "-" if "Base" in label else "--"

                if "Total" in label:
                    color = "black"
                    linewidth = 2.5
                elif "rLEDBAT" in label or "rLED" in label:
                    color = "blue"
                    linewidth = 2
                elif "Interactive" in label or "Int" in label:
                    color = "red"
                    linewidth = 2
                else:
                    color = None
                    linewidth = 2

                ax.plot(
                    df_plot["sweep_value"],
                    df_plot[col],
                    marker="o",
                    linestyle=linestyle,
                    linewidth=linewidth,
                    color=color,
                    label=label,
                )

        ax.set_xlabel(param.upper(), fontweight="bold")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", frameon=True)

        # Consistent axis
        if len(df_param) > 0:
            ax.set_xlim(df_param["sweep_value"].min(), df_param["sweep_value"].max())

        if y_min is not None:
            ax.set_ylim(y_min, y_max)

        param_label = {
            "nodes": "Number of Nodes",
            "flows": "Number of Flows",
            "pps": "Packets Per Second",
            "speed": "Node Speed (m/s)",
        }
        ax.set_title(param_label[param], fontweight="bold")

    plt.tight_layout(rect=[0, 0, 1, 0.96])
    plt.savefig(filename, dpi=300, bbox_inches="tight")
    print(f"Saved: {filename}")
    plt.close()


# 1. Network Throughput (6 lines)
plot_metric_grid(
    "Network Throughput",
    {
        "Base Total": "total_tput",
        "Base rLEDBAT": "rled_tput",
        "Base Interactive": "int_tput",
        "Modified Total": "total_tput",
        "Modified rLEDBAT": "rled_tput",
        "Modified Interactive": "int_tput",
    },
    "Throughput (Mbps)",
    "graph_a_throughput.png",
)

# 2. E2E Delay (6 lines)
plot_metric_grid(
    "End-to-End Delay",
    {
        "Base Avg": "delay_avg",
        "Base rLEDBAT": "delay_rled",
        "Base Interactive": "delay_int",
        "Modified Avg": "delay_avg",
        "Modified rLEDBAT": "delay_rled",
        "Modified Interactive": "delay_int",
    },
    "Delay (ms)",
    "graph_a_delay.png",
)

# 3. PDR (6 lines)
plot_metric_grid(
    "Packet Delivery Ratio",
    {
        "Base Total": "pdr_total",
        "Base rLEDBAT": "pdr_rled",
        "Base Interactive": "pdr_int",
        "Modified Total": "pdr_total",
        "Modified rLEDBAT": "pdr_rled",
        "Modified Interactive": "pdr_int",
    },
    "PDR (%)",
    "graph_a_pdr.png",
)

# 4. Drop Ratio (6 lines)
plot_metric_grid(
    "Drop Ratio",
    {
        "Base Total": "drop_total",
        "Base rLEDBAT": "drop_rled",
        "Base Interactive": "drop_int",
        "Modified Total": "drop_total",
        "Modified rLEDBAT": "drop_rled",
        "Modified Interactive": "drop_int",
    },
    "Drop Ratio (%)",
    "graph_a_drop.png",
)

# 5. Jitter (6 lines)
plot_metric_grid(
    "Jitter",
    {
        "Base Avg": "jitter_avg",
        "Base rLEDBAT": "jitter_rled",
        "Base Interactive": "jitter_int",
        "Modified Avg": "jitter_avg",
        "Modified rLEDBAT": "jitter_rled",
        "Modified Interactive": "jitter_int",
    },
    "Jitter (ms)",
    "graph_a_jitter.png",
)

# 6. Energy Consumption (2 lines: base vs modified total energy)
plot_metric_grid(
    "Energy Consumption",
    {"Base Total Energy": "energy_total", "Modified Total Energy": "energy_total"},
    "Total Energy (J)",
    "graph_a_energy.png",
    has_class_breakdown=False,
)

print("\nAll Category A graphs generated successfully!")
print("Files created:")
print("  - graph_a_throughput.png")
print("  - graph_a_delay.png")
print("  - graph_a_pdr.png")
print("  - graph_a_drop.png")
print("  - graph_a_jitter.png")
print("  - graph_a_energy.png")
