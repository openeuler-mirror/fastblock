compile_library (
  NAME
    monclient
  SRCS
    log_components.cc
    messages.pb.cc
    client.cc
  DEPS
    spdk::spdk
    protobuf
)

# add_subdirectory(test)
