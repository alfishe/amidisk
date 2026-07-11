#pragma once
#include "base.h"

namespace amidisk {

class ZipHandler : public ExternalExtractor {
public:
    explicit ZipHandler(const std::string& path);
    static bool can_handle(const std::string& path);

protected:
    std::string get_exe() const override;
    std::vector<std::string> get_test_cmd() const override;
    std::vector<std::string> get_list_cmd() const override;
    std::vector<std::string> get_extract_cmd(const std::string& raw_name) const override;
    std::vector<ArchiveEntry> parse_list_output(const std::string& output) const override;
    
private:
    mutable std::string cached_exe_;
};

} // namespace amidisk
