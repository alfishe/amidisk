/**
 * @file image_file.cpp
 * @brief Platform-optimized image file I/O implementation.
 *
 * Three I/O paths with conditional compilation:
 *
 * 1. USE_POSIX_IO (Unix/macOS):
 *    - pread()/pwrite() for Posix strategy - single syscall, no seek
 *    - mmap() for Mmap strategy - zero-copy, kernel-managed I/O
 *
 * 2. USE_WIN32_IO (Windows):
 *    - ReadFile/WriteFile with OVERLAPPED for positional I/O
 *    - Equivalent to POSIX pread/pwrite performance
 *
 * 3. Fstream fallback (all platforms):
 *    - C++ standard library, seek + read/write pattern
 *    - Higher overhead but maximum portability
 */

#include "image_file.h"
#include <filesystem>
#include <cstring>

#if USE_POSIX_IO
#include <sys/stat.h>
#include <cerrno>
#endif

namespace amidisk {

// ============================================================================
// Constructor / Destructor
// ============================================================================

ImageFileBlkDev::ImageFileBlkDev(const std::string& path, bool read_only, uint32_t block_bytes,
                                   IOStrategy strategy)
    : path_(path), read_only_(read_only), block_bytes_(block_bytes), size_bytes_(0), strategy_(strategy) {

// -----------------------------------------------------------------------------
// POSIX path (Unix/macOS): use file descriptors with pread/pwrite or mmap
// -----------------------------------------------------------------------------
#if USE_POSIX_IO
    if (strategy_ == IOStrategy::Posix || strategy_ == IOStrategy::Mmap) {
        int flags = read_only_ ? O_RDONLY : O_RDWR;
        if (!read_only_ && !std::filesystem::exists(path_)) {
            flags |= O_CREAT;
        }
        fd_ = ::open(path_.c_str(), flags, 0644);
        if (fd_ < 0) {
            throw BlockDeviceError("Failed to open image file: " + path_ + " (" + std::strerror(errno) + ")");
        }

        struct stat st;
        if (fstat(fd_, &st) == 0) {
            size_bytes_ = st.st_size;
        }

        // For Mmap strategy, attempt to map the file into memory
        // Falls back to Posix if mmap fails (e.g., empty file)
        if (strategy_ == IOStrategy::Mmap && size_bytes_ > 0) {
            setup_mmap();
        }
    } else

// -----------------------------------------------------------------------------
// Windows path: use HANDLE with ReadFile/WriteFile + OVERLAPPED
// -----------------------------------------------------------------------------
#elif USE_WIN32_IO
    if (strategy_ == IOStrategy::Posix) {
        // On Windows, "Posix" strategy means Win32 ReadFile/WriteFile with OVERLAPPED
        // This provides equivalent positional I/O without seek overhead
        DWORD access = read_only_ ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        DWORD creation = read_only_ ? OPEN_EXISTING : OPEN_ALWAYS;
        DWORD flags = FILE_ATTRIBUTE_NORMAL;

        handle_ = CreateFileA(
            path_.c_str(),
            access,
            FILE_SHARE_READ,  // Allow other readers
            nullptr,          // Default security
            creation,
            flags,
            nullptr           // No template
        );

        if (handle_ == INVALID_HANDLE_VALUE) {
            throw BlockDeviceError("Failed to open image file: " + path_);
        }

        // Get file size
        LARGE_INTEGER li;
        if (GetFileSizeEx(handle_, &li)) {
            size_bytes_ = static_cast<uint64_t>(li.QuadPart);
        }
    } else if (strategy_ == IOStrategy::Mmap) {
        // Mmap not yet implemented on Windows; fall back to Win32 positional I/O
        strategy_ = IOStrategy::Posix;
        // Recurse with corrected strategy (will hit the Posix branch above)
        *this = ImageFileBlkDev(path, read_only, block_bytes, strategy_);
        return;
    } else
#endif

// -----------------------------------------------------------------------------
// Fstream fallback: portable C++ standard library I/O
// -----------------------------------------------------------------------------
    {
        // Force fstream if we got here
        strategy_ = IOStrategy::Fstream;
        std::ios_base::openmode mode = std::ios::binary;
        if (read_only_) {
            mode |= std::ios::in;
        } else {
            mode |= std::ios::in | std::ios::out;
            // Create file if it doesn't exist
            if (!std::filesystem::exists(path_)) {
                std::ofstream create(path_, std::ios::binary);
                create.close();
            }
        }

        file_.open(path_, mode);
        if (!file_.is_open()) {
            throw BlockDeviceError("Failed to open image file: " + path_);
        }

        file_.seekg(0, std::ios::end);
        size_bytes_ = file_.tellg();
        file_.seekg(0, std::ios::beg);
    }

    infer_geometry();
}

ImageFileBlkDev::~ImageFileBlkDev() {
    close();
}

// ============================================================================
// POSIX mmap support
// ============================================================================

#if USE_POSIX_IO
void ImageFileBlkDev::setup_mmap() {
    if (mmap_ptr_ != nullptr) return;

    int prot = PROT_READ;
    if (!read_only_) prot |= PROT_WRITE;

    // MAP_SHARED ensures writes are visible to other processes and persisted
    mmap_ptr_ = static_cast<uint8_t*>(::mmap(nullptr, size_bytes_, prot, MAP_SHARED, fd_, 0));
    if (mmap_ptr_ == MAP_FAILED) {
        mmap_ptr_ = nullptr;
        // Fall back to POSIX pread/pwrite
        strategy_ = IOStrategy::Posix;
    } else {
        mmap_size_ = size_bytes_;
    }
}

void ImageFileBlkDev::teardown_mmap() {
    if (mmap_ptr_ != nullptr && mmap_ptr_ != MAP_FAILED) {
        ::munmap(mmap_ptr_, mmap_size_);
        mmap_ptr_ = nullptr;
        mmap_size_ = 0;
    }
}
#endif

// ============================================================================
// Geometry inference
// ============================================================================

void ImageFileBlkDev::infer_geometry() {
    uint64_t total_blocks = size_bytes_ / block_bytes_;
    geom_.cylinders = 0;
    geom_.heads = 1;
    geom_.sectors = 1;

    if (total_blocks == 0) return;

    // Recognize standard floppy geometry
    if (total_blocks == 1760) {
        geom_.cylinders = 80;
        geom_.heads = 2;
        geom_.sectors = 11;
        return;
    }

    // Default hard drive geometry (LBA translation)
    geom_.heads = 16;
    geom_.sectors = 63;
    geom_.cylinders = total_blocks / (geom_.heads * geom_.sectors);
    if (geom_.cylinders == 0 && total_blocks > 0) {
        geom_.cylinders = 1;
    }
}

// ============================================================================
// BlockDevice interface
// ============================================================================

Geometry ImageFileBlkDev::geometry() const {
    return geom_;
}

uint32_t ImageFileBlkDev::sector_size() const {
    return block_bytes_;
}

uint64_t ImageFileBlkDev::size_bytes() const {
    return size_bytes_;
}

// ============================================================================
// Read implementation
// ============================================================================

void ImageFileBlkDev::read(uint64_t offset, std::span<uint8_t> buffer) const {
    if (offset + buffer.size() > size_bytes_) {
        throw BlockDeviceError("Read past end of image file");
    }

#if USE_POSIX_IO
    // Mmap path: direct memory copy, no syscall
    if (strategy_ == IOStrategy::Mmap && mmap_ptr_ != nullptr) {
        std::memcpy(buffer.data(), mmap_ptr_ + offset, buffer.size());
        return;
    }

    // POSIX path: pread() - single syscall with offset, no seek required
    if (strategy_ == IOStrategy::Posix) {
        ssize_t n = ::pread(fd_, buffer.data(), buffer.size(), offset);
        if (n < 0 || static_cast<size_t>(n) != buffer.size()) {
            throw BlockDeviceError("Read error on image file");
        }
        return;
    }

#elif USE_WIN32_IO
    // Win32 path: ReadFile with OVERLAPPED for positional I/O
    if (strategy_ == IOStrategy::Posix && handle_ != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

        DWORD bytes_read = 0;
        if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, &ov)) {
            throw BlockDeviceError("Read error on image file");
        }
        if (bytes_read != buffer.size()) {
            throw BlockDeviceError("Incomplete read on image file");
        }
        return;
    }
