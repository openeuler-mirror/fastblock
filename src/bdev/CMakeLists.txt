add_executable(fastblock-vhost
  global.cc
  bdev_fastblock.cc
  bdev_fastblock_rpc.cc
  vhost.cc
  )

target_include_directories (fastblock-vhost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fastblock-vhost
  PUBLIC
    spdk::spdk
    osd_msg
    msg
    fbclient
  )

install(TARGETS fastblock-vhost DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)
