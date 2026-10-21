// Benchmark de tiermem: fallos/s y GB/s en esta máquina.
// Uso: tiermem_bench [archivo] [MiB]   (por defecto ./bench.bin, 1024 MiB)
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../tests/common.hpp"
#include "tiermem/tiermem.hpp"

using namespace tiermem;
using clk = std::chrono::steady_clock;

static double secs(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

struct Result { double s; std::uint64_t faults; std::uint64_t bytes; Stats st; };

static Result run(const std::string& path, BackendKind bk, std::size_t chunk, bool random,
                  unsigned prefetch, std::size_t touches, int threads, bool warm_host) {
    Config c;
    c.backend = bk;
    c.chunk_bytes = chunk;
    c.device_capacity_bytes = 64 << 20;
    c.host_capacity_bytes = warm_host ? (std::size_t(2) << 30) : (256 << 20);
    c.prefetch_chunks = prefetch;
    TierManager m(c);
    TensorView t = m.map_tensor(path);
    std::size_t nch = t.mapped / chunk;
    volatile std::uint64_t sink = 0;
    auto touch_all = [&](int tid, int nthreads, std::size_t n) {
        std::mt19937_64 rng(1234 + tid);
        std::uint64_t acc = 0;
        for (std::size_t i = tid; i < n; i += nthreads) {
            std::size_t ch = random ? (rng() % nch) : (i % nch);
            acc += static_cast<std::uint8_t>(t.data[ch * chunk + (i * 64) % chunk]);
        }
        sink = sink + acc;
    };
    if (warm_host) {  // cargar todo en el nivel host primero; luego medir sólo host->dispositivo
        touch_all(0, 1, nch);
    }
    Stats s0 = m.stats();
    auto a = clk::now();
    if (threads == 1) touch_all(0, 1, touches);
    else {
        std::vector<std::thread> th;
        for (int k = 0; k < threads; ++k) th.emplace_back(touch_all, k, threads, touches);
        for (auto& x : th) x.join();
    }
    auto b = clk::now();
    Stats s1 = m.stats();
    Result r;
    r.s = secs(a, b);
    r.faults = s1.faults - s0.faults;
    r.bytes = (s1.served_from_host + s1.served_from_storage - s0.served_from_host - s0.served_from_storage) * chunk;
    r.st = s1;
    return r;
}

static double raw_pread(const std::string& path, std::size_t chunk, bool random, std::size_t n) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
    off_t size = lseek(fd, 0, SEEK_END);
    std::size_t nch = static_cast<std::size_t>(size) / chunk;
    void* buf = std::aligned_alloc(4096, chunk);
    std::mt19937_64 rng(99);
    auto a = clk::now();
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t ch = random ? rng() % nch : i % nch;
        ssize_t r = pread(fd, buf, chunk, static_cast<off_t>(ch * chunk));
        if (r > 0) bytes += static_cast<std::uint64_t>(r);
    }
    double s = secs(a, clk::now());
    std::free(buf);
    close(fd);
    return static_cast<double>(bytes) / s / 1e9;
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "bench.bin";
    std::uint64_t mib = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1024;
    if (access(path.c_str(), R_OK) != 0) {
        std::printf("creando %s (%llu MiB)...\n", path.c_str(), (unsigned long long)mib);
        tt::make_file(path, mib << 20, 42);
    }
    std::printf("hilos hardware: %u  liburing compilado: %s\n", std::thread::hardware_concurrency(),
                iouring_compiled_in() ? "si" : "no");
    {
        Config c;
        TierManager probe(c);
        std::printf("modo userfaultfd: %s\n", to_string(probe.uffd_mode()));
    }
    std::printf("\n%-10s %-7s %-6s %-9s %-4s %-3s %10s %12s %9s %9s\n", "backend", "bloque", "patron",
                "origen", "pf", "hil", "fallos", "fallos/s", "GB/s", "seg");
    struct Case { BackendKind bk; const char* bn; std::size_t chunk; bool rnd; unsigned pf; int thr; bool warm; };
    std::vector<Case> cases = {
        {BackendKind::Pread, "pread", 4096, false, 0, 1, false},
        {BackendKind::Pread, "pread", 65536, false, 0, 1, false},
        {BackendKind::Pread, "pread", 65536, false, 8, 1, false},
        {BackendKind::IoUring, "io_uring", 65536, false, 8, 1, false},
        {BackendKind::Pread, "pread", 1 << 20, false, 0, 1, false},
        {BackendKind::IoUring, "io_uring", 1 << 20, false, 4, 1, false},
        {BackendKind::Pread, "pread", 4096, true, 0, 1, false},
        {BackendKind::IoUring, "io_uring", 4096, true, 0, 1, false},
        {BackendKind::Pread, "pread", 65536, true, 0, 1, false},
        {BackendKind::Pread, "pread", 65536, true, 0, 8, false},
        {BackendKind::Pread, "pread", 4096, true, 0, 1, true},
        {BackendKind::Pread, "pread", 65536, true, 0, 1, true},
        {BackendKind::Pread, "pread", 65536, true, 0, 8, true},
    };
    for (auto& k : cases) {
        std::size_t nch = static_cast<std::size_t>((mib << 20) / k.chunk);
        std::size_t touches = k.rnd ? std::min<std::size_t>(nch, 100000) : nch;
        Result r = run(path, k.bk, k.chunk, k.rnd, k.pf, touches, k.thr, k.warm);
        std::printf("%-10s %-7zu %-6s %-9s %-4u %-3d %10llu %12.0f %9.3f %9.3f\n", k.bn, k.chunk,
                    k.rnd ? "aleat" : "secu", k.warm ? "host" : "nvme", k.pf, k.thr,
                    (unsigned long long)r.faults, r.faults / r.s, r.bytes / r.s / 1e9, r.s);
    }
    std::printf("\nReferencia sin userfaultfd (pread O_DIRECT directo, 1 hilo):\n");
    std::printf("  secuencial 64 KiB: %.3f GB/s\n", raw_pread(path, 65536, false, (mib << 20) / 65536));
    std::printf("  secuencial 1 MiB:  %.3f GB/s\n", raw_pread(path, 1 << 20, false, mib));
    std::printf("  aleatorio 4 KiB:   %.3f GB/s\n", raw_pread(path, 4096, true, 100000));
    std::printf("  aleatorio 64 KiB:  %.3f GB/s\n", raw_pread(path, 65536, true, 16384));
    return 0;
}
