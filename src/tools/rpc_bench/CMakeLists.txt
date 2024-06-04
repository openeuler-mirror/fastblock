find_package(mlx5 REQUIRED)

add_executable(rpc_bench rpc_bench.pb.cc rpc_bench.cc)
target_link_libraries(rpc_bench
  PRIVATE
    fastblock_msg
    ${COMMON_LINKOPTS}
)
target_include_directories(rpc_bench
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)

install(TARGETS block_bench DESTINATION bin)
