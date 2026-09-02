# Each test is a standalone binary returning non-zero on failure -- no framework
# to install, so a clean checkout builds and tests with cmake + ctest alone.
set(MNEMOS_TESTS
    test_resp
    test_dict
    test_object
    test_listpack
    test_intset
    test_skiplist
    test_collections
    test_rdb
    test_dtoa)

foreach(test_name IN LISTS MNEMOS_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE mnemos_core)
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
