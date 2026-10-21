#!/usr/bin/env bash
# Reproduce toda la verificación de este repositorio. Uso: ./verificar.sh
set -euo pipefail
cd "$(dirname "$0")"
echo "== entorno"; uname -r; g++ --version | head -1; cmake --version | head -1
echo "vm.unprivileged_userfaultfd=$(cat /proc/sys/vm/unprivileged_userfaultfd 2>/dev/null || echo '?')"
ls -l /dev/userfaultfd 2>/dev/null || echo "/dev/userfaultfd ausente"

echo "== compilación Release (-Wall -Wextra -Wpedantic -Werror)"
rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" 2>&1 | grep -E "warning|error|Built target" || true

for i in 1 2 3; do
  echo "== ctest corrida $i"
  (cd build && ctest 2>&1 | grep -E "tests passed|tests failed|Total Test time")
done

echo "== ASan + UBSan"
rm -rf build-asan && cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DTIERMEM_SANITIZE=ON >/dev/null
cmake --build build-asan -j"$(nproc)" 2>&1 | grep -E "warning|error" || true
(cd build-asan && ctest 2>&1 | grep -E "tests passed|tests failed|Total Test time")

echo "== ThreadSanitizer"
rm -rf build-tsan && cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DTIERMEM_TSAN=ON >/dev/null
cmake --build build-tsan -j"$(nproc)" 2>&1 | grep -E "warning|error" || true
(cd build-tsan && ctest 2>&1 | grep -E "tests passed|tests failed|Total Test time")

if sudo -n true 2>/dev/null; then
  echo "== como root (modo userfaultfd completo)"
  (cd build && sudo -n ./tiermem_tests syscall_sobre_rango && sudo -n ./tiermem_tests all | grep -c '^OK' && sudo -n rm -f t_*.bin)
fi
echo "== FIN"
