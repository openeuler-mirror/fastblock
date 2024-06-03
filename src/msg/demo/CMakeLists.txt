find_package(mlx5 REQUIRED)

add_executable(server ping_pong.pb.cc server.cc)
target_link_libraries(server
  PRIVATE
      fastblock_msg
    ${COMMON_LINKOPTS}
)
target_include_directories(server
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)

add_executable(client ping_pong.pb.cc client.cc)
target_link_libraries(client
  PRIVATE
    fastblock_msg
    ${COMMON_LINKOPTS}
)
target_include_directories(client
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)

add_executable(ping_pong ping_pong.pb.cc ping_pong.cc)
target_link_libraries(ping_pong
  PRIVATE
    fastblock_msg
    ${COMMON_LINKOPTS}
)
target_include_directories(ping_pong
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)
