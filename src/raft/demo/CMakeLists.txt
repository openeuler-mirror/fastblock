# demos

macro (compile_demo name)
  set (args ${ARGN})

  cmake_parse_arguments (
    parsed_args
    ""
    ""
    "SOURCES;DEPS;COPTS"
    ${args})

  set (target ${name})
  add_executable (${target} ${parsed_args_SOURCES})

  target_include_directories (${target}
    PRIVATE 
      ${CMAKE_CURRENT_SOURCE_DIR}
      "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>")

  target_link_libraries (${target}
    PRIVATE
      ${parsed_args_DEPS})

  target_compile_options(${target}
    PRIVATE
      -Wno-error=write-strings
      ${parsed_args_COPTS}
  ) 
endmacro ()


compile_demo(demo_raftlog
  SOURCES
    demo_raftlog.cc
  DEPS
    fastblock_raft
)