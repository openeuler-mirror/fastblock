# Append-only store (AOS)
#
# I/O path is SPDK-native (spdk_bdev_* + callbacks on reactor).
# Pure logic (record / extent_map) can be unit-tested without a bdev.

compile_library(
  NAME aos
  SRCS
    record.cc
    extent_map.cc
    checkpoint.cc
    superblock.cc
    segment_allocator.cc
    segment.cc
    spdk_bdev_backend.cc
    mem_device_backend.cc
    pg_store.cc
    osd_store.cc
    snapshot_reader.cc
    snapshot_ingest.cc
    aos_runtime.cc
    log_store.cc
    kv_engine.cc
    aos_dump.cc
  DEPS
    spdk::spdk
  COPTS
    -Wno-error=unused-variable
    -Wno-error=pointer-arith
)

# Unit test: pure logic + mem_device_backend (no spdk_app).
add_executable(aos_m1_test
  aos_m1_test.cc
)

target_include_directories(aos_m1_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aos_m1_test
  PRIVATE
    aos
    spdk::spdk
)

add_test(NAME aos_m1_test COMMAND aos_m1_test)

# SPDK reactor integration test (malloc bdev).
add_executable(aos_spdk_test
  aos_spdk_test.cc
)

target_include_directories(aos_spdk_test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aos_spdk_test
  PRIVATE
    aos
    spdk::spdk
)

# Copy JSON next to binary for convenience.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/aos_malloc.json
  ${CMAKE_CURRENT_BINARY_DIR}/aos_malloc.json
  COPYONLY
)
