#!/bin/bash
# run_benchmarks.sh — Genera un CSV por (modo × n).
#
# Uso:
#   ./run_benchmarks.sh            # usa ./bench_ds
#   ./run_benchmarks.sh /otra/ruta/bench_ds
#
# Salida: results/seq_<n>.csv  y  results/random_<n>.csv
#
# Tamaños:
#   1 000        — tiny, referencia mínima
#  10 000        — pequeño
#  32 768        — pid_max por defecto de Linux (max procesos realista)
# 100 000        — referencia estándar (comparación con resultados anteriores)
# 500 000        — peor caso / stress test

set -euo pipefail

BENCH="${1:-./bench_ds}"
RESULTS_DIR="results"

if [ ! -x "$BENCH" ]; then
    echo "Error: '$BENCH' no encontrado o no ejecutable."
    echo "  Compila primero con: make"
    exit 1
fi

mkdir -p "$RESULTS_DIR"

SIZES=(1000 10000 32768 100000 500000)

run_scenario() {
    local n="$1"
    local mode="$2"
    local out="$RESULTS_DIR/${mode}_${n}.csv"
    printf "  n=%7d  %-10s -> %s ... " "$n" "$mode" "$out"
    "$BENCH" "$n" "$mode" "$out" > /dev/null
    echo "OK"
}

echo "==================================================================="
echo "  Kernel DS Benchmark Suite"
echo "  Binario : $BENCH"
echo "  Salida  : $RESULTS_DIR/"
echo "==================================================================="

echo ""
echo "--- Modo: seq (mejor caso para maple_tree, acceso contiguo) ---"
for n in "${SIZES[@]}"; do
    run_scenario "$n" seq
done

echo ""
echo "--- Modo: random (peor caso de cache, semilla=42 reproducible) ---"
for n in "${SIZES[@]}"; do
    run_scenario "$n" random
done

echo ""
echo "==================================================================="
echo "  Resultados:"
ls -lh "$RESULTS_DIR/"*.csv
echo "==================================================================="
echo ""
echo "Para generar gráficas:"
echo "  uv run plot_results.py"
