#include "tiermem/tiermem.hpp"

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef USERFAULTFD_IOC_NEW
#define USERFAULTFD_IOC_NEW _IO(0xAA, 0x00)
#endif

namespace tiermem {

const char* to_string(UffdMode m) {
    switch (m) {
        case UffdMode::Full: return "completo (syscall userfaultfd)";
        case UffdMode::DevNode: return "/dev/userfaultfd";
        case UffdMode::UserModeOnly: return "UFFD_USER_MODE_ONLY (sin privilegios)";
    }
    return "?";
}

namespace {

[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error(what + ": " + std::strerror(err));
}

int open_uffd(UffdMode* mode) {
    const int flags = O_CLOEXEC | O_NONBLOCK;
    int fd = static_cast<int>(syscall(SYS_userfaultfd, flags));
    if (fd >= 0) { *mode = UffdMode::Full; return fd; }
    int dev = ::open("/dev/userfaultfd", O_RDWR | O_CLOEXEC);
    if (dev >= 0) {
        fd = ioctl(dev, USERFAULTFD_IOC_NEW, flags);
        ::close(dev);
        if (fd >= 0) { *mode = UffdMode::DevNode; return fd; }
    }
    fd = static_cast<int>(syscall(SYS_userfaultfd, flags | UFFD_USER_MODE_ONLY));
    if (fd >= 0) { *mode = UffdMode::UserModeOnly; return fd; }
    return -errno;
}

struct Key {
    std::uint32_t tensor;
    std::uint32_t chunk;
    bool operator==(const Key& o) const { return tensor == o.tensor && chunk == o.chunk; }
};
struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        return std::hash<std::uint64_t>()((std::uint64_t(k.tensor) << 32) | k.chunk);
    }
};

struct Buffer {
    void* p = nullptr;
};

}  // namespace

struct TierManager::Impl {
    struct Tensor {
        std::byte* base = nullptr;
        std::size_t size = 0;
        std::size_t mapped = 0;
        std::size_t nchunks = 0;
        int handle = -1;
        std::vector<std::uint8_t> on_device;  // 1 si el bloque está mapeado
        std::vector<std::uint8_t> ref;        // bit de referencia (CLOCK)
        std::int64_t last_fault = -2;         // para detectar acceso secuencial
    };
    struct HostEntry {
        Key key;
        void* buf;
        bool prefetched_unused;
    };

    Config cfg;
    std::unique_ptr<StorageBackend> backend;
    UffdMode mode = UffdMode::Full;
    int uffd = -1;
    int evfd = -1;
    std::thread handler;

    std::mutex mu;  // protege todo el estado de abajo
    std::vector<Tensor> tensors;
    std::vector<Key> device_ring;  // bloques residentes en el nivel dispositivo
    std::size_t hand = 0;
    std::list<HostEntry> host_lru;  // frente = más reciente
    std::unordered_map<Key, std::list<HostEntry>::iterator, KeyHash> host_index;
    std::vector<void*> free_bufs;
    std::deque<std::pair<Key, bool>> pending_prefetch;  // (bloque, al dispositivo)
    bool stopping = false;

    // Estadísticas (sólo el hilo manejador escribe, salvo indicación)
    std::atomic<std::uint64_t> s_faults{0}, s_faults_res{0}, s_from_host{0}, s_from_storage{0},
        s_prefetched{0}, s_prefetch_hits{0}, s_dev_ev{0}, s_host_ev{0}, s_bytes{0}, s_dev_bytes{0},
        s_host_bytes{0}, s_dev_peak{0};

    std::size_t chunk() const { return cfg.chunk_bytes; }

    void* alloc_buf() {
        if (!free_bufs.empty()) {
            void* p = free_bufs.back();
            free_bufs.pop_back();
            return p;
        }
        void* p = std::aligned_alloc(StorageBackend::kAlign, chunk());
        if (!p) throw std::bad_alloc();
        return p;
    }
    void release_buf(void* p) { free_bufs.push_back(p); }

    // ---- nivel host (LRU) ----
    void host_touch(std::list<HostEntry>::iterator it) {
        host_lru.splice(host_lru.begin(), host_lru, it);
    }
    void host_make_room() {
        while (!host_lru.empty() && (s_host_bytes.load() + chunk() > cfg.host_capacity_bytes)) {
            HostEntry& victim = host_lru.back();
            host_index.erase(victim.key);
            release_buf(victim.buf);
            host_lru.pop_back();
            s_host_bytes -= chunk();
            ++s_host_ev;
        }
    }
    // Inserta (toma posesión de buf). Si no hay nivel host, libera buf.
    void host_insert(Key k, void* buf, bool prefetched) {
        if (cfg.host_capacity_bytes < chunk()) { release_buf(buf); return; }
        auto f = host_index.find(k);
        if (f != host_index.end()) { release_buf(buf); host_touch(f->second); return; }
        host_make_room();
        host_lru.push_front(HostEntry{k, buf, prefetched});
        host_index[k] = host_lru.begin();
        s_host_bytes += chunk();
    }

