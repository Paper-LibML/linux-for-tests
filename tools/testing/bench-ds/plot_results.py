#!/usr/bin/env python3
# /// script
# dependencies = [
#   "matplotlib>=3.7",
#   "pandas>=2.0",
#   "numpy>=1.24",
# ]
# ///
"""
Visualización del Kernel DS Benchmark.

Uso:
    uv run plot_results.py [results_dir]

    results_dir: directorio con los CSV de run_benchmarks.sh
                 (default: ./results)

Figuras generadas en results/plots/:
    01_comparacion_ops.png       — bar chart: DS × operacion, seq vs random
    02_escalabilidad_seq.png     — line chart: ns/op vs n, modo seq
    02_escalabilidad_random.png  — line chart: ns/op vs n, modo random
    03_impacto_modo.png          — grouped bars: seq vs random por DS
    04_sched_simulation.png      — tick time vs runqueue size
    05_peor_caso.png             — bar chart en n máximo disponible
"""

import sys
import glob
import os

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# ──────────────────────────────────────────────────────────────
# 0. Configuración global de estilo
# ──────────────────────────────────────────────────────────────
plt.rcParams.update({
    "figure.dpi": 150,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "font.size": 10,
})

DS_COLORS = {
    "maple_tree": "#1976D2",   # azul
    "rbtree":     "#D32F2F",   # rojo
    "list":       "#388E3C",   # verde
}
DS_LABELS = {
    "maple_tree": "Maple Tree",
    "rbtree":     "RB-Tree",
    "list":       "Lista",
}
DS_ORDER = ["maple_tree", "rbtree", "list"]

OPS_ORDER   = ["insert", "search", "search*", "iterate", "delete"]
OPS_DISPLAY = {
    "insert":  "insert",
    "search":  "search",
    "search*": "search*",
    "iterate": "iterate",
    "delete":  "delete",
}

# ──────────────────────────────────────────────────────────────
# 1. Carga de datos
# ──────────────────────────────────────────────────────────────
RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "results"
OUTDIR = os.path.join(RESULTS_DIR, "plots")
os.makedirs(OUTDIR, exist_ok=True)

csv_files = glob.glob(os.path.join(RESULTS_DIR, "*.csv"))
if not csv_files:
    print(f"No se encontraron CSV en '{RESULTS_DIR}'.")
    print("Ejecuta ./run_benchmarks.sh primero.")
    sys.exit(1)

frames = []
for f in csv_files:
    df = pd.read_csv(f)
    basename = os.path.basename(f).replace(".csv", "")
    parts = basename.split("_")
    try:
        file_n = int(parts[-1])
    except ValueError:
        continue
    df["file_n"] = file_n
    frames.append(df)

data = pd.concat(frames, ignore_index=True)

# Separar scheduler vs estructuras principales
is_sched = data["ds"] == "sched/rbt"
sched_raw = data[is_sched].copy()
ds_data   = data[~is_sched].copy()

# Extraer rq_size de "tick n=32" → 32
sched_data = sched_raw.copy()
sched_data["rq_size"] = (
    sched_data["operation"].str.extract(r"n=(\d+)")[0].astype(int)
)

available_ns = sorted(ds_data["file_n"].unique())
N_REF = 100000 if 100000 in available_ns else available_ns[-1]

def save(fig, name):
    path = os.path.join(OUTDIR, name)
    fig.savefig(path, bbox_inches="tight")
    print(f"  Guardado: {path}")
    plt.close(fig)

def bar_group(ax, data_subset, ops, width=0.25):
    """Dibuja barras agrupadas por DS para las operaciones dadas."""
    x = np.arange(len(ops))
    for i, ds in enumerate(DS_ORDER):
        vals = []
        for op in ops:
            row = data_subset[(data_subset["ds"] == ds) &
                              (data_subset["operation"] == op)]
            vals.append(float(row["ns_per_op"].iloc[0]) if not row.empty else 0.0)
        ax.bar(x + i * width, vals, width,
               label=DS_LABELS[ds],
               color=DS_COLORS[ds],
               alpha=0.85, edgecolor="white", linewidth=0.5)
    ax.set_xticks(x + width)
    ax.set_xticklabels(ops)
    return x

# ──────────────────────────────────────────────────────────────
# 2. Figura 01 — Comparación de operaciones (n=N_REF)
#    Dos subplots: seq  |  random
# ──────────────────────────────────────────────────────────────
print("\n[1/5] Comparación de operaciones principales...")

fig, axes = plt.subplots(1, 2, figsize=(15, 6))
fig.suptitle(
    f"Comparación de estructuras de datos del kernel  (n = {N_REF:,})",
    fontsize=13, fontweight="bold"
)

