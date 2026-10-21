#!/usr/bin/env bash
# Fase 1 (host-de-pruebas, usuario usuario-de-pruebas): sondeo de swap/cgroup, compilación, pruebas, binario GPU.
# No usa sudo, no toca configuración del sistema.
set -uo pipefail
cd "$(dirname "$0")/.."
echo "== $(date '+%F %T %Z') host=$(hostname) user=$(whoami) kernel=$(uname -r)"
echo "== swap / cgroup"
cat /proc/swaps; free -m | sed -n 1,3p
cat /proc/self/cgroup
U=/sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service
echo "controladores delegados en $U: $(cat $U/cgroup.controllers 2>&1)"
echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<vacío>}  bus: $(ls -l /run/user/$(id -u)/bus 2>&1)"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
systemd-run --user --scope --quiet -p MemoryMax=64M -p MemorySwapMax=infinity \
  bash -c 'echo "prueba scope: cgroup=$(cut -d: -f3 /proc/self/cgroup) memory.max=$(cat /sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)/memory.max) swap.max=$(cat /sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)/memory.swap.max)"' 2>&1
echo "systemd-run rc=$?"
echo "== compilación Release (-Werror), liburing: $(ls /usr/include/liburing.h 2>/dev/null || echo ausente)"
rm -rf build build-asan build-tsan build-cufile
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j8 2>&1 | grep -E "warning|error|Built target"
for i in 1 2 3; do echo "-- ctest Release corrida $i"; (cd build && ctest 2>&1 | grep -E "tests passed|tests failed|Failed|Total Test time"); done
echo "-- fault_secuencial x20"; ok=0; for i in $(seq 20); do (cd build && ./tiermem_tests fault_secuencial >/dev/null 2>&1) && ok=$((ok+1)); done; echo "fault_secuencial $ok/20"
echo "== ASan+UBSan"
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DTIERMEM_SANITIZE=ON >/dev/null && cmake --build build-asan -j8 2>&1 | grep -E "warning|error" ; (cd build-asan && ctest 2>&1 | grep -E "tests passed|tests failed|Failed")
echo "== TSan"
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DTIERMEM_TSAN=ON >/dev/null && cmake --build build-tsan -j8 2>&1 | grep -E "warning|error" ; (cd build-tsan && ctest 2>&1 | grep -E "tests passed|tests failed|Failed")
echo "== compilación del esqueleto cuFile (sólo compila; GDS: $(ls /dev/nvidia-fs* 2>/dev/null || echo 'sin /dev/nvidia-fs'))"
cmake -S . -B build-cufile -DCMAKE_BUILD_TYPE=Release -DTIERMEM_WITH_CUFILE=ON -DCMAKE_CXX_FLAGS="-I/usr/local/cuda/include" >/dev/null 2>&1
cmake --build build-cufile -j8 --target tiermem 2>&1 | grep -E "warning|error|Built target"; echo "cufile build rc=${PIPESTATUS[0]}"
echo "== binario GPU (nvcc $(/usr/local/cuda/bin/nvcc --version | tail -2 | head -1))"
/usr/local/cuda/bin/nvcc -O2 -std=c++20 -arch=sm_86 -Iinclude valor/valor_gpu.cu build/libtiermem.a -lpthread -o build/valor_gpu 2>&1 | grep -v "^$" ; echo "nvcc rc=${PIPESTATUS[0]}"
g++ -O2 -std=c++20 valor/gen_file.cpp -o build/gen_file && echo "gen_file ok"
echo "== FIN fase 1 $(date '+%T')"
