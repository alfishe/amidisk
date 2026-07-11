#include "sevenz.h"
#include <fstream>
#include <sstream>
#include <chrono>

namespace amidisk {

SevenZHandler::SevenZHandler(const std::string& path) : ExternalExtractor(path) {}

bool SevenZHandler::can_handle(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char buf[6];
    f.read(buf, 6);
    if (f.gcount() < 6) return false;
    std::string sig(buf, 6);
    return sig == "7z\xbc\xaf\x27\x1c";
}

std::string SevenZHandler::get_exe() const {
    if (cached_exe_.empty()) {
        cached_exe_ = find_executable({"7z", "7za"});
    }
    return cached_exe_;
}

std::vector<std::string> SevenZHandler::get_test_cmd() const {
    return {get_exe(), "t", path_};
}

std::vector<std::string> SevenZHandler::get_list_cmd() const {
    return {get_exe(), "l", "-slt", path_};
}

std::vector<std::string> SevenZHandler::get_extract_cmd(const std::string& raw_name) const {
    return {get_exe(), "x", "-so", path_, raw_name};
}

std::vector<ArchiveEntry> SevenZHandler::parse_list_output(const std::string& output) const {
    std::vector<ArchiveEntry> entries;
    std::stringstream ss(output);
    std::string line;
    
    std::string current_path;
    bool is_dir = false;
    uint32_t size = 0;
    
    while (std::getline(ss, line)) {
        if (line.empty() || line == "\r") {
            if (!current_path.empty()) {
                ArchiveEntry e;
                e.name = current_path;
                e.raw_name = current_path;
                e.size = size;
                e.isdir = is_dir;
                e.mtime = std::chrono::system_clock::now();
                entries.push_back(e);
                current_path.clear();
                size = 0;
                is_dir = false;
            }
            continue;
        }
        if (line.rfind("Path = ", 0) == 0) {
            current_path = line.substr(7);
            if (!current_path.empty() && current_path.back() == '\r') current_path.pop_back();
        } else if (line.rfind("Folder = ", 0) == 0) {
            is_dir = (line.find("+") != std::string::npos);
        } else if (line.rfind("Size = ", 0) == 0) {
            try { size = std::stoul(line.substr(7)); } catch (...) { size = 0; }
        }
    }
    if (!current_path.empty()) {
        ArchiveEntry e;
        e.name = current_path;
        e.raw_name = current_path;
        e.size = size;
        e.isdir = is_dir;
        e.mtime = std::chrono::system_clock::now();
        entries.push_back(e);
    }
    return entries;
}

} // namespace amidisk
