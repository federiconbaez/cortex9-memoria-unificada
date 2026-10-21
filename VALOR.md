# Experimento de valor del sistema 5 (tiermem) en hardware real

Fecha: 21/10/2026, 00:05–00:31 (UTC-3). Máquina `host-de-pruebas`, usuario `usuario-de-pruebas` (sin sudo).
AMD Ryzen 7 5700G, 32 GB RAM, RTX 3060 12 GB (driver 580.167.08, nvcc 12.9), kernel [redactado],
disco SATA ADATA SU630 (ext4, 97 % lleno), swap `/swap.img` 8 GiB. **GDS: NO** (no hay `/dev/nvidia-fs`).
La máquina estaba en uso por su dueño durante las corridas (GPU 25–37 % ocupada).

## Pregunta y umbral (fijado por Research)
¿Sirve tiermem para alimentar una carga GPU con un conjunto de trabajo mayor que la memoria
disponible? **Pasa sólo si su p99 de latencia es ≤ 0,50× el del swap del sistema operativo bajo la
misma carga.**

## Diseño (`valor/valor_gpu.cu`, `valor/fase2_valor.sh`)
- Conjunto de trabajo W = 1536 MiB; presupuesto de RAM = 256 MiB. Cada corrida va en su propio
  scope `systemd-run --user` con `MemoryMax = 303M` (25 MiB del contexto CUDA + 256 + holgura);
  delegación de cgroup v2 de usuario, sin root, sin tocar la configuración de swap.
- Carga GPU idéntica para todos: por acceso se elige un bloque de 1 MiB, la CPU lo copia a un buffer
  pinned (ahí ocurren los fallos), `cudaMemcpyAsync` a la GPU, un kernel suma sus bytes y se sincroniza.
  Se mide la latencia de cada acceso. Cada acceso se verifica (suma GPU == suma CPU y 3 bytes ==
  contenido esperado): **0 errores en todas las corridas**.
- Métodos (fuente del bloque):
  - **swap** (línea base principal): memoria anónima de W bytes; el kernel la manda a swap (1548 MiB en swap por corrida).
  - **mmap** (secundaria): el archivo mapeado; el kernel pagina con el page cache.
  - **tiermem**: `TierManager`, bloque 1 MiB, nivel dispositivo 128 MiB + host 128 MiB, `pread+O_DIRECT`,
    `UFFD_USER_MODE_ONLY` (liburing no está instalado en host-de-pruebas).
  - **explicit** (cota superior): `pread` O_DIRECT directo al buffer pinned, paginación escrita a mano.
- Patrón aleatorio: 600 accesos, 5 corridas (semillas 101–105). Patrón secuencial: una pasada de
  1536 accesos, **sólo 2 corridas**: el swap secuencial tardaba 90–125 s por corrida y se cortó para no
  ocupar la máquina de Federico.

## Resultados (mediana entre corridas)
| patrón | método | n | p50 ms (mediana) | p99 ms (mediana) | p99 ms (rango) | GB/s (mediana) | errores |
|---|---|---|---|---|---|---|---|
| aleat | swap | 5 | 60.2 | 809.6 | 141.7–1009.0 | 0.012 | 0 |
| aleat | tiermem | 5 | 13.6 | 26.5 | 23.1–41.0 | 0.082 | 0 |
| aleat | mmap | 5 | 35.9 | 100.2 | 58.4–106.2 | 0.026 | 0 |
| aleat | explicit | 5 | 10.8 | 26.1 | 20.5–31.3 | 0.094 | 0 |
| secu | swap | 2 | 59.4 | 212.1 | 147.4–276.9 | 0.015 | 0 |
| secu | tiermem | 2 | 0.7 | 77.2 | 71.9–82.4 | 0.096 | 0 |
| secu | mmap | 2 | 36.9 | 73.0 | 70.4–75.7 | 0.029 | 0 |
| secu | explicit | 2 | 4.5 | 22.0 | 16.9–27.1 | 0.158 | 0 |

aleat: p99 tiermem / p99 swap = 26.5/809.6 = 0.033x (umbral <= 0.50x) -> PASA

secu: p99 tiermem / p99 swap = 77.2/212.1 = 0.364x (umbral <= 0.50x) -> PASA

## Veredicto (sin maquillar)
- **Contra el swap del SO, PASA con margen.** Aleatorio: p99 26,5 ms contra 809,6 ms (0,033×).
  Aunque se compare el peor p99 de tiermem (41,0 ms) con el mejor del swap (141,7 ms), da 0,29×.
  El rendimiento es ~7× mayor (0,082 contra 0,012 GB/s). Secuencial (n=2): 0,36×.
- **Contra mmap/page cache:** gana en aleatorio (p99 0,26×, ~3× rendimiento). En secuencial empata el
  p99 (77 contra 73 ms), aunque el p50 es mucho menor (0,7 contra 37 ms, gracias al prefetch).
- **Contra paginación explícita a mano (`explicit`), NO gana:** p99 igual (26,5 contra 26,1 ms) y
  rendimiento algo menor. tiermem aporta *transparencia* (un puntero normal en vez de reescribir el
  código con `pread`) con un costo pequeño, pero no aporta velocidad sobre un programador que pagina a mano.
- El GPU es el consumidor, pero los fallos los atiende la CPU: tiermem no intercepta fallos de la
  GPU (imposible con el driver público, ver README). `cudaMallocManaged` no se midió: con W < 12 GB de
  VRAM no hay sobresuscripción y la comparación no tendría sentido; sobresuscribir pediría un archivo
  ≥ 12 GiB en un disco con 20 GB libres.
- Advertencias: n=5 (aleatorio) y n=2 (secuencial); máquina compartida y en uso, disco SATA casi lleno;
  el p99 del swap varió entre 142 y 1009 ms. La conclusión aguanta incluso con el peor caso de tiermem.

## Nivel A en hardware real (`valor/fase1.log`, `valor/diag.log`)
- Release (`-Werror`, gcc 13.3, cmake 3.28): ctest **15/15 ×3**; `fault_secuencial` **20/20**.
- ASan+UBSan: **15/15**.
- TSan: con ASLR por defecto **falla en el arranque** (14/15, `FATAL: ThreadSanitizer: unexpected
  memory mapping`, incompatibilidad conocida de TSan con la entropía de ASLR de kernels recientes; no es
  un fallo del código). Con `setarch -R` (sin root): **15/15 ×3** y **0** advertencias de TSan en `all`.
- Esqueleto cuFile: **sigue sin compilar**. `-DTIERMEM_WITH_CUFILE=ON` falló en la configuración de
  CMake (no busca `cufile.h` en `/usr/local/cuda/include`); no se reintentó. Sin `nvidia-fs`, GDS no aplica.

## Reproducir
```bash
bash valor/fase1_build_tests.sh          # sondeo + compilación + pruebas + nvcc
bash valor/fase2_valor.sh 1536 256 5     # experimento (≈45 min con el patrón secuencial)
python3 valor/resumen.py                 # tabla de medianas
```
