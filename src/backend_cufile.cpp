// =====================================================================
//  Backend CUDA / cuFile (GPUDirect Storage)  —  NO VERIFICADO
// =====================================================================
// Este archivo NUNCA se compiló ni se ejecutó: en la máquina donde se
// desarrolló no hay GPU, ni driver NVIDIA, ni CUDA, ni libcufile.
// Por defecto CMake NO lo incluye (opción TIERMEM_WITH_CUFILE=OFF).
//
// Qué haría: leer bloques del archivo directamente a memoria de GPU
// (cudaMalloc) con cuFileRead, sin rebote por RAM de host, si el sistema
// tiene nvidia-fs y un sistema de archivos compatible con GDS.
//
// Limitación de diseño importante (ver README): el driver NVIDIA (UVM)
// no expone una forma de reemplazar el manejador de fallos de página de
// la GPU. Por eso en una GPU real este backend se usaría con residencia
// EXPLÍCITA (TierManager::prefetch antes de lanzar el kernel), no con
// fallos interceptados como hace userfaultfd en la CPU.
// =====================================================================
#ifdef TIERMEM_WITH_CUFILE
#include <cufile.h>
#include <cuda_runtime.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "tiermem/backend.hpp"

namespace tiermem {

class CuFileBackend final : public StorageBackend {
public:
    CuFileBackend() { ok_ = cuFileDriverOpen().err == CU_FILE_SUCCESS; }
    ~CuFileBackend() override {
        for (auto& f : files_) {
            cuFileHandleDeregister(f.h);
            ::close(f.fd);
        }
        if (ok_) cuFileDriverClose();
    }
    const char* name() const override { return "cuFile (GDS) [NO VERIFICADO]"; }
    int open_file(const std::string& path) override {
        int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
        if (fd < 0) return -1;
        CUfileDescr_t d{};
        d.handle.fd = fd;
        d.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;
        CUfileHandle_t h;
        if (cuFileHandleRegister(&h, &d).err != CU_FILE_SUCCESS) { ::close(fd); return -1; }
        struct stat st {};
        fstat(fd, &st);
        files_.push_back({fd, h, static_cast<std::uint64_t>(st.st_size)});
        return static_cast<int>(files_.size() - 1);
    }
    void close_file(int) override {}
    std::uint64_t file_size(int h) const override { return files_[h].size; }
    // ATENCIÓN: aquí req.dst debe ser un puntero de memoria de GPU.
    void read_batch(std::span<ReadRequest> reqs) override {
        for (auto& r : reqs)
            r.result = cuFileRead(files_[r.handle].h, r.dst, r.length, static_cast<off_t>(r.offset), 0);
    }
private:
    struct F { int fd; CUfileHandle_t h; std::uint64_t size; };
    std::vector<F> files_;
    bool ok_ = false;
};

}  // namespace tiermem
#endif  // TIERMEM_WITH_CUFILE
