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

add_executable(snapshot_write_stress
  snapshot_write_stress.cc
)

target_include_directories(snapshot_write_stress
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(snapshot_write_stress
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
)

add_executable(snapshot_pg_stress
  osd_client.cc
  snapshot_pg_stress.cc
)

target_include_directories(snapshot_pg_stress
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(snapshot_pg_stress
  PUBLIC
    base
    osd_msg
    msg
)

add_executable(rollback_proof
  rollback_proof.cc
)

target_include_directories(rollback_proof
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rollback_proof
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
)

add_executable(clone_proof
  clone_proof.cc
)

target_include_directories(clone_proof
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(clone_proof
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
)

add_executable(flatten_proof
  flatten_proof.cc
)

target_include_directories(flatten_proof
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(flatten_proof
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
)
