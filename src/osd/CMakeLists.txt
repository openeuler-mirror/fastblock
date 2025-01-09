
# include_directories(${numa_INCLUDE_DIRS})

add_executable(fastblock-osd
  osd.cc
  osd_service.cc
  partition_manager.cc
  osd_stm.cc
  data_statistics.cc
  mon_client.cc
)

# target_link_libraries(fastblock-osd ${numa_LIBRARIES})
target_link_libraries(fastblock-osd 
  PUBLIC
    get_core
    monclient
    spdk::spdk
    raft_msg
    osd_msg
    raft
    msg
    localstore
)

install(TARGETS fastblock-osd DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)

target_include_directories (fastblock-osd
  PUBLIC
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
