compile_library(
  NAME par_mgr
  SRCS
    partition_manager.cc
    osd_stm.cc
  DEPS
    spdk::spdk
    raft_msg
    osd_msg
)

compile_library (
  NAME osd_lib
  SRCS
    osd_service.cc
  DEPS
    par_mgr
    monclient
    spdk::spdk
    raft
    osd_msg
    msg
    localstore
)

add_executable(fastblock-osd
  osd.cc
  osd_service.cc
  partition_manager.cc
  osd_stm.cc
)

install(TARGETS fastblock-osd DESTINATION bin)

target_include_directories (fastblock-osd
  PUBLIC
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fastblock-osd
  PUBLIC
    fastblock_osd_lib
)
