# VERIFICACIÓN — 05-memoria-unificada

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
