#include "rar.h"
#include <fstream>
#include <sstream>
#include <chrono>

namespace amidisk {

RarHandler::RarHandler(const std::string& path) : ExternalExtractor(path) {}

bool RarHandler::can_handle(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char buf[7];
    f.read(buf, 7);
    if (f.gcount() < 7) return false;
    std::string sig(buf, 7);
    return sig == "Rar!\x1a\x07\x00" || sig == "Rar!\x1a\x07\x01";
}

std::string RarHandler::get_exe() const {
    if (cached_exe_.empty()) {
        cached_exe_ = find_executable({"7z", "7za", "unrar"});
        if (cached_exe_.find("unrar") != std::string::npos) {
            exe_type_ = "unrar";
        } else {
            exe_type_ = "7z";
        }
    }
    return cached_exe_;
}

std::vector<std::string> RarHandler::get_test_cmd() const {
    return {get_exe(), "t", path_};
}

std::vector<std::string> RarHandler::get_list_cmd() const {
    if (exe_type_ == "7z") {
        return {get_exe(), "l", "-slt", path_};
    } else {
        return {get_exe(), "v", path_};
    }
}

std::vector<std::string> RarHandler::get_extract_cmd(const std::string& raw_name) const {
    if (exe_type_ == "7z") {
        return {get_exe(), "x", "-so", path_, raw_name};
    } else {
        return {get_exe(), "p", "-inul", path_, raw_name};
    }
}

std::vector<ArchiveEntry> RarHandler::parse_list_output(const std::string& output) const {
    std::vector<ArchiveEntry> entries;
    std::stringstream ss(output);
    std::string line;
    
    if (exe_type_ == "7z") {
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
    } else {
        // unrar parse
        std::vector<std::string> lines;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        
        int start_idx = -1;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].rfind("-------------", 0) == 0) {
                start_idx = i + 1;
                break;
            }
        }
        if (start_idx == -1 || start_idx >= (int)lines.size()) return entries;
        
        for (size_t i = start_idx; i < lines.size(); ++i) {
            if (lines[i].rfind("-------------", 0) == 0) break;
            
            std::stringstream ls(lines[i]);
            std::vector<std::string> parts;
            std::string part;
            while (ls >> part) {
                parts.push_back(part);
            }
            if (parts.size() < 8) continue;
            
            std::string attr = parts[0];
            bool is_dir = (attr[0] == 'd' || attr[0] == 'D');
            uint32_t size = 0;
            try { size = std::stoul(parts[1]); } catch (...) {}
            
            std::string name = parts[7];
            
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
