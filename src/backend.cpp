#include "tiermem/backend.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <vector>

#ifdef TIERMEM_HAVE_LIBURING
#include <liburing.h>
#endif

namespace tiermem {
namespace {

struct FileTable {
    std::mutex mu;
    std::vector<int> fds;
    std::vector<std::uint64_t> sizes;

    int add(int fd, std::uint64_t size) {
        std::lock_guard<std::mutex> lk(mu);
        fds.push_back(fd);
        sizes.push_back(size);
        return static_cast<int>(fds.size() - 1);
    }
    int fd(int h) {
        std::lock_guard<std::mutex> lk(mu);
        return (h >= 0 && static_cast<std::size_t>(h) < fds.size()) ? fds[h] : -1;
    }
    std::uint64_t size(int h) {
        std::lock_guard<std::mutex> lk(mu);
        return (h >= 0 && static_cast<std::size_t>(h) < sizes.size()) ? sizes[h] : 0;
    }
    void close(int h) {
        std::lock_guard<std::mutex> lk(mu);
        if (h >= 0 && static_cast<std::size_t>(h) < fds.size() && fds[h] >= 0) {
            ::close(fds[h]);
            fds[h] = -1;
        }
    }
    ~FileTable() {
        for (int f : fds)
            if (f >= 0) ::close(f);
    }
};

int open_ro(const std::string& path, bool direct, std::uint64_t* size_out) {
    int flags = O_RDONLY | O_CLOEXEC | (direct ? O_DIRECT : 0);
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) return -errno;
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        int e = errno;
        ::close(fd);
        return -e;
    }
    *size_out = static_cast<std::uint64_t>(st.st_size);
    return fd;
}

class PreadBackend final : public StorageBackend {
public:
    explicit PreadBackend(bool direct) : direct_(direct) {}
    const char* name() const override { return direct_ ? "pread+O_DIRECT" : "pread"; }
    int open_file(const std::string& path) override {
        std::uint64_t sz = 0;
        int fd = open_ro(path, direct_, &sz);
        if (fd < 0) return fd;
        return files_.add(fd, sz);
    }
    void close_file(int h) override { files_.close(h); }
    std::uint64_t file_size(int h) const override { return files_.size(h); }
    void read_batch(std::span<ReadRequest> reqs) override {
        for (auto& r : reqs) {
            int fd = files_.fd(r.handle);
            if (fd < 0) { r.result = -EBADF; continue; }
            std::size_t done = 0;
            long err = 0;
            while (done < r.length) {
                ssize_t n = ::pread(fd, static_cast<char*>(r.dst) + done, r.length - done,
                                    static_cast<off_t>(r.offset + done));
                if (n < 0) {
                    if (errno == EINTR) continue;
                    err = -errno;
                    break;
                }
                if (n == 0) break;  // EOF
                done += static_cast<std::size_t>(n);
                // Con O_DIRECT una lectura corta que no es EOF debe seguir alineada.
                if (direct_ && (done % kAlign) != 0) break;
            }
            r.result = (done == 0 && err != 0) ? err : static_cast<long>(done);
        }
    }
private:
    bool direct_;
    mutable FileTable files_;
};

#ifdef TIERMEM_HAVE_LIBURING
class IoUringBackend final : public StorageBackend {
public:
    IoUringBackend(bool direct, int* err) : direct_(direct) {
        int e = io_uring_queue_init(kDepth, &ring_, 0);
        if (e < 0) { *err = e; return; }
        ok_ = true;
        *err = 0;
    }
    ~IoUringBackend() override {
        if (ok_) io_uring_queue_exit(&ring_);
    }
    const char* name() const override { return direct_ ? "io_uring+O_DIRECT" : "io_uring"; }
    int open_file(const std::string& path) override {
        std::uint64_t sz = 0;
        int fd = open_ro(path, direct_, &sz);
        if (fd < 0) return fd;
        return files_.add(fd, sz);
    }
    void close_file(int h) override { files_.close(h); }
    std::uint64_t file_size(int h) const override { return files_.size(h); }
    void read_batch(std::span<ReadRequest> reqs) override {
        std::lock_guard<std::mutex> lk(mu_);  // el anillo no es thread-safe
        std::size_t i = 0;
        while (i < reqs.size()) {
            std::size_t n = std::min<std::size_t>(kDepth, reqs.size() - i);
            for (std::size_t k = 0; k < n; ++k) {
                ReadRequest& r = reqs[i + k];
                io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                int fd = files_.fd(r.handle);
                io_uring_prep_read(sqe, fd, r.dst, static_cast<unsigned>(r.length), r.offset);
                io_uring_sqe_set_data64(sqe, i + k);
            }
            int s = io_uring_submit(&ring_);
            if (s < 0) {
                for (std::size_t k = 0; k < n; ++k) reqs[i + k].result = s;
                i += n;
                continue;
            }
            for (std::size_t k = 0; k < n; ++k) {
                io_uring_cqe* cqe = nullptr;
                int w;
                do { w = io_uring_wait_cqe(&ring_, &cqe); } while (w == -EINTR);
                if (w < 0) break;
                reqs[io_uring_cqe_get_data64(cqe)].result = cqe->res;
                io_uring_cqe_seen(&ring_, cqe);
            }
            // Lecturas cortas (poco probables en archivos regulares): completar con pread.
            for (std::size_t k = 0; k < n; ++k) {
                ReadRequest& r = reqs[i + k];
                if (r.result < 0 || static_cast<std::size_t>(r.result) >= r.length) continue;
                if (r.result % static_cast<long>(kAlign) != 0) continue;  // llegó a EOF
                int fd = files_.fd(r.handle);
                std::size_t done = static_cast<std::size_t>(r.result);
                while (done < r.length) {
                    ssize_t m = ::pread(fd, static_cast<char*>(r.dst) + done, r.length - done,
                                        static_cast<off_t>(r.offset + done));
                    if (m <= 0) break;
                    done += static_cast<std::size_t>(m);
                }
                r.result = static_cast<long>(done);
            }
            i += n;
        }
    }
    bool ok() const { return ok_; }
private:
    static constexpr unsigned kDepth = 64;
    bool direct_;
    bool ok_ = false;
    io_uring ring_{};
    std::mutex mu_;
    mutable FileTable files_;
};
#endif

}  // namespace

bool iouring_compiled_in() {
#ifdef TIERMEM_HAVE_LIBURING
    return true;
#else
    return false;
#endif
}

std::unique_ptr<StorageBackend> make_backend(BackendKind kind, bool direct_io) {
#ifdef TIERMEM_HAVE_LIBURING
    if (kind == BackendKind::IoUring || kind == BackendKind::Auto) {
        int err = 0;
        auto b = std::make_unique<IoUringBackend>(direct_io, &err);
        if (b->ok()) return b;
        if (kind == BackendKind::IoUring) return nullptr;
    }
#else
    if (kind == BackendKind::IoUring) return nullptr;
#endif
    return std::make_unique<PreadBackend>(direct_io);
}

}  // namespace tiermem
