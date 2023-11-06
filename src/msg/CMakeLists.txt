compile_library (
  NAME msg
  SRCS
    log_components.cc
  DEPS
    spdk::spdk
    protobuf
)

add_subdirectory(demo)
