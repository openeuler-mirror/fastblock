add_executable(fastblock-vhost
  common.cc
  vhost.cc
  )

add_executable(fastblock-nvmf-tgt
  common.cc
  nvmf_tgt.cc
  )

target_include_directories (fastblock-vhost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fastblock-vhost
  PUBLIC
    spdk::spdk
    osd_msg
    msg
    get_core
    client
    fastblock_raft
    par_mgr
  )

target_include_directories (fastblock-nvmf-tgt
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fastblock-nvmf-tgt
  PUBLIC
    spdk::spdk
    osd_msg
    msg
    get_core
    client
    fastblock_raft
    par_mgr
  )

install(
  TARGETS
    fastblock-vhost
    fastblock-nvmf-tgt
  DESTINATION
    bin
  PERMISSIONS
    OWNER_EXECUTE
    OWNER_WRITE
    OWNER_READ
    GROUP_EXECUTE
    GROUP_READ
    WORLD_READ
    WORLD_EXECUTE)
