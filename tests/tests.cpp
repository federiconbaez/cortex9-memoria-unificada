// Pruebas de tiermem. Uso: tiermem_tests <nombre>   (o "all")
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <thread>

#include "common.hpp"
#include "tiermem/tiermem.hpp"

using namespace tiermem;

static int g_fail = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

static void print_stats(const char* tag, const TierManager& m) {
    Stats s = m.stats();
    std::printf("[%s] backend=%s uffd=%s faults=%llu resid=%llu host=%llu storage=%llu "
                "prefetched=%llu pf_hits=%llu dev_ev=%llu host_ev=%llu dev_peak=%llu\n",
                tag, m.backend_name(), to_string(m.uffd_mode()), (unsigned long long)s.faults,
                (unsigned long long)s.faults_resident, (unsigned long long)s.served_from_host,
                (unsigned long long)s.served_from_storage, (unsigned long long)s.prefetched,
                (unsigned long long)s.prefetch_hits, (unsigned long long)s.device_evictions,
                (unsigned long long)s.host_evictions, (unsigned long long)s.device_peak_bytes);
}

// Verifica todo el tensor contra el contenido esperado; devuelve # de bytes erróneos.
static std::size_t verify_all(const TensorView& t, std::uint64_t seed) {
    std::size_t bad = 0;
    for (std::size_t i = 0; i < t.size; ++i)
        if (static_cast<std::uint8_t>(t.data[i]) != tt::expected(seed, i)) ++bad;
    return bad;
}

// Páginas realmente residentes según el kernel (mincore).
static std::size_t resident_bytes(const TensorView& t) {
    std::vector<unsigned char> v(t.mapped / 4096);
    if (mincore(const_cast<std::byte*>(t.data), t.mapped, v.data()) != 0) return SIZE_MAX;
    std::size_t n = 0;
    for (unsigned char c : v) n += (c & 1);
    return n * 4096;
}

static void backend_check(BackendKind k) {
    auto b = make_backend(k, true);
    if (!b) { std::printf("backend no disponible\n"); CHECK(k != BackendKind::IoUring || !iouring_compiled_in()); return; }
    std::printf("backend: %s\n", b->name());
    const std::uint64_t size = 3 * 1024 * 1024 + 12345;  // cola no alineada
    std::string p = tt::make_file("t_backend.bin", size, 7);
    int h = b->open_file(p);
    CHECK(h >= 0);
    CHECK(b->file_size(h) == size);
    const std::size_t blk = 256 * 1024;
    std::vector<void*> bufs;
    std::vector<ReadRequest> reqs;
    for (std::uint64_t off = 0; off < size; off += blk) {
        void* d = std::aligned_alloc(4096, blk);
        bufs.push_back(d);
        reqs.push_back(ReadRequest{h, off, blk, d, 0});
    }
    b->read_batch(reqs);
    for (auto& r : reqs) {
        std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(blk, size - r.offset));
        CHECK(r.result == static_cast<long>(want));
        for (std::size_t i = 0; i < want; ++i)
            if (static_cast<std::uint8_t*>(r.dst)[i] != tt::expected(7, r.offset + i)) { CHECK(false); break; }
    }
    for (void* d : bufs) std::free(d);
    b->close_file(h);
}

static Config small_cfg() {
    Config c;
    c.chunk_bytes = 64 * 1024;
    c.device_capacity_bytes = 1 << 20;   // 1 MiB "VRAM"
    c.host_capacity_bytes = 4 << 20;     // 4 MiB RAM de host
    c.prefetch_chunks = 4;
    return c;
}

