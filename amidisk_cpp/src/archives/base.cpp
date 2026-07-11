#include "base.h"
#include "lha.h"
#include "rar.h"
#include "sevenz.h"
#include "zip.h"
#include <stdexcept>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define access _access
#define X_OK 0
#define PATH_SEPARATOR ';'
#else
#include <unistd.h>
#include <sys/wait.h>
#define PATH_SEPARATOR ':'
#endif

namespace amidisk {

std::string ArchiveHandler::find_executable(const std::vector<std::string>& names) {
    for (const auto& name : names) {
#ifdef _WIN32
        // On Windows, add .exe extension if not present
        std::string exe_name = name;
        if (exe_name.find(".exe") == std::string::npos) {
            exe_name += ".exe";
        }
#else
        const std::string& exe_name = name;
#endif
        // Simple which simulation using PATH
        const char* path_env_c = std::getenv("PATH");
        if (!path_env_c) continue;
        std::string path_env = path_env_c;
        std::stringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, PATH_SEPARATOR)) {
            std::filesystem::path full = std::filesystem::path(dir) / exe_name;
            if (access(full.string().c_str(), X_OK) == 0) {
                return full.string();
            }
        }

#ifndef _WIN32
        // Check homebrew/common paths (Unix only)
        std::vector<std::string> extras = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"};
        for (const auto& loc : extras) {
            std::string full = loc + "/" + exe_name;
            if (access(full.c_str(), X_OK) == 0) {
                return full;
            }
        }
#endif
    }
    return "";
}

#ifdef _WIN32
// Windows implementation using CreateProcess
static std::vector<uint8_t> run_cmd_internal(const std::vector<std::string>& cmd, bool& success, std::string& stderr_out) {
    // Build command line
    std::string cmdline;
    for (const auto& arg : cmd) {
        if (!cmdline.empty()) cmdline += " ";
        // Simple quoting - wrap args with spaces in quotes
        if (arg.find(' ') != std::string::npos) {
            cmdline += "\"" + arg + "\"";
        } else {
            cmdline += arg;
        }
    }

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE stdout_read, stdout_write, stderr_read, stderr_write;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        throw std::runtime_error("failed to create pipes");
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, const_cast<char*>(cmdline.c_str()), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(stdout_read); CloseHandle(stdout_write);
        CloseHandle(stderr_read); CloseHandle(stderr_write);
        success = false;
        stderr_out = "Failed to create process";
        return {};
    }

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    std::vector<uint8_t> stdout_data;
    std::string err_data;

    // Read stderr in a thread
    std::thread err_thread([&]() {
        char buf[4096];
        DWORD n;
        while (ReadFile(stderr_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
            err_data.append(buf, n);
        }
    });

    // Read stdout
    char buf[4096];
    DWORD n;
    while (ReadFile(stdout_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
        stdout_data.insert(stdout_data.end(), buf, buf + n);
    }

    err_thread.join();

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    success = (exit_code == 0);
    stderr_out = err_data;

    return stdout_data;
}
#else
// POSIX implementation using fork/exec
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
        _exit(1);
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
#endif

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
