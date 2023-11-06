add_executable(mon_client_test main.cc)
target_link_libraries(mon_client_test
  PUBLIC

    spdk::spdk
    fastblock_osd_lib
  PRIVATE
    ${COMMON_LINKOPTS}
)
target_include_directories(mon_client_test
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
)