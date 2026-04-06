#!/usr/bin/env python3
"""
Visualize rLEDBAT trace data from ns-3 simulation.
Reads three .dat files and plots WLedbat, Queuing Delay, and Base Delay over time.
"""

import matplotlib.pyplot as plt
import numpy as np

# File paths
window_file = "scratch/rledbat-wledbat.dat"
qdelay_file = "scratch/rledbat-qdelay.dat"
basedelay_file = "scratch/rledbat-basedelay.dat"

# Load data (skip comment lines starting with #)
try:
    window_data = np.loadtxt(window_file, comments="#")
    qdelay_data = np.loadtxt(qdelay_file, comments="#")
    basedelay_data = np.loadtxt(basedelay_file, comments="#")
except FileNotFoundError as e:
    print(f"Error: {e}")
    print("Make sure you run the simulation first to generate the .dat files")
    exit(1)

# Extract time and values
# Format: column 0 = time (s), column 1 = value
window_time = window_data[:, 0]
window_val = window_data[:, 1]

qdelay_time = qdelay_data[:, 0]
qdelay_val = qdelay_data[:, 1]

basedelay_time = basedelay_data[:, 0]
basedelay_val = basedelay_data[:, 1]

# Simulation end time — must match SIM_TIME in RledbatTest.cc
SIM_END = 63.0
TARGET_DELAY_MS = 5.0  # must match targetDelayMs default in RledbatTest.cc

# Extend each trace to SIM_END by appending the last known value.
# The ns-3 trace callback is edge-triggered (only fires on value changes),
# so if a value holds constant until the socket closes, no final event is written.
window_time = np.append(window_time, SIM_END)
window_val = np.append(window_val, window_val[-1])
qdelay_time = np.append(qdelay_time, SIM_END)
qdelay_val = np.append(qdelay_val, qdelay_val[-1])
basedelay_time = np.append(basedelay_time, SIM_END)
basedelay_val = np.append(basedelay_val, basedelay_val[-1])

# Create figure with 3 subplots sharing the same x-axis
fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 10), sharex=True)

# Plot 1: WLedbat (advertised window)
ax1.step(window_time, window_val / 1000, "b-", linewidth=1.5, where="post")
ax1.set_ylabel("WLedbat (KB)", fontsize=12)
ax1.set_title("rLEDBAT Internal State Over Time", fontsize=14, fontweight="bold")
ax1.grid(True, alpha=0.3)
ax1.set_xlim([0, SIM_END])

# Plot 2: Queuing Delay
ax2.plot(qdelay_time, qdelay_val, "r-", linewidth=1.5)
ax2.set_ylabel("Queuing Delay (ms)", fontsize=12)
ax2.axhline(
    y=TARGET_DELAY_MS,
    color="g",
    linestyle="--",
    linewidth=1,
    label=f"Target = {TARGET_DELAY_MS:.0f}ms",
)
ax2.grid(True, alpha=0.3)
ax2.legend(loc="upper right")

# Plot 3: Base Delay
ax3.plot(basedelay_time, basedelay_val, "orange", linewidth=1.5)
ax3.set_ylabel("Base Delay (ms)", fontsize=12)
ax3.set_xlabel("Time (s)", fontsize=12)
ax3.grid(True, alpha=0.3)

# Add vertical lines for session boundaries (8-session test)
# Combination boundaries matching RledbatTest.cc
sessions = [
    (1, "C1 start"),
    (8, "Spike1"),
    (9, "Spike1 end"),
    (20, "C1 end"),
    (22, "C2 start"),
    (40, "C2 end"),
    (42, "C3 start"),
    (50, "Spike2"),
    (51, "Spike2 end"),
    (62, "C3 end"),
]

for ax in [ax1, ax2, ax3]:
    for time, label in sessions:
        ax.axvline(x=time, color="gray", linestyle=":", linewidth=0.8, alpha=0.5)

plt.tight_layout()
plt.savefig("scratch/rledbat-traces.png", dpi=300, bbox_inches="tight")
print("Plot saved to: scratch/rledbat-traces.png")

# Also save individual plots for detailed analysis
fig2, ax_window = plt.subplots(figsize=(12, 4))
ax_window.plot(window_time, window_val / 1000, "b-", linewidth=1.5)
ax_window.set_xlabel("Time (s)", fontsize=12)
ax_window.set_ylabel("WLedbat (KB)", fontsize=12)
ax_window.set_title("rLEDBAT Advertised Window", fontsize=14)
ax_window.set_xlim([0, SIM_END])
ax_window.grid(True, alpha=0.3)
for time, _ in sessions:
    ax_window.axvline(x=time, color="gray", linestyle=":", linewidth=0.8, alpha=0.5)
plt.tight_layout()
plt.savefig("scratch/rledbat-window-only.png", dpi=300, bbox_inches="tight")

fig3, ax_qdelay = plt.subplots(figsize=(12, 4))
ax_qdelay.plot(qdelay_time, qdelay_val, "r-", linewidth=1.5)
ax_qdelay.axhline(y=5, color="g", linestyle="--", linewidth=1.5, label="Target = 5ms")
ax_qdelay.set_xlabel("Time (s)", fontsize=12)
ax_qdelay.set_ylabel("Queuing Delay (ms)", fontsize=12)
ax_qdelay.set_title("rLEDBAT Queuing Delay Estimation", fontsize=14)
ax_qdelay.set_xlim([0, SIM_END])
ax_qdelay.grid(True, alpha=0.3)
ax_qdelay.legend(loc="upper right")
for time, _ in sessions:
    ax_qdelay.axvline(x=time, color="gray", linestyle=":", linewidth=0.8, alpha=0.5)
plt.tight_layout()
plt.savefig("scratch/rledbat-qdelay-only.png", dpi=300, bbox_inches="tight")

# print("Individual plots saved:")
# print("  - rledbat-window-only.png")
# print("  - rledbat-qdelay-only.png")
# print("\nDone!")

# Print summary statistics
# print("\n=== Summary Statistics ===")
# print(f"Window:  min={window_val.min()/1000:.2f} KB, max={window_val.max()/1000:.2f} KB, "
#      f"mean={window_val.mean()/1000:.2f} KB")
# print(f"Qdelay:  min={qdelay_val.min():.2f} ms, max={qdelay_val.max():.2f} ms, "
#      f"mean={qdelay_val.mean():.2f} ms")
# print(f"Base:    min={basedelay_val.min():.2f} ms, max={basedelay_val.max():.2f} ms, "
#      f"mean={basedelay_val.mean():.2f} ms")
