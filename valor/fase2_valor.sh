#!/usr/bin/env bash
# Fase 2 (host-de-pruebas): experimento de valor. Uso: valor/fase2_valor.sh [W_MiB] [presupuesto_MiB] [reps]
# Cada corrida va en su propio scope de systemd --user con MemoryMax = base_CUDA + presupuesto.
set -uo pipefail
cd "$(dirname "$0")/.."
W=${1:-1536}; B=${2:-256}; R=${3:-5}; NR=600; NS=$W
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
F=$PWD/valor_ws.bin
OUT=valor/resultados-host-de-pruebas-$(date +%Y%m%d-%H%M).txt
exec > >(tee "$OUT") 2>&1
echo "== $(date '+%F %T %Z') W=${W}MiB presupuesto=${B}MiB reps=$R aleat=$NR accesos secu=$NS accesos"
nvidia-smi --query-gpu=name,driver_version,memory.used,utilization.gpu --format=csv,noheader
[ -f "$F" ] || ./build/gen_file "$F" "$W"
# base de memoria del proceso tras crear el contexto CUDA (sin límite)
BASE=$(systemd-run --user --scope --quiet ./build/valor_gpu explicit "$F" "$W" 1 aleat "$B" 1 | sed -n 's/.*base_cgroup_MiB=\([0-9]*\).*/\1/p')
LIM=$((BASE + B + 16)); echo "base CUDA=${BASE}MiB -> MemoryMax=${LIM}M"
for r in $(seq "$R"); do
  for pat in aleat secu; do
    N=$NR; [ "$pat" = secu ] && N=$NS
    for m in swap tiermem mmap explicit; do
      systemd-run --user --scope --quiet -p MemoryMax=${LIM}M -p MemorySwapMax=infinity \
        ./build/valor_gpu $m "$F" "$W" "$N" "$pat" "$B" $((100 + r)) | grep -E "^RES|^# tiermem faults"
    done
  done
done
rm -f "$F"
echo "== FIN $(date '+%T')  salida: $OUT"
