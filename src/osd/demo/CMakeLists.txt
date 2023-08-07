add_executable(demo
  demo_main.cc
  )

target_include_directories (demo
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(demo 
  PUBLIC 
    spdk::spdk
    base
    osd_msg
    msg
  )