for ax, mode, title in zip(axes,
                            ["seq", "sequential"],
                            ["Modo secuencial (mejor caso maple_tree)",
                             "Modo random (peor caso de caché)"]):
    # El CSV guarda "sequential" o "random" en la columna mode
    subset = ds_data[(ds_data["file_n"] == N_REF) &
                     (ds_data["mode"].isin([mode, "sequential" if mode == "seq" else "random"]))]
    ops = [op for op in OPS_ORDER if op in subset["operation"].values]
    bar_group(ax, subset, ops)
    ax.set_title(title)
    ax.set_xlabel("Operación")
    ax.set_ylabel("ns / operación (escala log)")
    ax.set_yscale("log")
    ax.legend(framealpha=0.9)

save(fig, "01_comparacion_ops.png")

# ──────────────────────────────────────────────────────────────
# 3. Figura 02 — Escalabilidad: ns/op vs n  (una figura por modo)
# ──────────────────────────────────────────────────────────────
print("[2/5] Escalabilidad (ns/op vs n)...")

TARGET_OPS = ["insert", "search", "iterate", "delete"]

for mode_label, mode_csv in [("Secuencial", "sequential"), ("Random", "random")]:
    mode_data = ds_data[ds_data["mode"] == mode_csv]

    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    fig.suptitle(
        f"Escalabilidad — ns/op vs número de elementos  [{mode_label}]",
        fontsize=13, fontweight="bold"
    )

    for ax, op in zip(axes.flat, TARGET_OPS):
        op_variants = {op, op + "*"}
        op_data = mode_data[mode_data["operation"].isin(op_variants)]

        for ds in DS_ORDER:
            subset = op_data[op_data["ds"] == ds].sort_values("file_n")
            if not subset.empty:
                ax.plot(subset["file_n"], subset["ns_per_op"],
                        marker="o", label=DS_LABELS[ds],
                        color=DS_COLORS[ds], linewidth=2, markersize=5)

        ax.set_title(op)
        ax.set_xlabel("n (log)")
        ax.set_ylabel("ns / operación")
        ax.set_xscale("log")
        ax.legend(framealpha=0.9)
        ax.xaxis.set_major_formatter(mticker.ScalarFormatter())

    fig.tight_layout()
    fname = f"02_escalabilidad_{mode_csv}.png"
    save(fig, fname)

# ──────────────────────────────────────────────────────────────
# 4. Figura 03 — Impacto del modo: seq vs random para n=N_REF
#    Una columna por DS
# ──────────────────────────────────────────────────────────────
print("[3/5] Impacto del modo de acceso (seq vs random)...")

fig, axes = plt.subplots(1, 3, figsize=(16, 6), sharey=False)
fig.suptitle(
    f"Impacto del orden de acceso — seq vs random  (n = {N_REF:,})",
    fontsize=13, fontweight="bold"
)

for ax, ds in zip(axes, DS_ORDER):
    ds_sub = ds_data[(ds_data["file_n"] == N_REF) & (ds_data["ds"] == ds)]
    ops_found, seq_vals, rnd_vals = [], [], []

    for op in ["insert", "search", "iterate", "delete"]:
        for op_variant in [op, op + "*"]:
            seq_row = ds_sub[(ds_sub["mode"] == "sequential") &
                             (ds_sub["operation"] == op_variant)]
            rnd_row = ds_sub[(ds_sub["mode"] == "random") &
                             (ds_sub["operation"] == op_variant)]
            if not seq_row.empty and not rnd_row.empty:
                ops_found.append(op)
                seq_vals.append(float(seq_row["ns_per_op"].iloc[0]))
                rnd_vals.append(float(rnd_row["ns_per_op"].iloc[0]))
                break

    x = np.arange(len(ops_found))
    w = 0.35
    ax.bar(x - w/2, seq_vals, w, label="Sequential",
           color="#1565C0", alpha=0.85, edgecolor="white")
    ax.bar(x + w/2, rnd_vals, w, label="Random",
           color="#B71C1C", alpha=0.85, edgecolor="white")
    ax.set_title(DS_LABELS[ds])
    ax.set_xlabel("Operación")
    ax.set_ylabel("ns / operación (log)")
    ax.set_xticks(x)
    ax.set_xticklabels(ops_found)
    ax.set_yscale("log")
    ax.legend(framealpha=0.9)

fig.tight_layout()
save(fig, "03_impacto_modo.png")

# ──────────────────────────────────────────────────────────────
# 5. Figura 04 — Simulación del scheduler CFS (rbtree runqueue)
#    tick time vs tamaño del runqueue
# ──────────────────────────────────────────────────────────────
print("[4/5] Simulación del scheduler...")

