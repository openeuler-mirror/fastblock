include(CMakeParseArguments)

set(CXX_STANDARD 20)
set(DEFAULT_LINKOPTS ${COMMON_LINKOPTS})
set(DEFAULT_COPTS -Wall -Wextra -Werror -Wno-missing-field-initializers)

function(compile_library)
  cmake_parse_arguments(CC_LIB
    "DISABLE_INSTALL;PUBLIC"
    "NAME"
    "HDRS;SRCS;COPTS;DEFINES;LINKOPTS;DEPS"
    ${ARGN}
    )

  set(_NAME "fastblock_${CC_LIB_NAME}")

  set(CC_SRCS "${CC_LIB_SRCS}")
  list(FILTER CC_SRCS EXCLUDE REGEX ".*\\.(h|inc)")

  if("${CC_SRCS}" STREQUAL "")
    set(CC_LIB_IS_INTERFACE 1)
  else()
    set(CC_LIB_IS_INTERFACE 0)
  endif()

  # 当没有源文件时，导出为interface
  if(NOT CC_LIB_IS_INTERFACE)
    # 创建静态库
    add_library(${_NAME} "")
    target_sources(${_NAME} PRIVATE ${CC_LIB_SRCS} ${CC_LIB_HDRS})
    target_include_directories(${_NAME}
      PUBLIC
      "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
      )
    target_compile_options(${_NAME}
      PRIVATE ${DEFAULT_COPTS}
      PRIVATE ${CC_LIB_COPTS})
    target_link_libraries(${_NAME}
      PUBLIC
        ${DEFAULT_LINKOPTS}
        ${CC_LIB_DEPS}

      PRIVATE
        ${CC_LIB_LINKOPTS}
      )
    target_compile_definitions(${_NAME} PUBLIC ${CC_LIB_DEFINES})
    # INTERFACE libraries can't have the CXX_STANDARD property set
    set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD ${CXX_STANDARD})
    set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

    set_target_properties(${_NAME} PROPERTIES
      OUTPUT_NAME "${_NAME}"
      )
  else()
    # 生成只有头文件的INTERFACE
    add_library(${_NAME} INTERFACE)
    target_include_directories(${_NAME}
      INTERFACE
      "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
      )
    target_link_libraries(${_NAME}
      INTERFACE
      ${CC_LIB_DEPS}
      ${CC_LIB_LINKOPTS}
      ${DEFAULT_LINKOPTS}
      )
    target_compile_definitions(${_NAME} INTERFACE ${CC_LIB_DEFINES})
  endif()
  # main symbol exported
  add_library(${CC_LIB_NAME} ALIAS ${_NAME})
endfunction()
