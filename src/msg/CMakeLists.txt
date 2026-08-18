find_package(mlx5 QUIET)

set(_msg_deps spdk::spdk protobuf)
if(FASTBLOCK_WITH_RDMA)
  if(NOT mlx5_FOUND)
    message(FATAL_ERROR "FASTBLOCK_WITH_RDMA=ON but mlx5 was not found")
  endif()
  list(APPEND _msg_deps mlx5 ibverbs rdmacm)
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
  DEPS
    ${_msg_deps}
)

add_subdirectory(demo)
