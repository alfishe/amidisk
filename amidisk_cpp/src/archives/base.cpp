#include "base.h"
#include "lha.h"
#include "rar.h"
#include "sevenz.h"
#include "zip.h"
#include <unistd.h>
#include <sys/wait.h>
#include <stdexcept>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <thread>

namespace amidisk {

std::string ArchiveHandler::find_executable(const std::vector<std::string>& names) {
    for (const auto& name : names) {
        // Simple which simulation using PATH
        std::string path_env = std::getenv("PATH");
        std::stringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                return full;
            }
        }
        
        // Check homebrew/common paths
        std::vector<std::string> extras = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"};
        for (const auto& loc : extras) {
            std::string full = loc + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                return full;
            }
        }
    }
    return "";
}

static std::vector<uint8_t> run_cmd_internal(const std::vector<std::string>& cmd, bool& success, std::string& stderr_out) {
    int out_pipe[2];
    int err_pipe[2];
    
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        throw std::runtime_error("failed to create pipes");
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("failed to fork");
    }
    
    if (pid == 0) {
        // Child
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        
        std::vector<char*> args;
        for (const auto& a : cmd) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        
        execvp(args[0], args.data());
        exit(1);
    }
    
    // Parent
    close(out_pipe[1]);
    close(err_pipe[1]);
    
    std::vector<uint8_t> stdout_data;
    std::string err_data;
    
    std::thread err_thread([&]() {
        char buf[4096];
        while (true) {
            ssize_t n = read(err_pipe[0], buf, sizeof(buf));
            if (n <= 0) break;
            err_data.append(buf, n);
        }
    });
    
    char buf[4096];
    while (true) {
        ssize_t n = read(out_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        stdout_data.insert(stdout_data.end(), buf, buf + n);
    }
    
    err_thread.join();
    
    close(out_pipe[0]);
    close(err_pipe[0]);
    
    int status;
    waitpid(pid, &status, 0);
    
    success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    stderr_out = err_data;
    
    return stdout_data;
}

std::string ArchiveHandler::run_command(const std::vector<std::string>& cmd, bool& success, std::string& stderr_out) {
    auto data = run_cmd_internal(cmd, success, stderr_out);
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

std::vector<uint8_t> ArchiveHandler::run_command_binary(const std::vector<std::string>& cmd, bool& success, std::string& stderr_out) {
    return run_cmd_internal(cmd, success, stderr_out);
}

bool ExternalExtractor::test_archive() {
    std::string exe = get_exe();
    if (exe.empty()) {
        std::cerr << "verifying archive integrity: " << path_ << "... ERROR: Extraction tool not found" << std::endl;
        return false;
    }
    
    std::cout << "verifying archive integrity: " << path_ << "... " << std::flush;
    
    bool success;
    std::string err_out;
    auto cmd = get_test_cmd();
    std::string out = run_command(cmd, success, err_out);
    
    if (success) {
        std::cout << "OK" << std::endl;
        return true;
    } else {
        std::cerr << "ERROR: " << (err_out.empty() ? out : err_out) << std::endl;
        return false;
    }
}

std::tuple<uint32_t, uint32_t, uint64_t> ExternalExtractor::stream_to_volume(
    Volume* vol, 
    const std::string& base_amiga_path, 
    uint32_t max_len, 
    uint32_t protect, 
    const std::string& comment
) {
    std::string exe = get_exe();
    if (exe.empty()) throw std::runtime_error("Extraction tool not found");
    
    bool success;
    std::string err_out;
    auto list_cmd = get_list_cmd();
    std::string out = run_command(list_cmd, success, err_out);
    if (!success) {
        throw std::runtime_error("failed to list archive: " + err_out);
    }
    
    auto entries = parse_list_output(out);
    
    uint32_t n = 0, dir_count = 0;
    uint64_t total_bytes = 0;
    
    for (const auto& entry : entries) {
        std::string name = entry.name;
        if (name.substr(0, 2) == "./") name = name.substr(2);
        else if (name == ".") continue;
        if (name.empty()) continue;
        
        // Truncate path parts
        std::stringstream ss(name);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(ss, part, '/')) {
            if (part.length() > max_len) part = part.substr(0, max_len);
            parts.push_back(part);
        }
        
        std::string rel_amiga;
        for (size_t i = 0; i < parts.size(); ++i) {
            rel_amiga += parts[i];
            if (i + 1 < parts.size()) rel_amiga += "/";
        }
        
        std::string amiga_dir = base_amiga_path;
        if (rel_amiga != ".") {
            if (!amiga_dir.empty() && amiga_dir.back() != '/') amiga_dir += "/";
            amiga_dir += rel_amiga;
        }
        
        if (entry.isdir) {
            if (!amiga_dir.empty()) {
                vol->makedirs(amiga_dir);
                dir_count++;
            }
        } else {
            std::string parent_dir;
            size_t slash = amiga_dir.find_last_of('/');
            if (slash != std::string::npos) {
                parent_dir = amiga_dir.substr(0, slash);
            }
            if (!parent_dir.empty()) {
                vol->makedirs(parent_dir);
            }
            
            auto extract_cmd = get_extract_cmd(entry.raw_name);
            bool extract_success;
            std::string extract_err;
            std::vector<uint8_t> data = run_command_binary(extract_cmd, extract_success, extract_err);
            if (!extract_success) {
                throw std::runtime_error("failed to extract " + entry.raw_name + ": " + extract_err);
            }
            
            vol->write_file(amiga_dir, data, {.protect = protect, .comment = comment});
            n++;
            total_bytes += data.size();
        }
    }
    
    return {n, dir_count, total_bytes};
}

std::unique_ptr<ArchiveHandler> create_archive_handler(const std::string& path) {
    if (LhaHandler::can_handle(path)) return std::make_unique<LhaHandler>(path);
    if (RarHandler::can_handle(path)) return std::make_unique<RarHandler>(path);
    if (SevenZHandler::can_handle(path)) return std::make_unique<SevenZHandler>(path);
    if (ZipHandler::can_handle(path)) return std::make_unique<ZipHandler>(path);
    return nullptr;
}

} // namespace amidisk
