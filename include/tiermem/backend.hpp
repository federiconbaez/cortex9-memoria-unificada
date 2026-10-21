// Interfaz de almacenamiento (nivel NVMe) para tiermem.
// Un backend lee bloques alineados de un archivo hacia un buffer en RAM de host.
// El backend CUDA/cuFile (GPUDirect Storage) se enchufaría aquí; ver
// src/backend_cufile.cpp (NO compilado, NO verificado).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace tiermem {

struct ReadRequest {
    int handle = -1;          // devuelto por StorageBackend::open_file
    std::uint64_t offset = 0; // alineado a kAlign
    std::size_t length = 0;   // múltiplo de kAlign
    void* dst = nullptr;      // alineado a kAlign
    long result = 0;          // bytes leídos (>=0) o -errno
};

class StorageBackend {
public:
    static constexpr std::size_t kAlign = 4096;
    virtual ~StorageBackend() = default;
    virtual const char* name() const = 0;
    // Abre un archivo de solo lectura. Devuelve handle >= 0 o -errno.
    virtual int open_file(const std::string& path) = 0;
    virtual void close_file(int handle) = 0;
    virtual std::uint64_t file_size(int handle) const = 0;
    // Ejecuta un lote de lecturas. Rellena req.result en cada una.
    // Implementaciones asíncronas (io_uring) envían el lote completo de una vez.
    virtual void read_batch(std::span<ReadRequest> reqs) = 0;
};

enum class BackendKind { Auto, Pread, IoUring };

// Crea un backend. direct_io=true abre los archivos con O_DIRECT (sin page cache).
// Devuelve nullptr si el backend pedido no está disponible (p.ej. io_uring deshabilitado).
std::unique_ptr<StorageBackend> make_backend(BackendKind kind, bool direct_io);

// true si el binario se compiló con liburing.
bool iouring_compiled_in();

}  // namespace tiermem