static std::map<std::string, std::function<void()>> tests = {
    {"backend_pread", [] { backend_check(BackendKind::Pread); }},
    {"backend_iouring", [] { backend_check(BackendKind::IoUring); }},
    {"fault_secuencial",
     [] {
         Config c;  // capacidad grande: sin expulsiones
         c.device_capacity_bytes = 64 << 20;
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_seq.bin", 16 << 20, 11));
         CHECK(verify_all(t, 11) == 0);
         Stats s = m.stats();
         CHECK(s.faults > 0);
         CHECK(s.device_evictions == 0);
         CHECK(s.served_from_storage + s.served_from_host == 256);  // 16 MiB / 64 KiB
         print_stats("fault_secuencial", m);
     }},
    {"fault_aleatorio",
     [] {
         TierManager m(small_cfg());
         auto t = m.map_tensor(tt::make_file("t_rand.bin", 24 << 20, 12));
         std::mt19937_64 rng(1);
         std::size_t bad = 0;
         for (int i = 0; i < 20000; ++i) {
             std::size_t off = rng() % t.size;
             if (static_cast<std::uint8_t>(t.data[off]) != tt::expected(12, off)) ++bad;
         }
         CHECK(bad == 0);
         print_stats("fault_aleatorio", m);
     }},
    {"cola_no_multiplo",
     [] {
         TierManager m(small_cfg());
         const std::size_t size = (5 << 20) + 777;
         auto t = m.map_tensor(tt::make_file("t_tail.bin", size, 13));
         CHECK(t.size == size);
         CHECK(t.mapped % (64 * 1024) == 0);
         CHECK(verify_all(t, 13) == 0);
         // Más allá del tamaño lógico, el bloque se rellena con ceros.
         for (std::size_t i = size; i < t.mapped; ++i) CHECK(t.data[i] == std::byte{0});
     }},
    {"expulsion_bajo_presion",
     [] {
         Config c = small_cfg();
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_evict.bin", 32 << 20, 14));
         for (int pass = 0; pass < 2; ++pass) {
             CHECK(verify_all(t, 14) == 0);
             std::size_t rb = resident_bytes(t);
             std::printf("residente segun mincore: %zu bytes (tope %zu)\n", rb, c.device_capacity_bytes);
             CHECK(rb <= c.device_capacity_bytes);
         }
         Stats s = m.stats();
         CHECK(s.device_evictions > 0);
         CHECK(s.device_peak_bytes <= c.device_capacity_bytes);
         CHECK(s.device_resident_bytes <= c.device_capacity_bytes);
         CHECK(s.host_resident_bytes <= c.host_capacity_bytes);
         CHECK(s.host_evictions > 0);
         print_stats("expulsion_bajo_presion", m);
     }},
    {"nivel_host",
     [] {
         // El conjunto de trabajo (3 MiB) excede el dispositivo (1 MiB) pero cabe en host (4 MiB):
         // en la segunda pasada nada debería venir del almacenamiento.
         Config c = small_cfg();
         c.prefetch_chunks = 0;
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_host.bin", 3 << 20, 15));
         CHECK(verify_all(t, 15) == 0);
         Stats s1 = m.stats();
         CHECK(verify_all(t, 15) == 0);
         Stats s2 = m.stats();
         CHECK(s2.served_from_storage == s1.served_from_storage);
         CHECK(s2.served_from_host > s1.served_from_host);
         print_stats("nivel_host", m);
     }},
    {"prefetch_secuencial",
     [] {
         Config c = small_cfg();
         c.prefetch_chunks = 8;
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_pf.bin", 8 << 20, 16));
         CHECK(verify_all(t, 16) == 0);
         Stats s = m.stats();
         CHECK(s.prefetched > 0);
         CHECK(s.prefetch_hits > 0);
         // 128 bloques; con prefetch=8, ~1 de cada 9 va al almacenamiento.
         CHECK(s.served_from_storage < 40);
         print_stats("prefetch_secuencial", m);
     }},
    {"prefetch_explicito",
     [] {
         Config c = small_cfg();
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_pfx.bin", 4 << 20, 17));
         m.prefetch(t, 0, 512 * 1024);
         for (int i = 0; i < 200 && resident_bytes(t) < 512 * 1024; ++i) usleep(5000);
         CHECK(resident_bytes(t) >= 512 * 1024);
         std::uint64_t f0 = m.stats().faults;
         for (std::size_t i = 0; i < 512 * 1024; ++i)
             if (static_cast<std::uint8_t>(t.data[i]) != tt::expected(17, i)) { CHECK(false); break; }
         CHECK(m.stats().faults == f0);  // ya residente: ningún fallo
     }},
    {"multihilo",
     [] {
         Config c;
         c.device_capacity_bytes = 64 << 20;
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_mt.bin", 32 << 20, 18));
         std::atomic<std::size_t> bad{0};
         std::vector<std::thread> th;
         for (int k = 0; k < 8; ++k)
             th.emplace_back([&, k] {
                 std::mt19937_64 rng(100 + k);
                 for (int i = 0; i < 100000; ++i) {
                     std::size_t off = rng() % t.size;
                     if (static_cast<std::uint8_t>(t.data[off]) != tt::expected(18, off)) ++bad;
                 }
             });
         for (auto& x : th) x.join();
         CHECK(bad == 0);
         print_stats("multihilo", m);
     }},
    {"multihilo_presion",
     [] {
         TierManager m(small_cfg());
         auto t = m.map_tensor(tt::make_file("t_mtp.bin", 32 << 20, 19));
         std::atomic<std::size_t> bad{0};
         std::vector<std::thread> th;
         for (int k = 0; k < 8; ++k)
             th.emplace_back([&, k] {
                 std::mt19937_64 rng(200 + k);
                 for (int i = 0; i < 5000; ++i) {
                     std::size_t off = rng() % (t.size - 4096);
                     // lectura de un rango (memcpy puede cruzar páginas y fallar a mitad)
                     std::uint8_t tmp[4096];
                     std::memcpy(tmp, t.data + off, sizeof tmp);
                     for (std::size_t j = 0; j < sizeof tmp; j += 97)
                         if (tmp[j] != tt::expected(19, off + j)) ++bad;
                 }
             });
         for (auto& x : th) x.join();
         CHECK(bad == 0);
         Stats s = m.stats();
         CHECK(s.device_evictions > 0);
         CHECK(s.device_peak_bytes <= small_cfg().device_capacity_bytes);
         CHECK(resident_bytes(t) <= small_cfg().device_capacity_bytes);
         print_stats("multihilo_presion", m);
     }},
    {"varios_tensores",
     [] {
         Config c = small_cfg();
         TierManager m(c);
         auto a = m.map_tensor(tt::make_file("t_a.bin", 6 << 20, 21));
         auto b = m.map_tensor(tt::make_file("t_b.bin", 6 << 20, 22));
         for (int r = 0; r < 2; ++r) {
             CHECK(verify_all(a, 21) == 0);
             CHECK(verify_all(b, 22) == 0);
         }
         // El tope del dispositivo es compartido entre tensores.
         CHECK(resident_bytes(a) + resident_bytes(b) <= c.device_capacity_bytes);
         print_stats("varios_tensores", m);
     }},
    {"solo_lectura_sigsegv",
     [] {
         TierManager m(small_cfg());
         auto t = m.map_tensor(tt::make_file("t_ro.bin", 1 << 20, 23));
         CHECK(static_cast<std::uint8_t>(t.data[10]) == tt::expected(23, 10));
         pid_t pid = fork();
         if (pid == 0) {
             tt::temp_files().clear();  // el hijo no borra los archivos del padre
             struct sigaction sa {};
             sa.sa_handler = [](int) { _exit(77); };
             sigaction(SIGSEGV, &sa, nullptr);
             *const_cast<volatile std::byte*>(t.data + 10) = std::byte{1};
             _exit(0);
         }
         int st = 0;
         waitpid(pid, &st, 0);
         CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 77);
     }},
    {"syscall_sobre_rango",
     [] {
         // Un syscall que lee del rango (write desde el tensor) genera un fallo en modo kernel.
         // En modo UFFD_USER_MODE_ONLY ese fallo NO se atiende: write() devuelve EFAULT.
         TierManager m(small_cfg());
         auto t = m.map_tensor(tt::make_file("t_sys.bin", 1 << 20, 24));
         std::string out = "t_sys_out.bin." + std::to_string(::getpid());
         int fd = ::open(out.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
         CHECK(fd >= 0);
         ssize_t w = ::write(fd, t.data + 128 * 1024, 4096);  // bloque aún no residente
         int e = errno;
         std::printf("modo=%s write()=%zd errno=%d\n", to_string(m.uffd_mode()), w, w < 0 ? e : 0);
         if (m.uffd_mode() == UffdMode::UserModeOnly) {
             CHECK(w < 0 && e == EFAULT);
             // Solución documentada: tocar/prefetch desde modo usuario antes del syscall.
             volatile std::byte x = t.data[128 * 1024];
             (void)x;
             w = ::write(fd, t.data + 128 * 1024, 4096);
         }
         CHECK(w == 4096);
         std::uint8_t back[4096];
         CHECK(::pread(fd, back, sizeof back, 0) == 4096);
         for (std::size_t i = 0; i < sizeof back; ++i)
             if (back[i] != tt::expected(24, 128 * 1024 + i)) { CHECK(false); break; }
         ::close(fd);
         ::unlink(out.c_str());
     }},
    {"sin_nivel_host",
     [] {
         Config c = small_cfg();
         c.host_capacity_bytes = 0;
         TierManager m(c);
         auto t = m.map_tensor(tt::make_file("t_nohost.bin", 4 << 20, 25));
         CHECK(verify_all(t, 25) == 0);
         CHECK(verify_all(t, 25) == 0);
         Stats s = m.stats();
         CHECK(s.served_from_host == 0);
         CHECK(s.host_resident_bytes == 0);
         CHECK(resident_bytes(t) <= c.device_capacity_bytes);
     }},
};

int main(int argc, char** argv) {
    std::vector<std::string> names;
    if (argc < 2 || std::string(argv[1]) == "all") for (auto& kv : tests) names.push_back(kv.first);
    else names.push_back(argv[1]);
    for (auto& n : names) {
        auto it = tests.find(n);
        if (it == tests.end()) { std::fprintf(stderr, "prueba desconocida: %s\n", n.c_str()); return 2; }
        int before = g_fail;
        try {
            it->second();
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "EXCEPCION en %s: %s\n", n.c_str(), ex.what());
            ++g_fail;
        }
        std::printf("%s %s\n", g_fail == before ? "OK  " : "FAIL", n.c_str());
    }
    return g_fail == 0 ? 0 : 1;
}
