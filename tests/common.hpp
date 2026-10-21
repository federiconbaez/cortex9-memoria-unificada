#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tt {

inline std::uint64_t splitmix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
// Contenido esperado del byte en 'off' para un archivo con semilla 'seed'.
inline std::uint8_t expected(std::uint64_t seed, std::uint64_t off) {
    return static_cast<std::uint8_t>(splitmix(seed ^ (off / 8)) >> (8 * (off % 8)));
}
// Archivos temporales de las pruebas: se borran al salir del proceso.
inline std::vector<std::string>& temp_files() {
    static std::vector<std::string> v;
    return v;
}
inline void remove_temp_files() {
    for (auto& p : temp_files()) ::unlink(p.c_str());
}

// Crea un archivo de 'size' bytes con contenido determinista. Devuelve la ruta.
// Con un nombre relativo, el archivo lleva el PID (varias corridas simultáneas en el mismo
// directorio no se pisan) y se borra al terminar. Una ruta absoluta se usa tal cual
// (el benchmark reutiliza su archivo).
inline std::string make_file(const std::string& name, std::uint64_t size, std::uint64_t seed) {
    const char* dir = std::getenv("TIERMEM_TEST_DIR");
    std::string path = name.starts_with("/")
                           ? name
                           : std::string(dir ? dir : ".") + "/" + name + "." + std::to_string(::getpid());
    if (!name.starts_with("/")) {
        if (temp_files().empty()) std::atexit(remove_temp_files);
        temp_files().push_back(path);
    }
    int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) { std::perror("open"); std::exit(2); }
    std::vector<std::uint8_t> buf(1 << 20);
    for (std::uint64_t off = 0; off < size; off += buf.size()) {
        std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - off));
        for (std::size_t i = 0; i < n; ++i) buf[i] = expected(seed, off + i);
        if (::write(fd, buf.data(), n) != static_cast<ssize_t>(n)) { std::perror("write"); std::exit(2); }
    }
    ::fsync(fd);
    ::close(fd);
    return path;
}

}  // namespace tt
