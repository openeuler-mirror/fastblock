add_subdirectory(aos)

# AOS-only production localstore (design v2 P4).
# Blob sources archived under legacy_blob/ and are not built.
compile_library(
  NAME localstore
  SRCS
    buffer_pool.cc
    spdk_buffer.cc
    storage_manager.cc
    blob_manager.cc
    kv_store.cc
    disk_log.cc

  DEPS
  spdk::spdk
  absl::flat_hash_map
  fastblock_base
  aos

  COPTS
  -Wno-error=unused-variable
  -Wno-error=pointer-arith # 对void*进行加减运算
  -Wno-error=write-strings # 把常量字符串当char*使用
)

# Blob demos disabled until rewritten for AOS (see legacy_blob/README.md).
# add_subdirectory(demo)
