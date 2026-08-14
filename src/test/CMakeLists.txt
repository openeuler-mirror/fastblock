find_package(absl REQUIRED)

enable_testing()

# RTC PR-1: sticky home_shard routing helpers (no SPDK / RDMA)
add_executable(pg_home_shard_test
  pg_home_shard_test.cc
)

target_include_directories(pg_home_shard_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME pg_home_shard_test COMMAND pg_home_shard_test)

# RTC PR-2: local-first predicate regression
add_executable(rtc_local_first_test
  rtc_local_first_test.cc
)
target_include_directories(rtc_local_first_test
  PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
add_test(NAME rtc_local_first_test COMMAND rtc_local_first_test)

# RTC PR-4: pg→shard COW semantics
add_executable(pg_shard_table_cow_test
  pg_shard_table_cow_test.cc
)
add_test(NAME pg_shard_table_cow_test COMMAND pg_shard_table_cow_test)

add_executable(disk_log_trim_test
  disk_log_trim_test.cc
)
target_include_directories(disk_log_trim_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/localstore/aos
)
target_link_libraries(disk_log_trim_test
  PRIVATE
    aos
    localstore
    spdk::spdk
)
add_test(NAME disk_log_trim_test COMMAND disk_log_trim_test)

add_executable(raft_contract_test
  raft_contract_test.cc
)
target_include_directories(raft_contract_test
  PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
target_link_libraries(raft_contract_test
  PUBLIC
    raft_msg
    spdk::spdk
)
add_test(NAME raft_contract_test COMMAND raft_contract_test)

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
