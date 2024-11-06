find_package(mlx5 REQUIRED)

add_executable(raft_bench raft_bench.cc)
target_link_libraries(raft_bench
  PRIVATE
    fastblock_raft
    fastblock_par_mgr
    fastblock_localstore
    fastblock_bdev_json
    ${COMMON_LINKOPTS}
)
target_include_directories(raft_bench
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)

install(TARGETS raft_bench DESTINATION bin)
