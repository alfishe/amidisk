#include "output.h"
#include <iostream>
#include <mutex>

namespace amidisk {
namespace out {

static std::mutex g_output_mutex;
static OutputHandler g_print_handler;
static OutputHandler g_error_handler;

void print(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    if (g_print_handler) {
        g_print_handler(msg);
    } else {
        std::cout << msg << std::endl;
    }
}

void error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    if (g_error_handler) {
        g_error_handler(msg);
    } else {
        std::cerr << msg << std::endl;
    }
}

void print_raw(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    if (g_print_handler) {
        g_print_handler(msg);
    } else {
        std::cout << msg;
    }
}

void clear_line() {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    if (!g_print_handler) {
        std::cout << "\x1b[2K\r";
    }
}

void flush() {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    if (!g_print_handler) {
        std::cout.flush();
    }
}

void set_print_handler(OutputHandler handler) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    g_print_handler = handler;
}

void set_error_handler(OutputHandler handler) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    g_error_handler = handler;
}

void reset_handlers() {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    g_print_handler = nullptr;
    g_error_handler = nullptr;
}

} // namespace out
} // namespace amidisk
