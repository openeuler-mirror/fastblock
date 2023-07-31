compile_library (
  NAME msg
  DEPS
    spdk::spdk
    fmt
    rte_timer
    rte_meter
    protobuf
)

add_subdirectory(demo)
