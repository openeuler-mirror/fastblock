

compile_library(
  NAME raft
  SRCS
    pg_group.cc
    raft_log.cc
    raft_node.cc
    raft_server.cc
    raft_server_properties.cc
    state_machine.cc
    raft_client_protocol.cc
  DEPS
    spdk::spdk
    raft_msg
    msg
    base
    localstore
)
