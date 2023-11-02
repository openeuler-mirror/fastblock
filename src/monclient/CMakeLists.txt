compile_library (
  NAME
    monclient
  SRCS
    log_components.cc
    message.pb.cc
    client.cc
  DEPS
    spdk::spdk
    protobuf
    par_mgr
    fastblock_raft
)

add_subdirectory(test)
