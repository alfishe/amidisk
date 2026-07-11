#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include "../fs/volume.h"

namespace amidisk {

class TarExtractor {
public:
    TarExtractor(const std::string& tar_path);
    ~TarExtractor();

    void extract_to(Volume* vol, const std::string& dest_path);

private:
    std::string tar_path_;
};

} // namespace amidisk