#endif

    // Fstream fallback: seek + read (higher overhead)
    file_.seekg(offset, std::ios::beg);
    if (!file_.read(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        throw BlockDeviceError("Read error on image file");
    }
}

// ============================================================================
// Write implementation
// ============================================================================

void ImageFileBlkDev::write(uint64_t offset, std::span<const uint8_t> buffer) {
    if (read_only_) {
        throw BlockDeviceError("Image file is read-only");
    }

    // Auto-extend file if writing past current end
    bool need_extend = offset + buffer.size() > size_bytes_;
    if (need_extend) {
        size_bytes_ = offset + buffer.size();
        infer_geometry();

#if USE_POSIX_IO
        // Extend file with ftruncate
        if (fd_ >= 0) {
            if (::ftruncate(fd_, size_bytes_) < 0) {
                throw BlockDeviceError("Failed to extend file");
            }
            // Re-establish mmap for new size
            if (strategy_ == IOStrategy::Mmap) {
                teardown_mmap();
                setup_mmap();
            }
        }
#elif USE_WIN32_IO
        // Extend file by seeking and setting end-of-file
        if (handle_ != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(size_bytes_);
            if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN) ||
                !SetEndOfFile(handle_)) {
                throw BlockDeviceError("Failed to extend file");
            }
        }