    // ---- nivel dispositivo (CLOCK) ----
    std::byte* chunk_addr(Key k) { return tensors[k.tensor].base + std::size_t(k.chunk) * chunk(); }

    void device_evict_one() {
        // Segunda oportunidad: salteamos los que tienen ref=1 (y lo bajamos a 0).
        for (;;) {
            if (hand >= device_ring.size()) hand = 0;
            Key k = device_ring[hand];
            Tensor& t = tensors[k.tensor];
            if (t.ref[k.chunk]) { t.ref[k.chunk] = 0; ++hand; continue; }
            std::byte* addr = chunk_addr(k);
            // Degradar al nivel host (si no estaba ya). El bloque está residente:
            // leerlo aquí no genera fallo.
            if (cfg.host_capacity_bytes >= chunk()) {
                auto f = host_index.find(k);
                if (f != host_index.end()) host_touch(f->second);
                else {
                    void* b = alloc_buf();
                    std::memcpy(b, addr, chunk());
                    host_insert(k, b, false);
                }
            }
            if (madvise(addr, chunk(), MADV_DONTNEED) != 0) fail("madvise(MADV_DONTNEED)", errno);
            t.on_device[k.chunk] = 0;
            device_ring[hand] = device_ring.back();
            device_ring.pop_back();
            s_dev_bytes -= chunk();
            ++s_dev_ev;
            return;
        }
    }
    void device_make_room() {
        while (!device_ring.empty() && s_dev_bytes.load() + chunk() > cfg.device_capacity_bytes)
            device_evict_one();
    }
    // Copia buf al bloque k (UFFDIO_COPY) SIN despertar a los hilos que esperan.
    // Quien llama debe terminar toda su contabilidad y recién después llamar wake(k):
    // así, cuando el hilo que falló continúa, las estadísticas y el estado ya son
    // coherentes con lo que ve (antes se despertaba dentro del ioctl y el hilo podía
    // leer stats() antes de que el manejador las actualizara).
    void device_install(Key k, const void* buf) {
        device_make_room();
        uffdio_copy c{};
        c.dst = reinterpret_cast<std::uintptr_t>(chunk_addr(k));
        c.src = reinterpret_cast<std::uintptr_t>(buf);
        c.len = chunk();
        c.mode = UFFDIO_COPY_MODE_DONTWAKE;
        for (;;) {
            if (ioctl(uffd, UFFDIO_COPY, &c) == 0) break;
            if (errno == EAGAIN && c.copy > 0) {  // copia parcial: continuar
                c.dst += static_cast<std::uint64_t>(c.copy);
                c.src += static_cast<std::uint64_t>(c.copy);
                c.len -= static_cast<std::uint64_t>(c.copy);
                c.copy = 0;
                continue;
            }
            if (errno == EAGAIN) continue;
            fail("UFFDIO_COPY", errno);
        }
        Tensor& t = tensors[k.tensor];
        t.on_device[k.chunk] = 1;
        t.ref[k.chunk] = 1;
        device_ring.push_back(k);
        s_dev_bytes += chunk();
        if (s_dev_bytes.load() > s_dev_peak.load()) s_dev_peak = s_dev_bytes.load();
    }

    void wake(Key k) {
        uffdio_range r{};
        r.start = reinterpret_cast<std::uintptr_t>(chunk_addr(k));
        r.len = chunk();
        ioctl(uffd, UFFDIO_WAKE, &r);
    }

