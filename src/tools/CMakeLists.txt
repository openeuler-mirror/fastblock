find_package(absl REQUIRED)

add_executable(block_bench
  block_bench.cc
)

target_include_directories (block_bench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(block_bench
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
)
