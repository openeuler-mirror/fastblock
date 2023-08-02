compile_library (
  NAME base
  DEPS
    spdk::spdk
  SRCS
    core_sharded.cc
  COPTS
    -Wno-error=unused-variable
    -Wno-error=pointer-arith  # 对void*进行加减运算
    -Wno-error=write-strings  # 把常量字符串当char*使用
)