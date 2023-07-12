# obj_demo
add_executable(obj_demo
  obj_demo.cc
)

target_link_libraries(obj_demo 
  PUBLIC
    fastblock_localstore
  PRIVATE
    ${COMMON_LINKOPTS}
)

target_include_directories(obj_demo
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)
