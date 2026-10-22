> Nota (2026-10-22): el historial se reescribió y los hashes citados aquí son anteriores; ver [docs/HASHES.md](docs/HASHES.md). / History was rewritten; hashes cited here are pre-rewrite, see docs/HASHES.md.

# Auditoría del experimento de valor del sistema 5 (tiermem) — Research Sentinel

Fecha: 21/10/2026 (UTC-3). Auditoría hecha sólo en la box, sobre lo versionado. No se ejecutó nada en `host-de-pruebas`.

## 1. Disponibilidad e integridad
- Commit `2d0208d` presente en `/workspace/sistemas/05-memoria-unificada`, padre `023da41f…`. Árbol limpio.
- `git archive --format=tar.gz HEAD | sha256sum` = `68b732deb2f4f98732382f08e37c1f480ae441aec18a913bf8020ad89dcbf085` → **coincide** con `68b732de…f085`.

## 2. Recálculo (`auditoria/recalc_valor.py`, salida en `auditoria/recalc_valor.out`)
**Limitación:** `valor/` **no tiene latencias por acceso**. Los "logs crudos" son líneas `RES` por corrida
(p50/p99/max ya calculados por `valor_gpu.cu`). No pude recalcular el p99 de cada corrida desde los datos crudos,
sólo el agregado entre corridas. Revisé el cálculo del percentil en el código (`pct(0.99)` = índice
`round(0.99·(N−1))`, con N=600 es el 7.º valor más alto): es correcto.

| patrón | método | p99 por corrida (ms) | mediana | claim |
|---|---|---|---|---|
| aleat | swap | 141,7 · 263,4 · 844,6 · 1009,0 · 809,6 | 809,6 | 809,6 ✔ |
| aleat | tiermem | 26,5 · 23,1 · 41,0 · 33,9 · 24,6 | 26,5 | 26,5 ✔ |
| aleat | mmap | 106,2 · 58,4 · 102,8 · 100,2 · 67,8 | 100,2 | 100,2 ✔ |
| aleat | explicit | 26,1 · 20,5 · 25,2 · 26,2 · 31,3 | 26,1 | 26,1 ✔ |
| secu | swap | 147,4 · 276,9 | 212,1 | ✔ |
| secu | tiermem | 82,4 · 71,9 | 77,2 | ✔ |

- Aleatorio: ratio de medianas 0,0327× ✔. **Pareado por corrida:** 0,187 · 0,088 · 0,049 · 0,034 · 0,030 (todas ≤ 0,50).
  Peor tiermem / mejor swap = 41,0/141,7 = **0,289×** ✔. Bootstrap (100k, remuestreo independiente de las 5 corridas
  de cada método, ratio de medianas): IC95 % **0,026–0,187**, P(ratio > 0,5) = 0.
- Secuencial (n=2): ratio de medianas 0,364× ✔, pero **pareado 0,559 y 0,260**: la corrida 1 **no pasa** (0,559 > 0,50).
  Peor/mejor = 0,559×. Con n=2 el secuencial **no es robusto**; no sirve como evidencia de pase.
- 0 errores de verificación en las 26 corridas ✔.

## 3. Justicia de la línea base swap
- **Swap activo:** sí. `fase1.log` muestra `/proc/swaps` con `/swap.img` (archivo, 8 GiB, usado 0) y el scope de prueba con
  `memory.swap.max=max`. Cada corrida swap termina con `memory.swap.current` = 1548 MiB (≈ W + 12 MiB del contexto),
  o sea que la memoria anónima realmente se pagina a swap y no hay OOM. **zswap/zram: no registrado** (no se leyó
  `/sys/module/zswap/parameters/enabled`); `/proc/swaps` no muestra zram.
- **Misma carga y verificación:** sí. Mismo binario y mismo bucle (`memcpy` a pinned → `cudaMemcpyAsync` → kernel de suma →
  sync), mismo contenido (splitmix semilla 42), misma verificación, mismo cronómetro. La única diferencia es la fuente del bloque.
  La memoria anónima se llena (y se fuerza a swap) **antes** de `T0`; el llenado no se cronometra.
