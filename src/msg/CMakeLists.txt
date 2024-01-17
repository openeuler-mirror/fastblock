find_package(mlx5 REQUIRED)

compile_library (
  NAME msg
  SRCS
    log_components.cc
  DEPS
    mlx5
    spdk::spdk
    protobuf
)

add_subdirectory(demo)
