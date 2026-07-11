include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

add_executable(amidisk_tests
    test_blkdev.cpp
    test_rdb_write.cpp
    test_ffs.cpp
    test_ffs_modify.cpp
    test_ffs_smoke.cpp
    test_archives.cpp
    test_pfs3_read.cpp
    test_pfs3_write.cpp
    test_sfs_read.cpp test_sfs_write.cpp
    test_rdb_rescue.cpp
    test_util.cpp
    # test_matrix.cpp
    # test_rescue.cpp
    # test_cli.cpp
)

target_link_libraries(amidisk_tests
    PRIVATE
    libamidisk
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(amidisk_tests)
