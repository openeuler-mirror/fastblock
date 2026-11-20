if(FASTBLOCK_WITH_RDMA)
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
endif()

add_executable(sock_server ping_pong.pb.cc sock_server.cc)
target_link_libraries(sock_server
  PRIVATE
    fastblock_msg
    osd_msg
    raft_msg
    ${COMMON_LINKOPTS}
)
target_include_directories(sock_server
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)

add_executable(sock_client ping_pong.pb.cc sock_client.cc)
target_link_libraries(sock_client
  PRIVATE
    fastblock_msg
    osd_msg
    raft_msg
    ${COMMON_LINKOPTS}
)
target_include_directories(sock_client
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)
