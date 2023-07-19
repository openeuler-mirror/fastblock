# osd
add_executable(osd
    osd.cc
)

link_directories(
    ${FASTBLOCK_DEPS_INSTALL_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build/deps_build/spdk-prefix/src/spdk/dpdk/build/lib/    
)

# target_compile_options(osd PRIVATE -fsanitize=address)

target_link_libraries(osd
    PUBLIC
        spdk::spdk
        absl::flat_hash_map
    PRIVATE  
        ${COMMON_LINKOPTS}
)

target_include_directories(osd
  PUBLIC
  "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
  spdk_INCLUDE_DIR
  "${FASTBLOCK_DEPS_INSTALL_DIR}/include/"
   "${CMAKE_CURRENT_SOURCE_DIR}/../../"
)
