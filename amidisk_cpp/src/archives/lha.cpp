#include "lha.h"
#include <fstream>
#include <sstream>
#include <chrono>

namespace amidisk {

LhaHandler::LhaHandler(const std::string& path) : ExternalExtractor(path) {}

bool LhaHandler::can_handle(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char buf[21];
    f.read(buf, 21);
    if (f.gcount() < 21) return false;
    std::string sig(buf + 2, 5);
    return sig == "-lh0-" || sig == "-lh1-" || sig == "-lh2-" || 
           sig == "-lh3-" || sig == "-lh4-" || sig == "-lh5-" || 
           sig == "-lh6-" || sig == "-lh7-" || sig == "-lhd-";
}

std::string LhaHandler::get_exe() const {
    if (cached_exe_.empty()) {
        cached_exe_ = find_executable({"lha", "7z", "7za"});
        if (cached_exe_.find("7z") != std::string::npos) {
            is_7z_ = true;
        }
    }
    return cached_exe_;
}

std::vector<std::string> LhaHandler::get_test_cmd() const {
    return {get_exe(), "t", path_};
}

std::vector<std::string> LhaHandler::get_list_cmd() const {
    if (is_7z_) {
        return {get_exe(), "l", "-slt", path_};
    } else {
        return {get_exe(), "v", path_};
    }
}

std::vector<std::string> LhaHandler::get_extract_cmd(const std::string& raw_name) const {
    if (is_7z_) {
        return {get_exe(), "x", "-so", path_, raw_name};
    } else {
        return {get_exe(), "pq", path_, raw_name};
    }
}

std::vector<ArchiveEntry> LhaHandler::parse_list_output(const std::string& output) const {
    std::vector<ArchiveEntry> entries;
    std::stringstream ss(output);
    std::string line;
    
    if (is_7z_) {
        // Simple 7z parse
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
                    e.mtime = std::chrono::system_clock::now(); // Skip parsing date for now
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
    } else {
        // Lha parse
        std::vector<std::string> lines;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        
        std::vector<size_t> borders;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].rfind("----------", 0) == 0) borders.push_back(i);
        }
        if (borders.size() < 2) return entries;
        
        for (size_t i = borders[0] + 1; i < borders[1]; ++i) {
            std::stringstream ls(lines[i]);
            std::vector<std::string> parts;
            std::string part;
            while (ls >> part) {
                parts.push_back(part);
            }
            if (parts.size() < 7) continue;
            
            std::string perm = parts[0];
            bool is_dir = (perm[0] == 'd' || perm == "[dir]");
            uint32_t size = 0;
            try { size = std::stoul(parts[3]); } catch (...) {}
            
            std::string name = parts.back();
            if (!name.empty() && name.back() == '/') {
                is_dir = true;
                name.pop_back();
            }
            
            ArchiveEntry e;
            e.name = name;
            e.raw_name = name;
            e.size = size;
            e.isdir = is_dir;
            e.mtime = std::chrono::system_clock::now();
            entries.push_back(e);
        }
    }
    return entries;
}

} // namespace amidisk
