# 05 · Memoria unificada por software (tiermem)

Gestor de memoria por niveles en C++20 para Linux. Reserva un rango virtual por cada
tensor (archivo en disco) y atiende los **fallos de página con `userfaultfd`**: cuando
el programa toca una dirección que no está cargada, un hilo manejador lee ese bloque del
disco y lo instala con `UFFDIO_COPY`. El programa consumidor sólo ve un puntero normal.

## El objetivo original y por qué no se hizo tal cual

El pedido era: *interceptar los fallos de página de la GPU y paginar tensores desde NVMe en
RAID 0 con GPUDirect Storage, sin pasar por la CPU.* Eso **no es posible tal cual**, por dos
motivos:

1. **El driver de NVIDIA (UVM) no expone un manejador de fallos de GPU reemplazable.** Los
   fallos de la GPU sobre memoria administrada los resuelve el módulo `nvidia-uvm` del kernel;
   no hay API pública para que un programa los atienda (no existe un "userfaultfd de GPU").
2. **En esta máquina no hay GPU ni driver NVIDIA ni CUDA.** Nada de la parte GPU se pudo
   compilar ni probar acá.

Lo que sí se construyó es la parte verificable con la misma arquitectura, del lado CPU:

| Nivel | En este repo | En una GPU real (futuro, NO verificado) |
|---|---|---|
| "dispositivo" | páginas residentes dentro del rango virtual, con **tope de bytes** (emula la VRAM) | memoria `cudaMalloc` |
| host | caché **LRU** en RAM de bloques expulsados o leídos por adelantado | RAM pinned |
| almacenamiento | el archivo, leído con `pread` + `O_DIRECT` o **io_uring** (liburing) | `cuFileRead` (GDS) directo a VRAM |

En una GPU real, como no se pueden interceptar sus fallos, el diseño correcto sería
**residencia explícita**: llamar `TierManager::prefetch()` sobre los rangos que usará el
kernel *antes* de lanzarlo. Esa API ya existe y está probada en CPU (`prefetch_explicito`).

## Cómo funciona

- `map_tensor(ruta)` reserva un rango `PROT_READ` (solo lectura) y lo registra con
  `UFFDIO_REGISTER_MODE_MISSING`.
- Un hilo manejador hace `poll` sobre el `userfaultfd`. Por cada fallo:
  1. si el bloque ya está residente (fallo duplicado de otro hilo) → `UFFDIO_WAKE`;
  2. si está en el nivel host → lo copia al rango (`UFFDIO_COPY`);
  3. si no, lo lee del archivo **en un único lote** junto con los siguientes
     `prefetch_chunks` bloques (sólo si el acceso viene siendo secuencial).
- Antes de instalar, si el nivel dispositivo superaría su tope, expulsa con **CLOCK
  (segunda oportunidad)**: degrada el bloque a host y hace `MADV_DONTNEED`. El próximo acceso
  vuelve a fallar y se atiende desde host.
- El bit de referencia de CLOCK se marca en cada fallo/instalación. **Limitación:** desde
  espacio de usuario no se ve el bit "accedido" del hardware, así que los accesos a páginas
  ya residentes no cuentan. Es una aproximación a LRU, no un LRU exacto.
- El nivel host es un LRU exacto (todos sus accesos pasan por el manejador).
- Backend enchufable (`include/tiermem/backend.hpp`): `pread+O_DIRECT`, `io_uring+O_DIRECT`
  y el esqueleto `src/backend_cufile.cpp` (**nunca compilado**, opción CMake
  `TIERMEM_WITH_CUFILE=OFF`).

## userfaultfd sin privilegios

En esta máquina `vm.unprivileged_userfaultfd=0` y `/dev/userfaultfd` es `root:root 0600`.
La biblioteca intenta, en orden:

1. `userfaultfd()` completo (funciona como root o con `vm.unprivileged_userfaultfd=1`);
2. `/dev/userfaultfd` + `USERFAULTFD_IOC_NEW` (si el usuario tiene permiso sobre el nodo);
3. `UFFD_USER_MODE_ONLY` (Linux ≥ 5.11): **funciona sin privilegios**, y es el modo que
   se usó en todas las pruebas como usuario normal.

