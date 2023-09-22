
add_executable(client_demo
    demo.cc
  )

target_include_directories (client_demo
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(client_demo 
  PUBLIC 
    fb_client
    msg 
  )

project(client_demo_c VERSION 1.0.0 LANGUAGES C CXX)
  
add_executable(client_demo_c
  demo.c
)
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories (client_demo_c
PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(client_demo_c 
  PUBLIC 
  fb_client
  msg
  base
)