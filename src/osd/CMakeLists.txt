
add_executable(osd
  osd.cc
  osd_service.cc
  partition_manager.cc
  osd_sm.cc
  mon_msg.pb.cc
  mon_client.cc
  )

target_include_directories (osd
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(osd 
  PUBLIC 
    spdk::spdk
    raft
    storage
    osd_msg
    msg
  )