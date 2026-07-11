#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include "../fs/volume.h"

namespace amidisk {

struct ArchiveEntry {
    std::string name;
    std::string raw_name;
    uint32_t size;
    bool isdir;
    std::chrono::system_clock::time_point mtime;
};

class ArchiveHandler {
public:
    virtual ~ArchiveHandler() = default;

    virtual bool test_archive() = 0;
    
    // Returns (num_files_streamed, num_dirs_created, total_bytes_streamed)
    virtual std::tuple<uint32_t, uint32_t, uint64_t> stream_to_volume(
        Volume* vol, 
        const std::string& base_amiga_path, 
        uint32_t max_len, 
        uint32_t protect, 
        const std::string& comment
    ) = 0;

protected:
    std::string path_;
    explicit ArchiveHandler(const std::string& path) : path_(path) {}
    
    // Helper to find executable in PATH
    static std::string find_executable(const std::vector<std::string>& names);
    
    // Helper to run a command and capture stdout
    static std::string run_command(const std::vector<std::string>& cmd, bool& success, std::string& stderr_out);
    
    // Helper to run a command and return output as binary vector
    static std::vector<uint8_t> run_command_binary(const std::vector<std::string>& cmd, bool& success, std::string& stderr_out);
};

class ExternalExtractor : public ArchiveHandler {
public:
    explicit ExternalExtractor(const std::string& path) : ArchiveHandler(path) {}
    
    bool test_archive() override;
    
    std::tuple<uint32_t, uint32_t, uint64_t> stream_to_volume(
        Volume* vol, 
        const std::string& base_amiga_path, 
        uint32_t max_len, 
        uint32_t protect, 
        const std::string& comment
    ) override;

protected:
    virtual std::string get_exe() const = 0;
    virtual std::vector<std::string> get_test_cmd() const = 0;
    virtual std::vector<std::string> get_list_cmd() const = 0;
    virtual std::vector<std::string> get_extract_cmd(const std::string& raw_name) const = 0;
    virtual std::vector<ArchiveEntry> parse_list_output(const std::string& output) const = 0;
};

// Factory to get appropriate handler
std::unique_ptr<ArchiveHandler> create_archive_handler(const std::string& path);

} // namespace amidisk
