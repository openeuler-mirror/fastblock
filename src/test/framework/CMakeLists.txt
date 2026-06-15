# Test framework library
add_library(test_framework STATIC
    test_runner.cc
)

target_include_directories(test_framework
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(test_framework
    PUBLIC
        base
        spdk::spdk
)

# Test runner executable
add_executable(fb_test_runner
    test_main.cc
)

target_link_libraries(fb_test_runner
    PRIVATE
        test_framework
        base
        osd_msg
        msg
)

# Install test framework headers
install(FILES
    test_framework.h
    test_config.h
    test_reporter.h
    test_harness.h
    DESTINATION include/fastblock/test
)