Consecuencia de `UFFD_USER_MODE_ONLY`: sólo se atienden fallos producidos en modo usuario.
Si se pasa un puntero del tensor **no residente** a un syscall (por ejemplo
`write(fd, tensor, n)`), el kernel no espera al manejador y el syscall devuelve **`EFAULT`**.
La prueba `syscall_sobre_rango` lo comprueba, y también que tocar la página antes (o usar
`prefetch`) lo resuelve. Como root (modo completo) el mismo `write` funciona; está
verificado en `VERIFICACION.md`. Para usar el modo completo sin root:
`sudo sysctl vm.unprivileged_userfaultfd=1`, o dar acceso al usuario a `/dev/userfaultfd`.

## Compilar y probar

Requisitos: g++ ≥ 10 (C++20), CMake ≥ 3.20, Linux ≥ 5.11; opcional `liburing-dev`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cd build && ctest                    # 15 pruebas
./tiermem_bench bench.bin 1024       # crea un archivo de 1 GiB y mide
cd .. && ./verificar.sh              # todo: 3 corridas, ASan+UBSan, TSan, root
```

Todas las compilaciones usan `-Wall -Wextra -Wpedantic -Werror`.
Sanitizers: `-DTIERMEM_SANITIZE=ON` (ASan+UBSan) y `-DTIERMEM_TSAN=ON` (ThreadSanitizer).

## Pruebas (15, en `tests/tests.cpp`)

| Prueba | Qué verifica |
|---|---|
| `backend_pread`, `backend_iouring` | cada backend lee exactamente el contenido del archivo, incluida una cola no alineada |
| `fault_secuencial` | 16 MiB leídos byte a byte a través de fallos == contenido del archivo |
| `fault_aleatorio` | 20 000 lecturas aleatorias con tope de 1 MiB sobre 24 MiB |
| `cola_no_multiplo` | archivo de tamaño no múltiplo del bloque; el relleno es cero |
| `expulsion_bajo_presion` | 32 MiB con tope de 1 MiB: datos correctos y **residencia real según `mincore()` ≤ tope** |
| `nivel_host` | la segunda pasada se sirve entera desde host, sin tocar el disco |
| `prefetch_secuencial` | la lectura anticipada evita la mayoría de las lecturas al disco |
| `prefetch_explicito` | tras `prefetch()` el rango está residente y no hay fallos |
| `multihilo` | 8 hilos × 100 000 lecturas aleatorias, sin presión |
| `multihilo_presion` | 8 hilos con `memcpy` de 4 KiB cruzando páginas, con expulsiones concurrentes |
| `varios_tensores` | dos tensores comparten el mismo tope de dispositivo |
| `solo_lectura_sigsegv` | escribir en el tensor produce `SIGSEGV` (proceso hijo) |
| `syscall_sobre_rango` | comportamiento documentado de `EFAULT` en `UFFD_USER_MODE_ONLY` |
| `sin_nivel_host` | funciona con nivel host = 0 |

Resultado medido el 20/10/2026 (UTC-3): 15/15 en 3 corridas seguidas (Release), 15/15 con
ASan+UBSan, 15/15 con TSan, sin reportes de los sanitizers. Ver `VERIFICACION.md`.

## Números medidos en esta máquina

Máquina: VM con 8 vCPU Intel Xeon, kernel 6.12.94, disco virtio `ext4` (no es un NVMe
físico ni un RAID 0). Archivo de 1 GiB, nivel dispositivo 64 MiB, host 256 MiB (2 GiB en
las filas "host"). Modo `UFFD_USER_MODE_ONLY`. Dos corridas del mismo binario:

| backend | bloque | patrón | origen | prefetch | hilos | fallos/s (corrida 1 / 2) | GB/s (1 / 2) |
|---|---|---|---|---|---|---|---|
| pread | 4 KiB | secuencial | disco | 0 | 1 | 21 677 / 25 972 | 0,089 / 0,106 |
| pread | 64 KiB | secuencial | disco | 0 | 1 | 7 371 / 8 678 | 0,483 / 0,569 |
| pread | 64 KiB | secuencial | disco | 8 | 1 | 8 237 / 9 934 | 0,540 / 0,651 |
| io_uring | 64 KiB | secuencial | disco | 8 | 1 | 10 557 / 12 405 | 0,692 / 0,813 |
| pread | 1 MiB | secuencial | disco | 0 | 1 | 791 / 943 | 0,829 / 0,989 |
| io_uring | 1 MiB | secuencial | disco | 4 | 1 | 896 / 1 156 | 0,939 / 1,212 |
| pread | 4 KiB | aleatorio | disco | 0 | 1 | 28 005 / 27 726 | 0,115 / 0,114 |
| io_uring | 4 KiB | aleatorio | disco | 0 | 1 | 27 783 / 26 172 | 0,114 / 0,107 |
| pread | 64 KiB | aleatorio | disco | 0 | 1 | 8 832 / 10 035 | 0,579 / 0,658 |
| pread | 64 KiB | aleatorio | disco | 0 | 8 | 10 907 / 11 911 | 0,715 / 0,780 |
| pread | 4 KiB | aleatorio | host | 0 | 1 | 61 274 / 58 600 | 0,251 / 0,240 |
| pread | 64 KiB | aleatorio | host | 0 | 1 | 27 831 / 24 372 | 1,824 / 1,597 |
| pread | 64 KiB | aleatorio | host | 0 | 8 | 20 000 / 17 323 | 1,311 / 1,135 |

Referencia sin userfaultfd (`pread` con `O_DIRECT` directo, 1 hilo), corrida 1 / 2:
secuencial 64 KiB 1,312 / 1,146 GB/s · secuencial 1 MiB 1,871 / 1,730 GB/s ·
aleatorio 4 KiB 0,226 / 0,222 GB/s · aleatorio 64 KiB 1,259 / 1,172 GB/s.

Lectura honesta de los números:
- El mejor caso desde disco (io_uring, bloques de 1 MiB, prefetch 4) llega a **~0,94–1,21 GB/s**,
  o sea ~50–70 % del `pread` directo de 1 MiB. El resto es el costo de la copia a host, `UFFDIO_COPY`
  y la expulsión.
- Con bloques de 4 KiB el costo fijo por fallo domina: unos **26 000–28 000 fallos/s**
  desde disco y **~60 000 fallos/s** desde host. Esa cifra es el techo del manejador actual.
- **No escala con hilos:** hay un único hilo manejador, y con 8 hilos consumidores desde host
  rinde *menos* que con 1. Paralelizar el manejador está pendiente.
- La variación entre corridas fue de hasta ~20 %. El disco es virtual y el host de la VM
  puede tener caché propia, así que estos números **no** representan un NVMe físico.

## Qué NO está verificado

- **GPU real, VRAM, CUDA:** no hay GPU en esta máquina. Nada se ejecutó en una GPU.
- **GPUDirect Storage / cuFile:** `src/backend_cufile.cpp` es un esqueleto **que nunca se
  compiló**. Puede tener errores de compilación.
- **Interceptar fallos de página de la GPU:** no se hizo y no se puede con el driver público
  de NVIDIA (ver arriba).
- **NVMe físico y RAID 0:** sólo se midió un disco virtio de una VM. No hay código especial
  para RAID 0: un RAID 0 de `mdadm` se ve como un solo dispositivo de bloques y debería
  funcionar igual, pero **no se midió**.
- **Escrituras:** los tensores son de solo lectura por diseño. No hay escritura de vuelta
  al disco.
- **Errores de E/S durante un fallo:** si la lectura del disco falla dentro del manejador, el
  proceso termina (`std::terminate`). No hay recuperación.
- **Memoria enorme:** se probó con hasta 1 GiB por tensor y 2 tensores a la vez.
- **Rendimiento con varios hilos manejadores:** no existe todavía.

## Qué probar en la máquina de Federico

1. `./verificar.sh` completo (compila, 3 corridas, sanitizers).
2. `./build/tiermem_bench /ruta/en/el/nvme/bench.bin 4096` sobre el NVMe/RAID 0 real,
   comparando con `fio --direct=1` en el mismo disco.
3. Con GPU NVIDIA + CUDA + GDS (`nvidia-fs`): compilar con `-DTIERMEM_WITH_CUFILE=ON`,
   arreglar lo que no compile y escribir una prueba que lea con `cuFileRead` a VRAM y compare
   contra el archivo. Recién ahí el backend GDS pasaría a "verificado".
4. Si se quiere el modo completo de userfaultfd: `sudo sysctl vm.unprivileged_userfaultfd=1`
   y volver a correr `syscall_sobre_rango` como usuario.
