compile_library (
  NAME monclient
  ENABLE_INSTALL 1
  SHARED 1
  SRCS
    log_components.cc
    messages.pb.cc
    client.cc
  DEPS
    spdk_log
    spdk_sock
    spdk_sock_posix
    protobuf
    par_mgr
)
