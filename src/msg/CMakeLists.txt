find_package(mlx5 REQUIRED)

compile_library (
  NAME msg
  ENABLE_INSTALL 1
  SHARED 1
  SRCS
    log_components.cc
  DEPS
    mlx5
    spdk_init
    # spdk_thread
    spdk_util
    spdk_log
    spdk::env_dpdk
    protobuf
    ibverbs
    rdmacm
)

add_subdirectory(demo)
