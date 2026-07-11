#pragma once

#include "image.h"
#include "blkdev/overlay.h"
#include <string>
#include <vector>
#include <utility>

namespace amidisk {

/// volume_report — inspect every volume of a (possibly simulated) image.
/// Returns (lines, all_ok). Shows label, fill/free, check result and a
/// sample of the root catalog.
std::pair<std::vector<std::string>, bool>
volume_report(DiskImage& img, bool list_root = true, bool deep = false);

/// commit_overlay — apply a verified overlay to the real image file.
/// Optionally backs up the RDB area first. Returns true on success.
/// Prints progress messages to stdout/stderr.
bool commit_overlay(OverlayBlockDevice& overlay, const std::string& path,
                    const std::string& backup_path = "");

} // namespace amidisk
