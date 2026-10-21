# VERIFICACIÓN — 05-memoria-unificada

> **Importante:** las secciones 1 a 4 corresponden al commit `7c08313`, que tenía un error de orden en el manejador de fallos. El error, su causa y la verificación del arreglo bajo carga están en la **sección 5**.

Fecha: 20/10/2026 (hora de Buenos Aires, UTC-3). Máquina: VM Debian 13, kernel 6.12.94+, 8 vCPU Intel Xeon, 15 GB RAM, sin GPU.

## 1. Script completo: `./verificar.sh`

Compila en Release con `-Wall -Wextra -Wpedantic -Werror`, corre `ctest` 3 veces seguidas, luego ASan+UBSan, ThreadSanitizer y, si hay `sudo -n`, la prueba de syscalls como root.

```
== entorno
6.12.94+
g++ (Debian 14.2.0-19) 14.2.0
cmake version 3.31.6
vm.unprivileged_userfaultfd=0
crw------- 1 root root 10, 257 Oct 14 23:50 /dev/userfaultfd
== compilación Release (-Wall -Wextra -Wpedantic -Werror)
[ 42%] Built target tiermem
[ 85%] Built target tiermem_bench
[100%] Built target tiermem_tests
== ctest corrida 1
100% tests passed, 0 tests failed out of 15
Total Test time (real) =   6.04 sec
== ctest corrida 2
100% tests passed, 0 tests failed out of 15
Total Test time (real) =   6.32 sec
== ctest corrida 3
100% tests passed, 0 tests failed out of 15
Total Test time (real) =   6.30 sec
== ASan + UBSan
100% tests passed, 0 tests failed out of 15
Total Test time (real) =   8.87 sec
== ThreadSanitizer
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  15.99 sec
== como root (modo userfaultfd completo)
modo=completo (syscall userfaultfd) write()=4096 errno=0
OK   syscall_sobre_rango
15
== FIN
```

## 2. Detalle de cada prueba (con estadísticas internas)

```
$ cd build && ./tiermem_tests all
backend: io_uring+O_DIRECT
OK   backend_iouring
backend: pread+O_DIRECT
OK   backend_pread
OK   cola_no_multiplo
residente segun mincore: 1048576 bytes (tope 1048576)
residente segun mincore: 1048576 bytes (tope 1048576)
[expulsion_bajo_presion] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=1024 resid=0 host=816 storage=208 prefetched=816 pf_hits=816 dev_ev=1008 host_ev=960 dev_peak=1048576
OK   expulsion_bajo_presion
[fault_aleatorio] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=19156 resid=0 host=2530 storage=16626 prefetched=134 pf_hits=17 dev_ev=19140 host_ev=16696 dev_peak=1048576
OK   fault_aleatorio
[fault_secuencial] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=256 resid=0 host=204 storage=52 prefetched=204 pf_hits=204 dev_ev=0 host_ev=0 dev_peak=16777216
OK   fault_secuencial
[multihilo] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=531 resid=19 host=0 storage=512 prefetched=0 pf_hits=0 dev_ev=0 host_ev=0 dev_peak=33554432
OK   multihilo
[multihilo_presion] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=41036 resid=234 host=3699 storage=37103 prefetched=258 pf_hits=26 dev_ev=40786 host_ev=37297 dev_peak=1048576
OK   multihilo_presion
[nivel_host] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=96 resid=0 host=48 storage=48 prefetched=0 pf_hits=0 dev_ev=80 host_ev=0 dev_peak=1048576
OK   nivel_host
OK   prefetch_explicito
[prefetch_secuencial] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=128 resid=0 host=112 storage=16 prefetched=112 pf_hits=112 dev_ev=112 host_ev=64 dev_peak=1048576
OK   prefetch_secuencial
OK   sin_nivel_host
OK   solo_lectura_sigsegv
modo=UFFD_USER_MODE_ONLY (sin privilegios) write()=-1 errno=14
OK   syscall_sobre_rango
[varios_tensores] backend=io_uring+O_DIRECT uffd=UFFD_USER_MODE_ONLY (sin privilegios) faults=384 resid=0 host=304 storage=80 prefetched=304 pf_hits=304 dev_ev=368 host_ev=320 dev_peak=1048576
OK   varios_tensores
```

Notas: `resid` = fallos duplicados de otro hilo sobre un bloque ya residente. `dev_peak` nunca supera el tope configurado (1 MiB = 1048576 en las pruebas con presión), y `mincore()` lo confirma desde el kernel.

## 3. Benchmark: `./build/tiermem_bench bench.bin 1024` (dos corridas)

