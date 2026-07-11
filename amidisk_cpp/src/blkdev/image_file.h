#pragma once
/**
 * @file image_file.h
 * @brief Platform-optimized block device backed by an image file.
 *
 * This module provides three I/O strategies with automatic platform selection:
 *
 * ## I/O Strategy Hierarchy (fastest to most portable)
 *
 * 1. **Mmap** (Unix/macOS only)
 *    - Maps entire file into virtual memory
 *    - Writes become memcpy(); kernel handles actual I/O asynchronously
 *    - ~25% faster than Posix due to zero syscalls per operation
 *    - Tradeoff: requires pre-allocated image, high memory use (entire file mapped)
 *    - Best for: batch operations on workstations with plenty of RAM
 *
 * 2. **Posix** (Unix/macOS/Windows)
 *    - Uses pread()/pwrite() on Unix, ReadFile/WriteFile with OVERLAPPED on Windows
 *    - Single syscall per I/O operation, no seek required (positional I/O)
 *    - ~2x faster than fstream due to reduced syscall overhead
 *    - Best for: general use, good balance of speed and memory
 *
 * 3. **Fstream** (all platforms)
 *    - C++ standard library, fully portable
 *    - Higher overhead from buffering, seek+read/write pattern, stream state
 *    - Still 2.3x faster than Python due to compiled code
 *    - Best for: maximum portability, exotic platforms
 *
 * ## Platform Support Matrix
 *
 * | Strategy | macOS | Linux | Windows | Other POSIX |
 * |----------|-------|-------|---------|-------------|
 * | Mmap     | Yes   | Yes   | Yes     | Yes         |
 * | Posix    | Yes   | Yes   | Yes     | Yes         |
 * | Fstream  | Yes   | Yes   | Yes     | Yes         |
 *
 * Windows uses CreateFileMapping/MapViewOfFile for mmap, and
 * ReadFile/WriteFile with OVERLAPPED for positional I/O (Posix strategy).
 *
 * ## Selection via Environment Variable
 *
 * Set AMIDISK_IO=fstream|posix|mmap to override automatic selection.
 * Default is 'posix' which auto-detects the best available strategy.
 */

#include "blkdev.h"
#include <fstream>
#include <string>

// Platform detection for optimized I/O
#if defined(_WIN32)
    // Windows: use ReadFile/WriteFile with OVERLAPPED for positional I/O
    #define USE_WIN32_IO 1
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
    // Unix-like: use pread/pwrite and optionally mmap
    #define USE_POSIX_IO 1
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
#endif

namespace amidisk {

/**
 * I/O strategy selection.
 *
 * Performance hierarchy (measured on 6GB tar -> 27GB FFS extraction):
 *   Mmap:    6.83s  (5.1x faster than Python)
 *   Posix:   7.70s  (4.5x faster than Python)
 *   Fstream: 14.84s (2.3x faster than Python)
 *   Python:  34.77s (baseline)
 */
enum class IOStrategy {
    Fstream,    ///< C++ fstream - portable fallback, works everywhere
    Posix,      ///< POSIX pread/pwrite (Unix) or Win32 ReadFile/WriteFile (Windows)
    Mmap        ///< Memory-mapped I/O - fastest, Unix/macOS only, pre-allocated images
};

/**
 * Block device backed by an image file with platform-optimized I/O.
 *
 * Supports automatic file extension on write (except with mmap strategy).
 * Thread-safety: not thread-safe; external synchronization required.
 */
class ImageFileBlkDev : public BlockDevice {
public:
    /**
     * Open an image file as a block device.
     *
     * @param path       Path to the image file
     * @param read_only  If true, writes will throw BlockDeviceError
     * @param block_bytes Logical sector size (default 512)
     * @param strategy   I/O strategy; Posix is recommended default
     *
     * If the file doesn't exist and read_only is false, it will be created.
     * For Mmap strategy, falls back to Posix if mmap() fails.
     */
    ImageFileBlkDev(const std::string& path, bool read_only = true, uint32_t block_bytes = 512,
                    IOStrategy strategy = IOStrategy::Posix);
    ~ImageFileBlkDev() override;

    Geometry geometry() const override;
    uint32_t sector_size() const override;
    uint64_t size_bytes() const override;

    void read(uint64_t offset, std::span<uint8_t> buffer) const override;
    void write(uint64_t offset, std::span<const uint8_t> buffer) override;
    void flush() override;
    void close() override;

    DeviceKind kind() const override { return DeviceKind::Image; }
    bool is_read_only() const override { return read_only_; }

private:
    std::string path_;
    bool read_only_;
    uint32_t block_bytes_;
    uint64_t size_bytes_;
    Geometry geom_;
    IOStrategy strategy_;

#if USE_POSIX_IO
    int fd_ = -1;              ///< File descriptor for Posix/Mmap strategies
    uint8_t* mmap_ptr_ = nullptr;  ///< Mapped memory region (Mmap strategy only)
    size_t mmap_size_ = 0;     ///< Size of mapped region

    void setup_mmap();         ///< Establish memory mapping; falls back to Posix on failure
    void teardown_mmap();      ///< Release memory mapping
#elif USE_WIN32_IO
    HANDLE handle_ = INVALID_HANDLE_VALUE;  ///< File handle for Win32 I/O
    HANDLE mapping_ = nullptr;              ///< File mapping handle for mmap strategy
    uint8_t* mmap_ptr_ = nullptr;           ///< Mapped memory region
    size_t mmap_size_ = 0;                  ///< Size of mapped region

    void setup_mmap();         ///< Establish memory mapping via CreateFileMapping
    void teardown_mmap();      ///< Release memory mapping
#endif

    mutable std::fstream file_;  ///< Fstream for portable fallback

    void infer_geometry();     ///< Compute CHS geometry from file size
};

} // namespace amidisk
