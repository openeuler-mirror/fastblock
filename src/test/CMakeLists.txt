find_package(absl REQUIRED)

add_executable(raft_membership
  osd_client.cc
  raft_membership.cc
)

target_include_directories (raft_membership
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(raft_membership
  PUBLIC
    base
    osd_msg
    msg
)