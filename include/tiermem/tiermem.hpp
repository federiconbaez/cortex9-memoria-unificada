// tiermem: gestor de memoria por niveles con fallos de página atendidos por userfaultfd.
//
// Niveles:
//   "dispositivo" (emulado): páginas residentes dentro del rango virtual reservado.
//                 Es lo que el código consumidor toca directamente; tiene un tope de bytes.
//   host:         caché LRU en RAM de bloques expulsados del nivel dispositivo.
//   almacenamiento (NVMe): el archivo del tensor, leído con pread+O_DIRECT o io_uring.
//
// Los tensores se mapean de SOLO LECTURA. Escribir en ellos produce SIGSEGV.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "tiermem/backend.hpp"

namespace tiermem {

struct Config {
    std::size_t chunk_bytes = 64 * 1024;            // granularidad de fallo/expulsión (múltiplo de 4096)
    std::size_t device_capacity_bytes = 64 << 20;   // tope del nivel "dispositivo"
    std::size_t host_capacity_bytes = 256 << 20;    // tope del nivel host (0 = sin nivel host)
    unsigned prefetch_chunks = 4;                   // lectura anticipada tras un fallo que sigue una racha secuencial
    BackendKind backend = BackendKind::Auto;
    bool direct_io = true;
};

enum class UffdMode {
    Full,          // syscall userfaultfd sin restricciones (root o vm.unprivileged_userfaultfd=1)
    DevNode,       // abierto vía /dev/userfaultfd
    UserModeOnly,  // UFFD_USER_MODE_ONLY: sólo fallos desde modo usuario
};
const char* to_string(UffdMode m);

struct Stats {
    std::uint64_t faults = 0;              // mensajes de fallo atendidos
    std::uint64_t faults_resident = 0;     // fallo duplicado sobre bloque ya residente (sólo wake)
    std::uint64_t served_from_host = 0;    // bloques subidos desde el nivel host
    std::uint64_t served_from_storage = 0; // bloques leídos del archivo por un fallo
    std::uint64_t prefetched = 0;          // bloques traídos por prefetch
    std::uint64_t prefetch_hits = 0;       // fallos evitados porque el prefetch ya estaba en host
    std::uint64_t device_evictions = 0;
    std::uint64_t host_evictions = 0;
    std::uint64_t bytes_from_storage = 0;
    std::uint64_t device_resident_bytes = 0;
    std::uint64_t host_resident_bytes = 0;
    std::uint64_t device_peak_bytes = 0;
};

struct TensorView {
    const std::byte* data = nullptr;
    std::size_t size = 0;      // tamaño lógico (tamaño del archivo)
    std::size_t mapped = 0;    // tamaño del rango reservado (redondeado a chunk)
    int id = -1;
};

class TierManager {
public:
    // Lanza std::runtime_error si no se puede crear el userfaultfd o el backend.
    explicit TierManager(const Config& cfg);
    ~TierManager();
    TierManager(const TierManager&) = delete;
    TierManager& operator=(const TierManager&) = delete;

    // Mapea un archivo como tensor de solo lectura. Lanza std::runtime_error en error.
    TensorView map_tensor(const std::string& path);
    // Pide traer al nivel dispositivo [offset, offset+len) del tensor (asíncrono).
    void prefetch(const TensorView& t, std::size_t offset, std::size_t len);

    Stats stats() const;
    UffdMode uffd_mode() const;
    const char* backend_name() const;
    const Config& config() const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace tiermem