    // Lee del almacenamiento un lote de bloques. Devuelve buffers (mismo orden).
    std::vector<void*> storage_read(const std::vector<Key>& keys) {
        std::vector<ReadRequest> reqs(keys.size());
        std::vector<void*> bufs(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const Tensor& t = tensors[keys[i].tensor];
            bufs[i] = alloc_buf();
            reqs[i].handle = t.handle;
            reqs[i].offset = std::uint64_t(keys[i].chunk) * chunk();
            reqs[i].length = chunk();
            reqs[i].dst = bufs[i];
        }
        backend->read_batch(reqs);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            long r = reqs[i].result;
            if (r < 0) fail("lectura del almacenamiento", static_cast<int>(-r));
            std::size_t got = static_cast<std::size_t>(r);
            if (got < chunk()) std::memset(static_cast<char*>(bufs[i]) + got, 0, chunk() - got);
            s_bytes += got;
        }
        return bufs;
    }

    bool in_host(Key k) const { return host_index.count(k) != 0; }

    // Atiende el fallo sobre el bloque k.
    void service(Key k, bool count_fault) {
        Tensor& t = tensors[k.tensor];
        // Prefetch adaptativo: sólo si este fallo continúa una racha secuencial.
        const bool sequential = (std::int64_t(k.chunk) == t.last_fault + 1);
        t.last_fault = k.chunk;
        if (count_fault) ++s_faults;
        if (t.on_device[k.chunk]) {
            if (count_fault) ++s_faults_res;
            t.ref[k.chunk] = 1;
            wake(k);
            return;
        }
        auto f = host_index.find(k);
        if (f != host_index.end()) {
            if (f->second->prefetched_unused) { ++s_prefetch_hits; f->second->prefetched_unused = false; }
            host_touch(f->second);
            device_install(k, f->second->buf);
            ++s_from_host;
            wake(k);
            return;
        }
        // Del almacenamiento, junto con la lectura anticipada secuencial.
        std::vector<Key> keys{k};
        for (unsigned i = 1; sequential && i <= cfg.prefetch_chunks; ++i) {
            std::uint64_t c = std::uint64_t(k.chunk) + i;
            if (c >= t.nchunks) break;
            Key p{k.tensor, static_cast<std::uint32_t>(c)};
            if (t.on_device[c] || in_host(p)) continue;
            keys.push_back(p);
        }
        if (cfg.host_capacity_bytes < chunk()) keys.resize(1);  // sin nivel host no hay dónde guardar
        std::vector<void*> bufs = storage_read(keys);
        device_install(k, bufs[0]);
        ++s_from_storage;
        host_insert(k, bufs[0], false);
        for (std::size_t i = 1; i < keys.size(); ++i) {
            host_insert(keys[i], bufs[i], true);
            ++s_prefetched;
        }
        wake(k);  // último paso: toda la contabilidad ya está hecha
    }

    void explicit_prefetch(Key k) {
        Tensor& t = tensors[k.tensor];
        if (t.on_device[k.chunk]) return;
        auto f = host_index.find(k);
        if (f != host_index.end()) {
            host_touch(f->second);
            device_install(k, f->second->buf);
        } else {
            std::vector<void*> b = storage_read({k});
            device_install(k, b[0]);
            host_insert(k, b[0], false);
        }
        ++s_prefetched;
        wake(k);  // por si algún hilo ya estaba esperando este bloque
    }

    bool lookup(std::uintptr_t addr, Key* out) {
        for (std::size_t i = 0; i < tensors.size(); ++i) {
            auto b = reinterpret_cast<std::uintptr_t>(tensors[i].base);
            if (addr >= b && addr < b + tensors[i].mapped) {
                out->tensor = static_cast<std::uint32_t>(i);
                out->chunk = static_cast<std::uint32_t>((addr - b) / chunk());
                return true;
            }
        }
        return false;
    }

    void loop() {
        uffd_msg msgs[64];
        for (;;) {
            pollfd p[2] = {{uffd, POLLIN, 0}, {evfd, POLLIN, 0}};
            int r = poll(p, 2, -1);
            if (r < 0) {
                if (errno == EINTR) continue;
                std::abort();
            }
            if (p[0].revents & POLLIN) {
                ssize_t n = read(uffd, msgs, sizeof(msgs));
                if (n > 0) {
                    std::lock_guard<std::mutex> lk(mu);
                    std::size_t cnt = static_cast<std::size_t>(n) / sizeof(uffd_msg);
                    for (std::size_t i = 0; i < cnt; ++i) {
                        if (msgs[i].event != UFFD_EVENT_PAGEFAULT) continue;
                        Key k{};
                        auto addr = static_cast<std::uintptr_t>(msgs[i].arg.pagefault.address);
                        if (lookup(addr, &k)) service(k, true);
                        else {  // dirección desconocida: despertar igual para no colgar al hilo
                            uffdio_range rg{addr & ~std::uintptr_t(4095), 4096};
                            ioctl(uffd, UFFDIO_WAKE, &rg);
                        }
                    }
                } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    std::abort();
                }
            }
            if (p[1].revents & POLLIN) {
                std::uint64_t v;
                if (read(evfd, &v, sizeof v) < 0) { /* nada */ }
                std::lock_guard<std::mutex> lk(mu);
                while (!pending_prefetch.empty()) {
                    Key k = pending_prefetch.front().first;
                    pending_prefetch.pop_front();
                    explicit_prefetch(k);
                }
                if (stopping) return;
            }
        }
    }
};

