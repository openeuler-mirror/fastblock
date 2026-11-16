find_package(absl REQUIRED)

add_executable(image_verify
  image_verify.cc
  dump_compare.cc
)

target_include_directories(image_verify
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/localstore/aos
)

target_link_libraries(image_verify
  PUBLIC
    base
    osd_msg
    msg
    monclient
    fbclient
    aos
)

install(TARGETS image_verify DESTINATION bin)

add_executable(image_verify_pattern_test
  verify_pattern_test.cc
)

target_include_directories(image_verify_pattern_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME image_verify_pattern_test COMMAND image_verify_pattern_test)
