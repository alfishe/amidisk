#pragma once

#include <string>
#include <sstream>
#include <functional>

namespace amidisk {

// Output channels - centralized so we can redirect to log, GUI, etc. later
namespace out {

// Print to stdout (normal output)
void print(const std::string& msg);

// Print to stderr (errors/warnings)
void error(const std::string& msg);

// Print without newline (for progress, etc.)
void print_raw(const std::string& msg);

// Clear current line (for progress updates)
void clear_line();

// Flush stdout
void flush();

// Set custom output handlers (for testing, GUI integration, etc.)
using OutputHandler = std::function<void(const std::string&)>;
void set_print_handler(OutputHandler handler);
void set_error_handler(OutputHandler handler);
void reset_handlers();

} // namespace out

// Helper to build formatted strings via stringstream
class fmt {
public:
    fmt() = default;

    template<typename T>
    fmt& operator<<(const T& val) {
        ss_ << val;
        return *this;
    }

    std::string str() const { return ss_.str(); }
    operator std::string() const { return ss_.str(); }

private:
    std::ostringstream ss_;
};

} // namespace amidisk
