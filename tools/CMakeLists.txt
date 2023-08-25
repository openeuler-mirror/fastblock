find_package(spdk REQUIRED)
find_package(absl REQUIRED)

add_executable(fbbench
  fbbench.cc
  )

target_include_directories (fbbench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fbbench
  PUBLIC 
    spdk::spdk
    base
    osd_msg
    msg
  )