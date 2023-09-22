add_executable(bdev
  global.cc
  bdev_fastblock.cc
  bdev_fastblock_rpc.cc
  vhost.cc
  )

target_include_directories (bdev
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(bdev
  PUBLIC
    spdk::spdk
    osd_msg
    msg
    fb_client
  )