find_package(spdk REQUIRED)
find_package(absl REQUIRED)

add_executable(rpcbench
  rpcbench.cc
  )

target_include_directories (rpcbench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rpcbench
  PUBLIC 
    spdk::spdk
    base
    osd_msg
    msg
  )

add_executable(opbench
  opbench.cc
  )

target_include_directories (opbench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(opbench
  PUBLIC 
    spdk::spdk
    base
    osd_msg
    msg
  )

