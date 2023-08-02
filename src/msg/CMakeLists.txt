compile_library (
  NAME msg
  DEPS
    spdk::spdk
    fmt
    protobuf
)

add_subdirectory(demo)
