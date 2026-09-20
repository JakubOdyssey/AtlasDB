#include <algorithm>
#include <atlas/storage/file.hpp>
#include <limits>
#include <mutex>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace atlas {
struct File::Impl {
    std::filesystem::path path;
    mutable std::mutex mutex;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
    ~Impl() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
#else
        if (handle >= 0)
            ::close(handle);
#endif
    }
};
namespace {
[[noreturn]] void io_error(const std::filesystem::path &p, const char *operation) {
#ifdef _WIN32
    const auto error = GetLastError();
    throw IoError(p.string() + ": " + operation + " (Win32 " + std::to_string(error) + ")");
#else
    const auto error = errno;
    throw IoError(p.string() + ": " + operation + " (" + std::string(std::strerror(error)) + ")");
#endif
}
void valid_offset(std::uint64_t offset, std::size_t length = 0) {
    const auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if (length > maximum || offset > maximum - length)
        throw LimitError("file offset overflow");
}
} // namespace
File::File(const std::filesystem::path &path, bool create, bool exclusive_create)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
#ifdef _WIN32
    impl_->handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    exclusive_create ? CREATE_NEW : (create ? OPEN_ALWAYS : OPEN_EXISTING),
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE)
        io_error(path, "open/exclusive lock");
#else
    impl_->handle =
        ::open(path.c_str(),
               O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0) | (exclusive_create ? O_EXCL : 0), 0600);
    if (impl_->handle < 0)
        io_error(path, "open");
    if (flock(impl_->handle, LOCK_EX | LOCK_NB) < 0) {
        io_error(path, "exclusive lock");
    }
    if (create) {
        auto parent = path.parent_path();
        if (parent.empty())
            parent = ".";
        int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) {
            io_error(parent, "open parent directory");
        }
        const int result = fsync(directory);
        const int e = errno;
        ::close(directory);
        if (result < 0) {
            errno = e;
            io_error(parent, "sync parent directory");
        }
    }
#endif
}
File::~File() = default;
std::uint64_t File::size() const {
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    LARGE_INTEGER result{};
    if (!GetFileSizeEx(impl_->handle, &result))
        io_error(impl_->path, "size");
    return static_cast<std::uint64_t>(result.QuadPart);
#else
    struct stat st{};
    if (fstat(impl_->handle, &st) < 0)
        io_error(impl_->path, "size");
    return static_cast<std::uint64_t>(st.st_size);
#endif
}
void File::read(std::uint64_t offset, std::span<std::byte> bytes) const {
    valid_offset(offset, bytes.size());
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN))
        io_error(impl_->path, "seek read");
#endif
    std::size_t done = 0;
    while (done < bytes.size()) {
        auto count = std::min<std::size_t>(bytes.size() - done, 1U << 30);
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(impl_->handle, bytes.data() + done, static_cast<DWORD>(count), &got, nullptr))
            io_error(impl_->path, "read");
#else
        auto got =
            pread(impl_->handle, bytes.data() + done, count, static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            io_error(impl_->path, "read");
        }
#endif
        if (got == 0)
            throw IoError(impl_->path.string() + ": unexpected short read at " +
                          std::to_string(offset + done));
        done += static_cast<std::size_t>(got);
    }
}
void File::write(std::uint64_t offset, std::span<const std::byte> bytes) {
    valid_offset(offset, bytes.size());
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN))
        io_error(impl_->path, "seek write");
#endif
    std::size_t done = 0;
    while (done < bytes.size()) {
        auto count = std::min<std::size_t>(bytes.size() - done, 1U << 30);
#ifdef _WIN32
        DWORD put = 0;
        if (!WriteFile(impl_->handle, bytes.data() + done, static_cast<DWORD>(count), &put,
                       nullptr))
            io_error(impl_->path, "write");
#else
        auto put =
            pwrite(impl_->handle, bytes.data() + done, count, static_cast<off_t>(offset + done));
        if (put < 0) {
            if (errno == EINTR)
                continue;
            io_error(impl_->path, "write");
        }
#endif
        if (put == 0)
            throw IoError(impl_->path.string() + ": zero-length write");
        done += static_cast<std::size_t>(put);
    }
}
void File::resize(std::uint64_t bytes) {
    valid_offset(bytes);
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFilePointerEx(impl_->handle, pos, nullptr, FILE_BEGIN) || !SetEndOfFile(impl_->handle))
        io_error(impl_->path, "resize");
#else
    if (ftruncate(impl_->handle, static_cast<off_t>(bytes)) < 0)
        io_error(impl_->path, "resize");
#endif
}
void File::sync() {
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    if (!FlushFileBuffers(impl_->handle))
        io_error(impl_->path, "durability barrier");
#else
    if (fsync(impl_->handle) < 0)
        io_error(impl_->path, "durability barrier");
#endif
}
} // namespace atlas
