#pragma once

#include <string>
#include <cstdint>
#include <utility>

namespace amidisk {

// Parse "image.hdf:Volume/path" into (image_path, "Volume/path")
std::pair<std::string, std::string> parse_image_arg(const std::string& arg, const std::string& fallback = "");

// All command functions throw on error; callers catch in main()
void cmd_info(const std::string& image_path, const std::string& vol_name, bool json_out, bool deep);
void cmd_ls(const std::string& image_path, const std::string& vol_name, const std::string& path, bool json_out);
void cmd_cat(const std::string& image_path, const std::string& vol_name, const std::string& path);
void cmd_check(const std::string& image_path, const std::string& vol_name, bool deep);
void cmd_extract(const std::string& image_path, const std::string& vol_name, const std::string& path, const std::string& dest, bool recursive);
void cmd_put(const std::string& image_path, const std::string& vol_name, const std::string& dest, const std::string& src, bool recursive, uint32_t protect, const std::string& comment, bool bulk);
void cmd_cp(const std::string& src_arg, const std::string& dst_arg, bool recursive, bool checksum, bool bulk);
void cmd_sync(const std::string& src_arg, const std::string& dst_arg, bool del, bool dry_run, bool update, bool checksum, bool bulk);
void cmd_bench(const std::string& image_path, const std::string& vol_name, const std::string& path, uint32_t limit_files, uint64_t limit_bytes, const std::string& filter);
void cmd_create(const std::string& image_path, const std::string& size_str, bool adf, bool force, const std::string& layout, const std::string& format_label, const std::string& dostype_str);
void cmd_format(const std::string& image_path, const std::string& vol_name, const std::string& label, const std::string& dostype_str);
void cmd_rdb_init(const std::string& image_path, uint32_t sectors, uint32_t heads, uint32_t rdb_cyls, bool force);
void cmd_part_add(const std::string& image_path, const std::string& drv_name, const std::string& size_str, const std::string& dostype_str, uint32_t bs, bool bootable, int32_t boot_pri, const std::string& format_label, bool no_auto_fs);
void cmd_part_del(const std::string& image_path, const std::string& drv_name, bool write);
void cmd_fs_add(const std::string& image_path, const std::string& fs_path, const std::string& dostype_str, uint16_t version, uint16_t revision);
void cmd_fs_extract(const std::string& image_path, int32_t num, const std::string& out_path);
void cmd_bootblock(const std::string& image_path, const std::string& vol_name, const std::string& bb_path);
void cmd_rdb_scan(const std::string& image_path, uint64_t rdb_area_secs, bool progress);
void cmd_rdb_rebuild(const std::string& image_path, const std::string& backup_path, bool deep, bool write);
void cmd_repair(const std::string& image_path, const std::string& vol_name, bool deep, bool write);

} // namespace amidisk
