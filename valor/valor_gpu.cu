// Experimento de VALOR del sistema 5 (tiermem) en GPU real.
// Misma carga GPU para todos los métodos: por cada acceso se elige un bloque de 1 MiB
// de un conjunto de trabajo de W bytes (W >> presupuesto de RAM del cgroup), la CPU lo
// copia a un buffer pinned (aquí ocurren los fallos de página), se sube a la GPU con
// cudaMemcpyAsync y un kernel suma sus bytes. Se mide la latencia de cada acceso.
//
// Métodos (fuente del bloque):
//   swap     : memoria anónima de W bytes; el kernel la pagina a SWAP (línea base principal)
//   mmap     : el archivo mapeado con mmap; el kernel pagina con el page cache (secundaria)
//   tiermem  : TierManager (userfaultfd), dispositivo+host <= presupuesto, almacenamiento O_DIRECT
//   explicit : pread O_DIRECT directo al buffer pinned (paginación explícita a mano; cota superior)
//
// Uso: valor_gpu <metodo> <archivo> <W_MiB> <accesos> <aleat|secu> <presupuesto_MiB> <semilla>
#include <cuda_runtime.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../tests/common.hpp"
#include "tiermem/tiermem.hpp"

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  std::fprintf(stderr, "CUDA %s: %s\n", #x, cudaGetErrorString(e_)); std::exit(3);} } while (0)

static constexpr std::size_t CH = 1 << 20;
static constexpr std::uint64_t SEED = 42;

__global__ void sum_bytes(const unsigned char* p, std::size_t n, unsigned long long* out) {
    unsigned long long acc = 0;
    for (std::size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) acc += p[i];
    atomicAdd(out, acc);
}

static long rss_cgroup_bytes() {
    FILE* f = std::fopen("/proc/self/cgroup", "r");
    if (!f) return -1;
    char line[4096]; std::string path;
    while (std::fgets(line, sizeof line, f)) if (!std::strncmp(line, "0::", 3)) { path = line + 3; path.pop_back(); }
    std::fclose(f);
    FILE* g = std::fopen(("/sys/fs/cgroup" + path + "/memory.current").c_str(), "r");
    if (!g) return -1;
    long v = -1; if (std::fscanf(g, "%ld", &v) != 1) v = -1; std::fclose(g);
    return v;
}
static long cg_file(const char* name) {
    FILE* f = std::fopen("/proc/self/cgroup", "r");
    if (!f) return -1;
    char line[4096]; std::string path;
    while (std::fgets(line, sizeof line, f)) if (!std::strncmp(line, "0::", 3)) { path = line + 3; path.pop_back(); }
    std::fclose(f);
    FILE* g = std::fopen(("/sys/fs/cgroup" + path + "/" + name).c_str(), "r");
    if (!g) return -1;
    char buf[64] = {0}; if (!std::fgets(buf, sizeof buf, g)) buf[0] = 0; std::fclose(g);
    if (!std::strncmp(buf, "max", 3)) return -2;
    return std::atol(buf);
}

int main(int argc, char** argv) {
    if (argc < 8) { std::fprintf(stderr, "uso: %s metodo archivo W_MiB accesos aleat|secu presupuesto_MiB semilla\n", argv[0]); return 2; }
    std::string method = argv[1], path = argv[2];
    std::size_t W = std::strtoull(argv[3], nullptr, 10) << 20;
    std::size_t N = std::strtoull(argv[4], nullptr, 10);
    bool rnd = std::string(argv[5]) == "aleat";
    std::size_t budget = std::strtoull(argv[6], nullptr, 10) << 20;
    unsigned seed = std::atoi(argv[7]);
    std::size_t nch = W / CH;

    CK(cudaSetDevice(0));
    CK(cudaFree(nullptr));  // crear contexto antes de medir la memoria base
    unsigned char *pin = nullptr, *dev = nullptr; unsigned long long* dsum = nullptr;
    CK(cudaHostAlloc(&pin, CH, cudaHostAllocDefault));
    CK(cudaMalloc(&dev, CH)); CK(cudaMalloc(&dsum, sizeof *dsum));
    cudaStream_t st; CK(cudaStreamCreate(&st));
    long base = rss_cgroup_bytes();

    const unsigned char* src = nullptr;
    std::unique_ptr<tiermem::TierManager> tm;
    int fd = -1;
    unsigned char* anon = nullptr;
    if (method == "swap") {
        anon = static_cast<unsigned char*>(mmap(nullptr, W, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (anon == MAP_FAILED) { std::perror("mmap"); return 2; }
        madvise(anon, W, MADV_NOHUGEPAGE);
        for (std::size_t off = 0; off < W; off += 8) {  // mismo contenido que el archivo
            std::uint64_t v = tt::splitmix(SEED ^ (off / 8));
            std::memcpy(anon + off, &v, 8);
        }
        src = anon;
    } else if (method == "mmap") {
        fd = ::open(path.c_str(), O_RDONLY);
        void* p = mmap(nullptr, W, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { std::perror("mmap"); return 2; }
        posix_fadvise(fd, 0, W, POSIX_FADV_DONTNEED);  // vaciar page cache propio (sin root, sólo páginas limpias)
        src = static_cast<const unsigned char*>(p);
    } else if (method == "tiermem") {
        tiermem::Config c;
        c.chunk_bytes = CH;
        c.device_capacity_bytes = budget / 2;
        c.host_capacity_bytes = budget / 2;
        c.prefetch_chunks = 4;
        c.backend = tiermem::BackendKind::Pread;
        tm = std::make_unique<tiermem::TierManager>(c);
        auto tv = tm->map_tensor(path);
        src = reinterpret_cast<const unsigned char*>(tv.data);
        std::printf("# tiermem uffd=%s backend=%s dev=%zuMiB host=%zuMiB\n", tiermem::to_string(tm->uffd_mode()),
                    tm->backend_name(), budget / 2 >> 20, budget / 2 >> 20);
    } else if (method == "explicit") {
        fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
        if (fd < 0) { std::perror("open"); return 2; }
    } else { std::fprintf(stderr, "metodo desconocido\n"); return 2; }

    std::mt19937_64 rng(seed);
    std::vector<double> lat; lat.reserve(N);
    std::size_t bad = 0;
    auto T0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        std::size_t c = rnd ? rng() % nch : i % nch;
        auto a = std::chrono::steady_clock::now();
        if (method == "explicit") {
            if (pread(fd, pin, CH, static_cast<off_t>(c * CH)) != static_cast<ssize_t>(CH)) { std::perror("pread"); return 2; }
        } else {
            std::memcpy(pin, src + c * CH, CH);
        }
        CK(cudaMemsetAsync(dsum, 0, sizeof *dsum, st));
        CK(cudaMemcpyAsync(dev, pin, CH, cudaMemcpyHostToDevice, st));
        sum_bytes<<<64, 256, 0, st>>>(dev, CH, dsum);
        unsigned long long g = 0;
        CK(cudaMemcpyAsync(&g, dsum, sizeof g, cudaMemcpyDeviceToHost, st));
        CK(cudaStreamSynchronize(st));
        auto b = std::chrono::steady_clock::now();
        lat.push_back(std::chrono::duration<double, std::milli>(b - a).count());
        // verificación: suma GPU == suma CPU del buffer, y 3 bytes == contenido esperado
        unsigned long long h = 0; for (std::size_t k = 0; k < CH; ++k) h += pin[k];
        if (h != g) ++bad;
        for (std::size_t k : {std::size_t(0), CH / 2 + 3, CH - 1})
            if (pin[k] != tt::expected(SEED, c * CH + k)) ++bad;
    }
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
    std::vector<double> s = lat; std::sort(s.begin(), s.end());
    auto pct = [&](double p) { return s[std::min(s.size() - 1, static_cast<std::size_t>(p * (s.size() - 1) + 0.5))]; };
    // tiempo neto de acceso (sin la verificación en CPU)
    double sum_lat = 0; for (double x : lat) sum_lat += x;
    long swap_now = cg_file("memory.swap.current");
    std::printf("RES metodo=%s patron=%s W_MiB=%zu accesos=%zu presupuesto_MiB=%zu base_cgroup_MiB=%ld "
                "p50_ms=%.3f p99_ms=%.3f max_ms=%.3f GBps=%.3f swap_cgroup_MiB=%ld errores=%zu total_s=%.2f\n",
                method.c_str(), rnd ? "aleat" : "secu", W >> 20, N, budget >> 20, base >> 20, pct(0.5), pct(0.99),
                s.back(), (double)N * CH / (sum_lat / 1e3) / 1e9, swap_now < 0 ? swap_now : swap_now >> 20, bad, total);
    if (tm) {
        auto x = tm->stats();
        std::printf("# tiermem faults=%llu storage=%llu host=%llu dev_ev=%llu peak_dev_MiB=%llu\n",
                    (unsigned long long)x.faults, (unsigned long long)x.served_from_storage,
                    (unsigned long long)x.served_from_host, (unsigned long long)x.device_evictions,
                    (unsigned long long)(x.device_peak_bytes >> 20));
    }
    return bad ? 1 : 0;
}
