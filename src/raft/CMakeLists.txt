compile_library(
  NAME raft
  SRCS
    pg_group.cc
    raft_log.cc
    raft_node.cc
    raft_server.cc
    state_machine.cc
    raft_client_protocol.cc
    append_entry_buffer.cc
    configuration_manager.cc
  DEPS
    spdk::spdk
    raft_msg
    msg
    base
    localstore
)

# add_subdirectory(demo)
