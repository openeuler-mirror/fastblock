

compile_library(
  NAME raft_msg
  SRCS
    raft_msg.pb.cc
  DEPS
    protobuf
)

compile_library(
  NAME osd_msg
  SRCS
    osd_msg.pb.cc
  DEPS
    protobuf
)