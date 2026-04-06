import csv
import re

import numpy as np

input_file = "final_integrated_results.txt"
output_file = "parsed_results.csv"

rows = []

current_pass = None
current_sweep = None
current_run_label = None


def extract_value(pattern, text):
    m = re.search(pattern, text)
    return float(m.group(1)) if m else None


with open(input_file, "r") as f:
    lines = f.readlines()

i = 0
while i < len(lines):
    line = lines[i].strip()

    # ---------------- PASS ----------------
    if line.startswith("PASS:"):
        current_pass = line.split(":")[1].strip()

    # ---------------- SWEEP ----------------
    elif line.startswith("Sweep"):
        current_sweep = line.split()[1].lower()

    # ---------------- RUN LABEL (independent runs) ----------------
    elif line.startswith("Run:"):
        current_run_label = line.replace("Run:", "").strip()

    # ---------------- TAG (START OF ONE EXPERIMENT) ----------------
    elif line.startswith("Tag:"):
        tag_name = line.split(":")[1].strip()

        # Defaults
        run_type = "sweep"
        sweep_type = current_sweep
        sweep_value = None

        # ---- Independent Runs ----
        if tag_name.startswith("run_"):
            run_type = "independent"
            sweep_type = tag_name.replace("run_", "")  # cadf_only, ecs_only, atd_only
            sweep_value = 1

        # ---- Sweep Runs ----
        else:
            config_line = ""

            if i + 2 < len(lines) and "Config:" in lines[i + 2]:
                config_line = lines[i + 2]

            if config_line:
                if current_sweep == "nodes":
                    m = re.search(r"N=(\d+)", config_line)
                    sweep_value = int(m.group(1)) if m else None

                elif current_sweep == "flows":
                    m = re.search(r"flows=(\d+)", config_line)
                    sweep_value = int(m.group(1)) if m else None

                elif current_sweep == "pps":
                    m = re.search(r"pps=(\d+)", config_line)
                    sweep_value = int(m.group(1)) if m else None

                elif current_sweep == "speed":
                    m = re.search(r"speed=([\d\.]+)", config_line)
                    sweep_value = float(m.group(1)) if m else None

                elif current_sweep == "coverage":
                    m = re.search(r"coverage=([\d\.]+)", config_line)
                    sweep_value = float(m.group(1)) if m else None

        # ---------------- BLOCK EXTRACTION ----------------
        block_lines = []
        j = i + 1

        while (
            j < len(lines)
            and not lines[j].startswith("Tag:")
            and not lines[j].startswith("====")
        ):
            block_lines.append(lines[j])
            j += 1

        block = "\n".join(block_lines)

        # ---------------- METRICS ----------------
        total_tput = extract_value(r"Total Network\s+:\s+([\d\.]+)", block)
        rled_tput = extract_value(r"rLEDBAT Flows\s+:\s+([\d\.]+)", block)
        int_tput = extract_value(r"Interactive Flows:\s+([\d\.]+)", block)

        yielding = extract_value(r"Yielding Ratio\s+:\s+([\d\.]+)", block)
        fairness = extract_value(r"Jain Fairness\s+:\s+([\d\.]+)", block)

        delay_avg = extract_value(
            r"\[E2E Delay\][\s\S]*?Average\s+:\s+([\d\.]+)", block
        )
        delay_rled = extract_value(r"rLEDBAT avg\s+:\s+([\d\.]+)", block)
        delay_int = extract_value(r"Interactive avg\s+:\s+([\d\.]+)", block)

        pdr_total = extract_value(r"\[PDR\][\s\S]*?Total\s+:\s+([\d\.]+)", block)
        pdr_rled = extract_value(r"\[PDR\][\s\S]*?rLEDBAT\s+:\s+([\d\.]+)", block)
        pdr_int = extract_value(r"\[PDR\][\s\S]*?Interactive\s+:\s+([\d\.]+)", block)

        drop_total = extract_value(
            r"\[Drop Ratio\][\s\S]*?Total\s+:\s+([\d\.]+)", block
        )
        drop_rled = extract_value(
            r"\[Drop Ratio\][\s\S]*?rLEDBAT\s+:\s+([\d\.]+)", block
        )
        drop_int = extract_value(
            r"\[Drop Ratio\][\s\S]*?Interactive\s+:\s+([\d\.]+)", block
        )

        jitter_avg = extract_value(r"\[Jitter\][\s\S]*?Average\s+:\s+([\d\.]+)", block)
        jitter_rled = extract_value(
            r"\[Jitter\][\s\S]*?rLEDBAT avg\s+:\s+([\d\.]+)", block
        )
        jitter_int = extract_value(
            r"\[Jitter\][\s\S]*?Interactive avg\s+:\s+([\d\.]+)", block
        )

        energy_total = extract_value(r"Total \(all nodes\):\s+([\d\.]+)", block)
        energy_per_node = extract_value(r"Per-Node Average\s+:\s+([\d\.]+)", block)

        # ---------------- PER-SENDER THROUGHPUT ----------------
        rled_vals = []
        int_vals = []

        k = i
        while k < len(lines) and "[Per-Sender Throughput]" not in lines[k]:
            k += 1
        k += 1

        while k < len(lines) and not lines[k].startswith("[Per-Flow"):
            l = lines[k].strip()

            m1 = re.search(r"rLEDBAT.*:\s+([\d\.]+)\s+Mbps", l)
            m2 = re.search(r"Interactive.*:\s+([\d\.]+)\s+Mbps", l)

            if m1:
                rled_vals.append(float(m1.group(1)))
            if m2:
                int_vals.append(float(m2.group(1)))

            k += 1

        # ---- Mean + Std ----
        rled_mean = np.mean(rled_vals) if rled_vals else 0
        rled_std = np.std(rled_vals) if rled_vals else 0

        int_mean = np.mean(int_vals) if int_vals else 0
        int_std = np.std(int_vals) if int_vals else 0

        # ---------------- STORE ROW ----------------
        rows.append(
            [
                run_type,
                sweep_type,
                sweep_value,
                tag_name,
                current_pass,
                total_tput,
                rled_tput,
                int_tput,
                delay_avg,
                delay_rled,
                delay_int,
                pdr_total,
                pdr_rled,
                pdr_int,
                drop_total,
                drop_rled,
                drop_int,
                jitter_avg,
                jitter_rled,
                jitter_int,
                energy_total,
                energy_per_node,
                yielding,
                fairness,
                rled_mean,
                rled_std,
                int_mean,
                int_std,
            ]
        )

    i += 1

# ---------------- WRITE CSV ----------------
header = [
    "run_type",
    "sweep_type",
    "sweep_value",
    "tag",
    "pass",
    "total_tput",
    "rled_tput",
    "int_tput",
    "delay_avg",
    "delay_rled",
    "delay_int",
    "pdr_total",
    "pdr_rled",
    "pdr_int",
    "drop_total",
    "drop_rled",
    "drop_int",
    "jitter_avg",
    "jitter_rled",
    "jitter_int",
    "energy_total",
    "energy_per_node",
    "yielding",
    "fairness",
    "rled_mean",
    "rled_std",
    "int_mean",
    "int_std",
]

with open(output_file, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(header)
    writer.writerows(rows)

print(f"Saved to {output_file}")