#endif
    }

#if USE_POSIX_IO
    // Mmap path: direct memory copy, kernel handles writeback
    if (strategy_ == IOStrategy::Mmap && mmap_ptr_ != nullptr) {
        std::memcpy(mmap_ptr_ + offset, buffer.data(), buffer.size());
        return;
    }

    // POSIX path: pwrite() - single syscall with offset, no seek required
    if (strategy_ == IOStrategy::Posix) {
        ssize_t n = ::pwrite(fd_, buffer.data(), buffer.size(), offset);
        if (n < 0 || static_cast<size_t>(n) != buffer.size()) {
            throw BlockDeviceError("Write error on image file");
        }
        return;
    }

#elif USE_WIN32_IO
    // Win32 path: WriteFile with OVERLAPPED for positional I/O
    if (strategy_ == IOStrategy::Posix && handle_ != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

        DWORD bytes_written = 0;
        if (!WriteFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_written, &ov)) {
            throw BlockDeviceError("Write error on image file");
        }
        if (bytes_written != buffer.size()) {
            throw BlockDeviceError("Incomplete write on image file");
        }
        return;
    }
#endif

    // Fstream fallback: seek + write (higher overhead)
    file_.seekp(offset, std::ios::beg);
    if (!file_.write(reinterpret_cast<const char*>(buffer.data()), buffer.size())) {
        throw BlockDeviceError("Write error on image file");
    }
}

// ============================================================================
// Flush implementation
// ============================================================================

void ImageFileBlkDev::flush() {
#if USE_POSIX_IO
    // Mmap: msync forces dirty pages to disk
    if (strategy_ == IOStrategy::Mmap && mmap_ptr_ != nullptr) {
        ::msync(mmap_ptr_, mmap_size_, MS_SYNC);
        return;
    }

    // POSIX: fsync flushes kernel buffers to disk
    if (strategy_ == IOStrategy::Posix && fd_ >= 0) {
        ::fsync(fd_);
        return;
    }

#elif USE_WIN32_IO
    // Win32: FlushFileBuffers
    if (handle_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(handle_);
        return;
    }
#endif

    // Fstream fallback
    if (file_.is_open()) {
        file_.flush();
    }
}

// ============================================================================
// Close implementation
// ============================================================================

void ImageFileBlkDev::close() {
#if USE_POSIX_IO
    teardown_mmap();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#elif USE_WIN32_IO
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
#endif

    if (file_.is_open()) {
        file_.close();
    }
}

} // namespace amidisk