### Corrida 1
```
creando bench.bin (1024 MiB)...
hilos hardware: 8  liburing compilado: si
modo userfaultfd: UFFD_USER_MODE_ONLY (sin privilegios)

backend    bloque  patron origen    pf   hil     fallos     fallos/s      GB/s       seg
pread      4096    secu   nvme      0    1       262144        21677     0.089    12.093
pread      65536   secu   nvme      0    1        16384         7371     0.483     2.223
pread      65536   secu   nvme      8    1        16384         8237     0.540     1.989
io_uring   65536   secu   nvme      8    1        16384        10557     0.692     1.552
pread      1048576 secu   nvme      0    1         1024          791     0.829     1.295
io_uring   1048576 secu   nvme      4    1         1024          896     0.939     1.143
pread      4096    aleat  nvme      0    1        94345        28005     0.115     3.369
io_uring   4096    aleat  nvme      0    1        94345        27783     0.114     3.396
pread      65536   aleat  nvme      0    1        15402         8832     0.579     1.744
pread      65536   aleat  nvme      0    8        15328        10907     0.715     1.405
pread      4096    aleat  host      0    1        93830        61274     0.251     1.531
pread      65536   aleat  host      0    1        15369        27831     1.824     0.552
pread      65536   aleat  host      0    8        15312        20000     1.311     0.766

Referencia sin userfaultfd (pread O_DIRECT directo, 1 hilo):
  secuencial 64 KiB: 1.312 GB/s
  secuencial 1 MiB:  1.871 GB/s
  aleatorio 4 KiB:   0.226 GB/s
  aleatorio 64 KiB:  1.259 GB/s
```

### Corrida 2
```
hilos hardware: 8  liburing compilado: si
modo userfaultfd: UFFD_USER_MODE_ONLY (sin privilegios)

backend    bloque  patron origen    pf   hil     fallos     fallos/s      GB/s       seg
pread      4096    secu   nvme      0    1       262144        25972     0.106    10.093
pread      65536   secu   nvme      0    1        16384         8678     0.569     1.888
pread      65536   secu   nvme      8    1        16384         9934     0.651     1.649
io_uring   65536   secu   nvme      8    1        16384        12405     0.813     1.321
pread      1048576 secu   nvme      0    1         1024          943     0.989     1.086
io_uring   1048576 secu   nvme      4    1         1024         1156     1.212     0.886
pread      4096    aleat  nvme      0    1        94345        27726     0.114     3.403
io_uring   4096    aleat  nvme      0    1        94345        26172     0.107     3.605
pread      65536   aleat  nvme      0    1        15402        10035     0.658     1.535
pread      65536   aleat  nvme      0    8        15323        11911     0.780     1.286
pread      4096    aleat  host      0    1        93830        58600     0.240     1.601
pread      65536   aleat  host      0    1        15369        24372     1.597     0.631
pread      65536   aleat  host      0    8        15294        17323     1.135     0.883

Referencia sin userfaultfd (pread O_DIRECT directo, 1 hilo):
  secuencial 64 KiB: 1.146 GB/s
  secuencial 1 MiB:  1.730 GB/s
  aleatorio 4 KiB:   0.222 GB/s
  aleatorio 64 KiB:  1.172 GB/s
```

## 4. Comprobaciones del entorno hechas a mano

```
$ cat /proc/sys/vm/unprivileged_userfaultfd
0
$ ls -l /dev/userfaultfd
crw------- 1 root root 10, 257 Oct 14 23:50 /dev/userfaultfd
# programa C mínimo: userfaultfd(O_CLOEXEC|O_NONBLOCK) como usuario
plain: -1 Operation not permitted
usermode (UFFD_USER_MODE_ONLY): ok
$ cat /proc/sys/kernel/io_uring_disabled
0
$ nvidia-smi
nvidia-smi: command not found
$ ls /dev/nvidia*
ls: cannot access '/dev/nvidia*': No such file or directory
```

## 5. Incidente: `fault_secuencial` fallaba bajo carga (21/10/2026, UTC-3)

### Qué pasó

Una verificación independiente del commit `7c08313` falló: `./verificar.sh` salió con código 8, y en 6 corridas de `ctest` la prueba `fault_secuencial` falló 2 veces. La máquina compartida estaba muy cargada por compilaciones de otras agentes.

Lo reproduje con 8 bucles ocupados en paralelo: falló **4 de 30** corridas aisladas. Siempre se veía la misma salida:

```
FALLO tests/tests.cpp:105: s.served_from_storage + s.served_from_host == 256
[fault_secuencial] ... faults=256 resid=0 host=203 storage=52 ... dev_ev=0 host_ev=0 dev_peak=16711680
```

### Causa raíz: error real en el manejador, no en la prueba

Se atendieron 256 fallos y los 16 MiB se verificaron byte por byte (los datos eran correctos), pero las estadísticas contaban 255 bloques servidos y `dev_peak` era un bloque menos (16711680 = 255 × 64 KiB).

El manejador instalaba el bloque con `UFFDIO_COPY` en modo normal, y ese ioctl **despierta al hilo que falló dentro de la misma llamada**. Recién después actualizaba la contabilidad (`served_from_*`, `device_peak`, inserción en el nivel host, contadores de prefetch). Bajo carga, el hilo despertado terminaba de leer y llamaba `stats()` antes de que el hilo manejador volviera a ejecutarse, así que veía el estado del último bloque sin actualizar. Es una violación de orden real: lo que ve el consumidor (página residente) no coincidía con el estado del gestor. La aserción de la prueba era correcta.