if sched_data.empty:
    print("  (sin datos de scheduler — omitido)")
else:
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    fig.suptitle(
        "Simulación CFS scheduler — tick time vs tamaño del runqueue\n"
        "(tick = pick_next [O(1)] + dequeue + vruntime += 4ms + enqueue [O(log n)])",
        fontsize=12, fontweight="bold"
    )

    CMAP = plt.get_cmap("tab10")

    for ax, mode_csv, mode_label in zip(
            axes,
            ["sequential", "random"],
            ["Modo secuencial", "Modo random"]):

        mode_sched = sched_data[sched_data["mode"] == mode_csv]
        ns_sorted = sorted(mode_sched["file_n"].unique())

        for ci, n_val in enumerate(ns_sorted):
            subset = mode_sched[mode_sched["file_n"] == n_val].sort_values("rq_size")
            if not subset.empty:
                ax.plot(subset["rq_size"], subset["ns_per_op"],
                        marker="o", label=f"n={n_val:,}",
                        color=CMAP(ci), linewidth=2, markersize=5)

        # Marca visual: pid_max por defecto
        ax.axvline(x=32, color="gray", linestyle="--", linewidth=1, alpha=0.6)
        ax.text(34, ax.get_ylim()[1] * 0.95 if ax.get_ylim()[1] > 0 else 1,
                "~típico\npor CPU", fontsize=8, color="gray", va="top")

        ax.set_title(mode_label)
        ax.set_xlabel("Tareas en el runqueue (escala log₂)")
        ax.set_ylabel("ns / tick")
        ax.set_xscale("log", base=2)
        ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
        ax.legend(framealpha=0.9, fontsize=8)

    fig.tight_layout()
    save(fig, "04_sched_simulation.png")

# ──────────────────────────────────────────────────────────────
# 6. Figura 05 — Peor caso: n máximo disponible
#    Muestra cómo se comportan las estructuras en el extremo
# ──────────────────────────────────────────────────────────────
print("[5/5] Peor caso / stress test...")

max_n = available_ns[-1]

fig, axes = plt.subplots(1, 2, figsize=(15, 6))
fig.suptitle(
    f"Peor caso — n = {max_n:,}  (mayor tamaño probado)",
    fontsize=13, fontweight="bold"
)

for ax, mode_csv, mode_label in zip(
        axes,
        ["sequential", "random"],
        ["Secuencial", "Random (peor caso de caché)"]):

    subset = ds_data[(ds_data["file_n"] == max_n) & (ds_data["mode"] == mode_csv)]
    if subset.empty:
        ax.text(0.5, 0.5, f"Sin datos\n{mode_label}\nn={max_n:,}",
                ha="center", va="center", transform=ax.transAxes, fontsize=12)
        continue

    ops = [op for op in OPS_ORDER if op in subset["operation"].values]
    bar_group(ax, subset, ops)
    ax.set_title(mode_label)
    ax.set_xlabel("Operación")
    ax.set_ylabel("ns / operación (log)")
    ax.set_yscale("log")
    ax.legend(framealpha=0.9)

fig.tight_layout()
save(fig, "05_peor_caso.png")

# ──────────────────────────────────────────────────────────────
# 7. Figura 06 — Resumen: pid_max (n=32768) — escenario max-processes
# ──────────────────────────────────────────────────────────────
PID_MAX = 32768
if PID_MAX in available_ns:
    print("[+] Escenario pid_max (n=32768)...")
    fig, axes = plt.subplots(1, 2, figsize=(15, 6))
    fig.suptitle(
        f"Escenario 'máximo de procesos' — n = {PID_MAX:,}  (Linux pid_max por defecto)\n"
        "Simula una tabla de procesos llena",
        fontsize=12, fontweight="bold"
    )

    for ax, mode_csv, mode_label in zip(
            axes,
            ["sequential", "random"],
            ["Acceso ordenado (p.ej. iteración del kernel)",
             "Acceso aleatorio (búsquedas por PID arbitrario)"]):
        subset = ds_data[(ds_data["file_n"] == PID_MAX) & (ds_data["mode"] == mode_csv)]
        if subset.empty:
            continue
        ops = [op for op in OPS_ORDER if op in subset["operation"].values]
        bar_group(ax, subset, ops)
        ax.set_title(mode_label)
        ax.set_xlabel("Operación")
        ax.set_ylabel("ns / operación (log)")
        ax.set_yscale("log")
        ax.legend(framealpha=0.9)

    fig.tight_layout()
    save(fig, "06_pid_max_scenario.png")

print(f"\nTodas las graficas en: {OUTDIR}/")
