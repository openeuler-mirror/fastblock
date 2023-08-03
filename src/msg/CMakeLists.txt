compile_library (
  NAME msg
  SRCS
    log_components.cc
  DEPS
    spdk::spdk
    fmt
    protobuf
)

add_subdirectory(demo)
