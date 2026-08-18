find_package(mlx5 QUIET)

set(_msg_deps spdk::spdk protobuf osd_msg raft_msg)
set(_msg_defs)
if(FASTBLOCK_WITH_RDMA)
  if(NOT mlx5_FOUND)
    message(FATAL_ERROR "FASTBLOCK_WITH_RDMA=ON but mlx5 was not found")
  endif()
  list(APPEND _msg_deps mlx5 ibverbs rdmacm)
  list(APPEND _msg_defs FASTBLOCK_WITH_RDMA)
endif()

compile_library (
  NAME msg
  SRCS
    log_components.cc
    quota.cc
    cm_pool.cc
    connection.cc
    client.cc
    server.cc
    factory.cc
    probe_rnic.cc
  DEPS
    ${_msg_deps}
  DEFINES
    ${_msg_defs}
)

add_subdirectory(demo)
