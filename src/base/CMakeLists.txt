compile_library (
  NAME base
  DEPS
    spdk::spdk
  SRCS
    core_sharded.cc
)