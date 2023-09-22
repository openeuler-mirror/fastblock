compile_library(
  NAME localstore
  SRCS
  object_store.cc
  rolling_blob.cc
  buffer_pool.cc
  spdk_buffer.cc
  blob_manager.cc
  kv_store.cc

  DEPS
  spdk::spdk
  absl::flat_hash_map
  fastblock_base

  COPTS
  -Wno-error=unused-variable
  -Wno-error=pointer-arith # 对void*进行加减运算
  -Wno-error=write-strings # 把常量字符串当char*使用
)

# add_subdirectory(demo)
