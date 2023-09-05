compile_library(
  NAME par_mgr
  SRCS
    partition_manager.cc
    osd_sm.cc
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
    mon
    spdk::spdk
    raft
    osd_msg
    msg
    localstore
)

add_executable(osd
  osd.cc
  osd_service.cc
  partition_manager.cc
  osd_sm.cc
)

target_include_directories (osd
  PUBLIC
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(osd
  PUBLIC
    fastblock_osd_lib
)
