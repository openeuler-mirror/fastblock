add_executable(server ping_pong.pb.cc server.cc)
target_link_libraries(server
  PUBLIC
    fastblock_msg
  PRIVATE
    ${COMMON_LINKOPTS}
)
target_include_directories(server
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)

add_executable(client ping_pong.pb.cc client.cc)
target_link_libraries(client
  PUBLIC
    fastblock_msg
  PRIVATE
    ${COMMON_LINKOPTS}
)
target_include_directories(client
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)