- **Mismo MemoryMax:** sí, los cuatro métodos corren en su propio scope con `MemoryMax=303M`, `MemorySwapMax=infinity`.
  Los niveles de tiermem viven en RAM del host (nivel "dispositivo" = páginas anónimas vía `UFFDIO_COPY`, 128 MiB; nivel host =
  `aligned_alloc`, 128 MiB), así que se cuentan en el cgroup. tiermem y explicit leen con `O_DIRECT`: no usan page cache.
  **No se registró `memory.peak`, `memory.current` ni `memory.events`** en la medición; la única evidencia indirecta es
  `swap.current=0` en tiermem/explicit (contra 12 MiB en mmap), compatible con que no llegaron a presión de reclaim. Es indirecta.
- **Orden:** no aleatorizado. Siempre swap → tiermem → mmap → explicit dentro de cada repetición. No hay efecto de page cache
  caliente que favorezca a tiermem (O_DIRECT; mmap hace `FADV_DONTNEED`). Sí puede haber un efecto en contra del swap: es el
  único método que **escribe** ~1,3 GiB al disco justo antes de medir, y parte de esa escritura puede seguir en curso al arrancar.
- **Varianza del swap (141–1009 ms):** hay un escalón entre las dos tandas: reps 1–2 (00:11) dan 141,7 y 263,4 ms, y reps 3–5
  (00:22–00:31) dan 844,6, 1009,0 y 809,6 ms. `total_s` del swap crece monótonamente (27,6 → 60,6 s). tiermem sube menos
  (23–26 → 25–41 ms). Los logs no la explican: no hay carga de GPU/CPU por corrida (sólo una lectura al inicio) ni estado del
  disco. Hipótesis no verificada: disco SATA DRAM-less al 97 % que se degrada con las escrituras repetidas del swap, más la
  carga del dueño. El pase no depende de esto: con la mejor corrida del swap (141,7) el peor tiermem da 0,29×.
- **Ventaja estructural (legítima):** el swap atiende fallos de 4 KiB con readahead chico, mientras que tiermem lee el bloque de
  1 MiB en una sola E/S `O_DIRECT`. Es justamente lo que se quiere medir contra "el swap del SO" por defecto, sin tunear.

## 4. Reproducibilidad
- Las reps 3–5 **no salen del script versionado**: el encabezado ("reps 3-5 sólo aleat") y las líneas `# tiermem uffd=…` (que el
  `grep` de `fase2_valor.sh` filtraría) muestran que se usó otro comando, que no está commiteado. Las semillas de las reps 3–5
  no quedan registradas (la línea `RES` no imprime la semilla). El archivo de las reps 1–2 no tiene `== FIN` (corte manual, como se declaró).
- En la box (sin GPU): build Release limpio desde `git archive HEAD`, **ctest 15/15**. No compilé `valor_gpu.cu` (no hay nvcc/GPU).

## 5. Veredicto
- **Umbral #5: PASA** en acceso aleatorio, n=5, de forma robusta (todas las parejas ≤ 0,19×, peor/mejor 0,29×, IC95 % bootstrap ≤ 0,19×).
- Secuencial: pasa en mediana pero no es robusto (una de dos corridas da 0,56×). No suma evidencia.
- Etiqueta «pasa contra el swap, no le gana al paginado explícito»: **correcta**. El umbral no pedía ganarle a `pread`.
- **Nivel: B+** (valor demostrado en hardware real contra la línea base fijada, con margen, en el patrón principal).
  No llega más arriba por: n chico, sin latencias crudas por acceso, reps 3–5 con un comando no versionado, sin `memory.peak`,
  zswap no registrado, orden no aleatorizado, máquina compartida, sin GDS y sin `cudaMallocManaged`.

## Caveats para una remedición
Guardar las latencias por acceso, imprimir la semilla, registrar `memory.peak`/`memory.events` y el estado de zswap, aleatorizar
el orden de los métodos, dejar asentar la escritura del swap (o `sync`) antes de `T0`, y versionar el comando exacto de cada tanda.
