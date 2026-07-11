#include "zip.h"
#include <fstream>
#include <sstream>
#include <chrono>

namespace amidisk {

ZipHandler::ZipHandler(const std::string& path) : ExternalExtractor(path) {}

bool ZipHandler::can_handle(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char buf[4];
    f.read(buf, 4);
    if (f.gcount() < 4) return false;
    std::string sig(buf, 4);
    return sig == "PK\x03\x04";
}

std::string ZipHandler::get_exe() const {
    if (cached_exe_.empty()) {
        cached_exe_ = find_executable({"unzip"});
    }
    return cached_exe_;
}

std::vector<std::string> ZipHandler::get_test_cmd() const {
    return {get_exe(), "-t", path_};
}

std::vector<std::string> ZipHandler::get_list_cmd() const {
    return {get_exe(), "-l", path_};
}

std::vector<std::string> ZipHandler::get_extract_cmd(const std::string& raw_name) const {
    return {get_exe(), "-p", path_, raw_name};
}

std::vector<ArchiveEntry> ZipHandler::parse_list_output(const std::string& output) const {
    std::vector<ArchiveEntry> entries;
    std::stringstream ss(output);
    std::string line;
    
    std::vector<std::string> lines;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    
    int start_idx = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("---------", 0) == 0) {
            start_idx = i + 1;
            break;
        }
    }
    
    if (start_idx == -1) return entries;
    
    for (size_t i = start_idx; i < lines.size(); ++i) {
        if (lines[i].rfind("---------", 0) == 0) break;
        
        std::stringstream ls(lines[i]);
        std::vector<std::string> parts;
        std::string part;
        while (ls >> part) {
            parts.push_back(part);
        }
        if (parts.size() < 4) continue;
        
        uint32_t size = 0;
        try { size = std::stoul(parts[0]); } catch (...) {}
        
        std::string name = parts[3];
        for (size_t j = 4; j < parts.size(); ++j) {
            name += " " + parts[j];
        }
        
        bool is_dir = false;
        if (!name.empty() && name.back() == '/') {
            is_dir = true;
            name.pop_back();
        }
        
        ArchiveEntry e;
        e.name = name;
        e.raw_name = name + (is_dir ? "/" : "");
        e.size = size;
        e.isdir = is_dir;
        e.mtime = std::chrono::system_clock::now();
        entries.push_back(e);
    }
    return entries;
}

} // namespace amidisk
