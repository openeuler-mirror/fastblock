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

install(TARGETS block_bench DESTINATION bin)

add_executable(fbbench
  fbbench.cc
)

target_include_directories (fbbench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fbbench
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
)

install(TARGETS fbbench DESTINATION bin)