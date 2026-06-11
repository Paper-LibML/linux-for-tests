#!/usr/bin/env python3
# /// script
# dependencies = ["matplotlib>=3.7", "pandas>=2.0"]
# ///
"""
Gráfico simple: tiempo (ns/op) vs N para cada operación y modo.
Uso: uv run plot_simple.py [results_dir]
Salida: results/plots/simple_time_vs_n.png
"""
import sys, os, glob
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "results"
OUTDIR = os.path.join(RESULTS_DIR, "plots")
os.makedirs(OUTDIR, exist_ok=True)

# ── Carga de datos ──────────────────────────────────────────────
frames = []
for f in glob.glob(os.path.join(RESULTS_DIR, "*.csv")):
    df = pd.read_csv(f)
    frames.append(df)

data = pd.concat(frames, ignore_index=True)
data = data[data["ds"] != "sched/rbt"].copy()

# Normalizar "search*" → "search" para la lista (misma operación)
data["operation"] = data["operation"].str.replace("*", "", regex=False)

DS_COLORS = {"maple_tree": "#1976D2", "rbtree": "#D32F2F", "list": "#388E3C"}
DS_LABELS = {"maple_tree": "Maple Tree", "rbtree":  "RB-Tree",   "list": "Lista"}
DS_ORDER  = ["maple_tree", "rbtree", "list"]
OPS       = ["insert", "search", "iterate", "delete"]
MODES     = [("sequential", "Secuencial"), ("random", "Random")]

# ── Figura: 2 filas (modos) × 4 columnas (operaciones) ─────────
fig, axes = plt.subplots(
    2, 4, figsize=(16, 8), sharey=False,
    gridspec_kw={"hspace": 0.45, "wspace": 0.35}
)
fig.suptitle("Tiempo por operación vs N  —  estructuras de datos del kernel",
             fontsize=14, fontweight="bold", y=1.01)

for row, (mode_csv, mode_label) in enumerate(MODES):
    mode_data = data[data["mode"] == mode_csv]
    for col, op in enumerate(OPS):
        ax = axes[row][col]
        op_data = mode_data[mode_data["operation"] == op]

        for ds in DS_ORDER:
            subset = op_data[op_data["ds"] == ds].sort_values("n")
            if subset.empty:
                continue
            ax.plot(subset["n"], subset["ns_per_op"],
                    marker="o", markersize=5, linewidth=2,
                    color=DS_COLORS[ds], label=DS_LABELS[ds])

        ax.set_title(f"{op}\n[{mode_label}]", fontsize=10)
        ax.set_xlabel("N (elementos)", fontsize=8)
        ax.set_ylabel("ns / op", fontsize=8)
        ax.set_xscale("log")
        ax.grid(True, alpha=0.3)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
        ax.tick_params(axis="x", labelsize=7, rotation=15)
        ax.tick_params(axis="y", labelsize=8)

        if row == 0 and col == 0:
            ax.legend(fontsize=8, framealpha=0.9)

# Leyenda global abajo
handles = [
    plt.Line2D([0], [0], color=DS_COLORS[ds], marker="o", linewidth=2,
               markersize=5, label=DS_LABELS[ds])
    for ds in DS_ORDER
]
fig.legend(handles=handles, loc="lower center", ncol=3,
           fontsize=10, framealpha=0.9,
           bbox_to_anchor=(0.5, -0.04))

out = os.path.join(OUTDIR, "simple_time_vs_n.png")
fig.savefig(out, dpi=150, bbox_inches="tight")
print(f"Guardado: {out}")
