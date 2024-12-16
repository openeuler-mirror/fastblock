

compile_library(
  NAME raft_msg
  ENABLE_INSTALL 1
  SHARED 1
  SRCS
    common_msg.pb.cc
    raft_msg.pb.cc
  DEPS
    protobuf
)

compile_library(
  NAME osd_msg
  ENABLE_INSTALL 1
  SHARED 1
  SRCS
    common_msg.pb.cc
    osd_msg.pb.cc
  DEPS
    protobuf
)