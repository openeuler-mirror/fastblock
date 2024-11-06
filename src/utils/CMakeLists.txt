compile_library (
  NAME get_core
  DEPS
    numa
  SRCS
    get_core.cc
)

compile_library (
  NAME bdev_json
  SRCS
    bdev_json.cc
)