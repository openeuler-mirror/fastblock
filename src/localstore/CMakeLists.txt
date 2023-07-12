compile_library (
  NAME localstore
  SRCS 
    object_store.cc
  DEPS
    spdk::spdk
    absl::flat_hash_map
  COPTS
    -Wno-error=pointer-arith  # 对void*进行加减运算
    -Wno-error=write-strings  # 把常量字符串当char*使用
)

add_subdirectory(demo)