#!/usr/bin/env python3
"""
Plot Category C metrics: Jain Fairness, Yielding Ratio, Throughput Distribution
Clean, consistent, publication-ready plots
"""

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# 🔥 Global style
plt.rcParams.update(
    {
        "font.size": 12,
        "axes.titlesize": 14,
        "axes.labelsize": 12,
        "legend.fontsize": 10,
        "figure.titlesize": 18,
        "axes.facecolor": "white",
        "figure.facecolor": "white",
    }
)

# Read CSV
df = pd.read_csv("parsed_results.csv")
df_sweep = df[df["run_type"] == "sweep"].copy()

sweep_params = ["nodes", "flows", "pps", "speed"]

param_labels = {
    "nodes": "Number of Nodes",
    "flows": "Number of Flows",
    "pps": "Packets Per Second",
    "speed": "Node Speed (m/s)",
}

FIGSIZE = (16, 12)

# =========================================================
# 1. Jain Fairness Index
# =========================================================
fig, axes = plt.subplots(2, 2, figsize=FIGSIZE)
fig.suptitle("Jain Fairness Index vs Parameters", fontweight="bold")
axes = axes.flatten()

for idx, param in enumerate(sweep_params):
    ax = axes[idx]
    df_param = df_sweep[df_sweep["sweep_type"] == param].sort_values("sweep_value")

    df_base = df_param[df_param["pass"] == "base"]
    df_mod = df_param[df_param["pass"] == "modified"]

    if len(df_base) > 0:
        ax.plot(
            df_base["sweep_value"],
            df_base["fairness"],
            marker="o",
            linestyle="-",
            linewidth=2.5,
            color="blue",
            label="Base",
            zorder=2,
        )

    if len(df_mod) > 0:
        ax.plot(
            df_mod["sweep_value"],
            df_mod["fairness"],
            marker="s",
            linestyle="--",
            linewidth=2.5,
            color="green",
            label="Modified",
            zorder=2,
        )

    ax.set_xlabel(param.upper(), fontweight="bold")
    ax.set_ylabel("Jain Fairness Index")
    ax.set_title(param_labels[param], fontweight="bold")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", frameon=True)
    ax.set_ylim(0, 1.05)

    if len(df_param) > 0:
        ax.set_xlim(df_param["sweep_value"].min(), df_param["sweep_value"].max())

plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.savefig("graph_c_fairness.png", dpi=300, bbox_inches="tight")
print("Saved: graph_c_fairness.png")
plt.close()


# =========================================================
# 2. Yielding Ratio
# =========================================================
fig, axes = plt.subplots(2, 2, figsize=FIGSIZE)
fig.suptitle("Yielding Ratio vs Parameters", fontweight="bold")
axes = axes.flatten()

y_vals = df_sweep["yielding"].dropna()
y_min, y_max = (y_vals.min(), y_vals.max()) if len(y_vals) > 0 else (None, None)

for idx, param in enumerate(sweep_params):
    ax = axes[idx]
    df_param = df_sweep[df_sweep["sweep_type"] == param].sort_values("sweep_value")

    df_base = df_param[df_param["pass"] == "base"]
    df_mod = df_param[df_param["pass"] == "modified"]

    if len(df_base) > 0:
        ax.plot(
            df_base["sweep_value"],
            df_base["yielding"],
            marker="o",
            linestyle="-",
            linewidth=2.5,
            color="blue",
            label="Base",
            zorder=2,
        )

    if len(df_mod) > 0:
        ax.plot(
            df_mod["sweep_value"],
            df_mod["yielding"],
            marker="s",
            linestyle="--",
            linewidth=2.5,
            color="green",
            label="Modified",
            zorder=2,
        )

    ax.axhline(
        y=1.0,
        linestyle=":",
        linewidth=2,
        alpha=0.7,
        color="red",
        label="Ideal (1.0)",
        zorder=1,
    )

    ax.set_xlabel(param.upper(), fontweight="bold")
    ax.set_ylabel("Yielding Ratio (rLED/Int)")
    ax.set_title(param_labels[param], fontweight="bold")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", frameon=True)

    if len(df_param) > 0:
        ax.set_xlim(df_param["sweep_value"].min(), df_param["sweep_value"].max())

    if y_min is not None:
        ax.set_ylim(y_min, y_max)

plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.savefig("graph_c_yielding.png", dpi=300, bbox_inches="tight")
print("Saved: graph_c_yielding.png")
plt.close()


# =========================================================
# 3. Throughput Distribution (FIXED)
# =========================================================
fig, axes = plt.subplots(2, 2, figsize=FIGSIZE)
fig.suptitle("Per-Flow Throughput Distribution vs Parameters", fontweight="bold")
axes = axes.flatten()

vals = pd.concat([df_sweep["rled_mean"], df_sweep["int_mean"]]).dropna()
y_min, y_max = (vals.min(), vals.max()) if len(vals) > 0 else (None, None)

for idx, param in enumerate(sweep_params):
    ax = axes[idx]
    df_param = df_sweep[df_sweep["sweep_type"] == param].sort_values("sweep_value")

    df_base = df_param[df_param["pass"] == "base"]
    df_mod = df_param[df_param["pass"] == "modified"]

    if len(df_base) > 0:
        x = df_base["sweep_value"].values

        # rLEDBAT
        mean = df_base["rled_mean"]
        std = df_base["rled_std"]
        ax.plot(
            x,
            mean,
            marker="o",
            linestyle="-",
            linewidth=2.5,
            color="blue",
            label="Base rLEDBAT",
            zorder=2,
        )
        ax.fill_between(
            x, mean - std, mean + std, color="blue", alpha=0.1, linewidth=0, zorder=1
        )

        # Interactive
        mean = df_base["int_mean"]
        std = df_base["int_std"]
        ax.plot(
            x,
            mean,
            marker="^",
            linestyle="-",
            linewidth=2.5,
            color="red",
            label="Base Interactive",
            zorder=2,
        )
        ax.fill_between(
            x, mean - std, mean + std, color="red", alpha=0.1, linewidth=0, zorder=1
        )

    if len(df_mod) > 0:
        x = df_mod["sweep_value"].values

        # rLEDBAT
        mean = df_mod["rled_mean"]
        std = df_mod["rled_std"]
        ax.plot(
            x,
            mean,
            marker="s",
            linestyle="--",
            linewidth=2.5,
            color="cyan",
            label="Modified rLEDBAT",
            zorder=2,
        )
        ax.fill_between(
            x, mean - std, mean + std, color="cyan", alpha=0.1, linewidth=0, zorder=1
        )

        # Interactive
        mean = df_mod["int_mean"]
        std = df_mod["int_std"]
        ax.plot(
            x,
            mean,
            marker="D",
            linestyle="--",
            linewidth=2.5,
            color="orange",
            label="Modified Interactive",
            zorder=2,
        )
        ax.fill_between(
            x, mean - std, mean + std, color="orange", alpha=0.1, linewidth=0, zorder=1
        )

    ax.set_xlabel(param.upper(), fontweight="bold")
    ax.set_ylabel("Per-Flow Throughput (Mbps)")
    ax.set_title(param_labels[param], fontweight="bold")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", frameon=True)

    if len(df_param) > 0:
        ax.set_xlim(df_param["sweep_value"].min(), df_param["sweep_value"].max())

    if y_min is not None:
        ax.set_ylim(y_min, y_max)

plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.savefig("graph_c_throughput_distribution.png", dpi=300, bbox_inches="tight")
print("Saved: graph_c_throughput_distribution.png")
plt.close()


print("\nAll Category C graphs generated successfully!")
