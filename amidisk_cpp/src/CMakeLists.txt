add_library(libamidisk STATIC
    blkdev/blkdev.cpp
    blkdev/image_file.cpp
    blkdev/vhd.cpp
    blkdev/part_blkdev.cpp
    blkdev/overlay.cpp
    blkdev/bulk_cache.cpp
    rdb/blocks.cpp
    rdb/rdisk.cpp
    rdb/rescue.cpp
    fs/util.cpp
    fs/ffs.cpp
    fs/pfs3.cpp
    fs/sfs.cpp
    archives/tar.cpp
    archives/base.cpp
    archives/lha.cpp
    archives/rar.cpp
    archives/sevenz.cpp
    archives/zip.cpp
    image.cpp
    cli_util.cpp
    safety.cpp
    output.cpp
)

target_include_directories(libamidisk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libamidisk PUBLIC CLI11::CLI11 nlohmann_json::nlohmann_json)

add_executable(amidisk main.cpp)
target_link_libraries(amidisk PRIVATE libamidisk)