### Arreglo

- `device_install()` usa ahora `UFFDIO_COPY_MODE_DONTWAKE`: instala la página sin despertar a nadie.
- Cada camino (`service()` desde host, desde disco y fallo duplicado, y `explicit_prefetch()`) hace **toda** su contabilidad y **al final** llama `UFFDIO_WAKE`. Cuando el hilo que falló continúa, el estado ya es coherente.
- La prueba **no se debilitó**: sigue exigiendo exactamente 256 bloques servidos.

Además corregí un segundo riesgo que encontré al investigar. Las pruebas usaban nombres fijos (`t_seq.bin`, ...) en `build/`, así que dos procesos de prueba simultáneos en el mismo directorio podían truncar el archivo del otro (`O_TRUNC`) mientras estaba mapeado. Ahora cada archivo lleva el PID y se borra al salir. El benchmark usa una ruta absoluta y conserva su archivo.

### Verificación del arreglo

Después del arreglo, `fault_secuencial` pasó 60/60 corridas aisladas con 8 bucles ocupados, y 4 corridas simultáneas en el mismo directorio pasaron todas. Antes había fallado 4 de 30.

Campaña completa con `stress-ng --cpu 8` corriendo en paralelo todo el tiempo (script en el mismo orden que abajo):

```
== carga: stress-ng --cpu 8 (pid 894116); loadavg: 21.34 12.06 6.79 34/2006 894182
Release ctest: 20/20 en verde
ASan+UBSan ctest: 5/5 en verde
TSan ctest: 5/5 en verde
verificar.sh corrida 1 (bajo carga): exit=0
verificar.sh corrida 2 (bajo carga): exit=0
loadavg al final: 16.47 19.20 15.76 20/1550 1030274
== FIN campaña
```

Salida completa de la 2.ª corrida de `./verificar.sh` bajo carga:

```
== entorno
6.12.94+
g++ (Debian 14.2.0-19) 14.2.0
cmake version 3.31.6
vm.unprivileged_userfaultfd=0
crw------- 1 root root 10, 257 Oct 14 23:50 /dev/userfaultfd
== compilación Release (-Wall -Wextra -Wpedantic -Werror)
[ 42%] Built target tiermem
[100%] Built target tiermem_tests
[100%] Built target tiermem_bench
== ctest corrida 1
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  14.55 sec
== ctest corrida 2
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  14.94 sec
== ctest corrida 3
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  11.75 sec
== ASan + UBSan
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  19.14 sec
== ThreadSanitizer
100% tests passed, 0 tests failed out of 15
Total Test time (real) =  32.34 sec
== como root (modo userfaultfd completo)
modo=completo (syscall userfaultfd) write()=4096 errno=0
OK   syscall_sobre_rango
15
== FIN
```

### Benchmark después del arreglo

Medido sin `stress-ng`, pero con la máquina cargada por otras agentes (loadavg ≈ 14 al empezar y ≈ 9,5 al terminar). El `UFFDIO_WAKE` extra no cambia de forma medible las filas limitadas por CPU: 4 KiB desde disco da 26 545–30 619 fallos/s y 4 KiB desde host 59 704 fallos/s, en línea con las corridas 1 y 2. Las filas limitadas por disco bajaron, pero la referencia sin userfaultfd también bajó (secuencial 1 MiB 1,600 GB/s contra 1,73–1,87), lo que indica contención en el disco compartido.

```
creando /workspace/sistemas/05-memoria-unificada/build/bench.bin (1024 MiB)...
hilos hardware: 8  liburing compilado: si
modo userfaultfd: UFFD_USER_MODE_ONLY (sin privilegios)

backend    bloque  patron origen    pf   hil     fallos     fallos/s      GB/s       seg
pread      4096    secu   nvme      0    1       262144        26545     0.109     9.876
pread      65536   secu   nvme      0    1        16384         8508     0.558     1.926
pread      65536   secu   nvme      8    1        16384         7416     0.486     2.209
io_uring   65536   secu   nvme      8    1        16384         5953     0.390     2.752
pread      1048576 secu   nvme      0    1         1024          599     0.628     1.709
io_uring   1048576 secu   nvme      4    1         1024          873     0.915     1.173
pread      4096    aleat  nvme      0    1        94345        30619     0.125     3.081
io_uring   4096    aleat  nvme      0    1        94345        26462     0.108     3.565
pread      65536   aleat  nvme      0    1        15402         6432     0.422     2.394
pread      65536   aleat  nvme      0    8        15338         8811     0.577     1.741
pread      4096    aleat  host      0    1        93830        59704     0.245     1.572
pread      65536   aleat  host      0    1        15369        25607     1.678     0.600
pread      65536   aleat  host      0    8        15314        17498     1.146     0.875

Referencia sin userfaultfd (pread O_DIRECT directo, 1 hilo):
  secuencial 64 KiB: 0.987 GB/s
  secuencial 1 MiB:  1.600 GB/s
  aleatorio 4 KiB:   0.270 GB/s
  aleatorio 64 KiB:  1.151 GB/s
```
