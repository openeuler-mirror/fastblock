add_executable(fastblock-vhost
  global.cc
  common.cc
  bdev_fastblock.cc
  bdev_fastblock_rpc.cc
  vhost.cc
  )

add_executable(fastblock-nvmf-tgt
  global.cc
  common.cc
  bdev_fastblock.cc
  bdev_fastblock_rpc.cc
  nvmf_tgt.cc
  )

target_include_directories (fastblock-vhost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fastblock-vhost
  PUBLIC
    spdk::spdk
    osd_msg
    msg
    fbclient
    get_core
  )

target_include_directories (fastblock-nvmf-tgt
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fastblock-nvmf-tgt
  PUBLIC
    spdk::spdk
    osd_msg
    msg
    fbclient
    get_core
  )

install(TARGETS fastblock-vhost fastblock-nvmf-tgt DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)