TierManager::TierManager(const Config& cfg) : impl_(std::make_unique<Impl>()) {
    Impl& I = *impl_;
    I.cfg = cfg;
    if (cfg.chunk_bytes == 0 || cfg.chunk_bytes % StorageBackend::kAlign != 0)
        throw std::invalid_argument("chunk_bytes debe ser múltiplo de 4096");
    if (cfg.device_capacity_bytes < cfg.chunk_bytes)
        throw std::invalid_argument("device_capacity_bytes debe alojar al menos un bloque");
    I.backend = make_backend(cfg.backend, cfg.direct_io);
    if (!I.backend) throw std::runtime_error("backend de almacenamiento no disponible");
    int fd = open_uffd(&I.mode);
    if (fd < 0) fail("userfaultfd (probar vm.unprivileged_userfaultfd=1 o kernel >= 5.11)", -fd);
    I.uffd = fd;
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = 0;
    if (ioctl(I.uffd, UFFDIO_API, &api) != 0) fail("UFFDIO_API", errno);
    I.evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (I.evfd < 0) fail("eventfd", errno);
    I.handler = std::thread([this] { impl_->loop(); });
}

TierManager::~TierManager() {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.stopping = true;
    }
    std::uint64_t one = 1;
    if (write(I.evfd, &one, sizeof one) < 0) { /* ignorar */ }
    I.handler.join();
    for (auto& t : I.tensors) {
        if (t.base) munmap(t.base, t.mapped);
        I.backend->close_file(t.handle);
    }
    for (auto& e : I.host_lru) std::free(e.buf);
    for (void* p : I.free_bufs) std::free(p);
    close(I.evfd);
    close(I.uffd);
}

TensorView TierManager::map_tensor(const std::string& path) {
    Impl& I = *impl_;
    int h = I.backend->open_file(path);
    if (h < 0) fail("abrir " + path, -h);
    std::size_t size = I.backend->file_size(h);
    if (size == 0) {
        I.backend->close_file(h);
        throw std::runtime_error("archivo vacío: " + path);
    }
    std::size_t nchunks = (size + I.chunk() - 1) / I.chunk();
    std::size_t mapped = nchunks * I.chunk();
    void* base = mmap(nullptr, mapped, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) fail("mmap", errno);
    // Sin THP: queremos granularidad exacta de bloque al expulsar.
    madvise(base, mapped, MADV_NOHUGEPAGE);
    uffdio_register reg{};
    reg.range.start = reinterpret_cast<std::uintptr_t>(base);
    reg.range.len = mapped;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(I.uffd, UFFDIO_REGISTER, &reg) != 0) {
        int e = errno;
        munmap(base, mapped);
        fail("UFFDIO_REGISTER", e);
    }
    if (!(reg.ioctls & (1ULL << _UFFDIO_COPY))) {
        munmap(base, mapped);
        throw std::runtime_error("el rango no soporta UFFDIO_COPY");
    }
    std::lock_guard<std::mutex> lk(I.mu);
    Impl::Tensor t;
    t.base = static_cast<std::byte*>(base);
    t.size = size;
    t.mapped = mapped;
    t.nchunks = nchunks;
    t.handle = h;
    t.on_device.assign(nchunks, 0);
    t.ref.assign(nchunks, 0);
    I.tensors.push_back(std::move(t));
    TensorView v;
    v.data = static_cast<const std::byte*>(base);
    v.size = size;
    v.mapped = mapped;
    v.id = static_cast<int>(I.tensors.size() - 1);
    return v;
}

void TierManager::prefetch(const TensorView& tv, std::size_t offset, std::size_t len) {
    Impl& I = *impl_;
    if (tv.id < 0 || len == 0 || offset >= tv.size) return;
    std::size_t end = std::min(tv.size, offset + len);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        for (std::size_t c = offset / I.chunk(); c * I.chunk() < end; ++c)
            I.pending_prefetch.push_back({Key{static_cast<std::uint32_t>(tv.id), static_cast<std::uint32_t>(c)}, true});
    }
    std::uint64_t one = 1;
    if (write(I.evfd, &one, sizeof one) < 0) { /* ignorar */ }
}

Stats TierManager::stats() const {
    const Impl& I = *impl_;
    Stats s;
    s.faults = I.s_faults;
    s.faults_resident = I.s_faults_res;
    s.served_from_host = I.s_from_host;
    s.served_from_storage = I.s_from_storage;
    s.prefetched = I.s_prefetched;
    s.prefetch_hits = I.s_prefetch_hits;
    s.device_evictions = I.s_dev_ev;
    s.host_evictions = I.s_host_ev;
    s.bytes_from_storage = I.s_bytes;
    s.device_resident_bytes = I.s_dev_bytes;
    s.host_resident_bytes = I.s_host_bytes;
    s.device_peak_bytes = I.s_dev_peak;
    return s;
}

UffdMode TierManager::uffd_mode() const { return impl_->mode; }
const char* TierManager::backend_name() const { return impl_->backend->name(); }
const Config& TierManager::config() const { return impl_->cfg; }

}  // namespace tiermem
