cmake_minimum_required(VERSION 3.12.0)
project(fastblock VERSION 0.0.1 LANGUAGES CXX)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# CCACHE_DIR is initially provided by vtools
set(CCACHE_DIR $ENV{CCACHE_DIR} CACHE PATH "ccache directory")
set(ENV{CCACHE_DIR} ${CCACHE_DIR})
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_UNITY_BUILD_BATCH_SIZE 10)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (DEPS_INSTALL_DIR)
  set(FASTBLOCK_DEPS_INSTALL_DIR ${DEPS_INSTALL_DIR} CACHE STRING "Managed dependencies install directory")
else ()
  set(FASTBLOCK_DEPS_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps_install CACHE STRING "Managed dependencies install directory")
endif()

message("deps will install to ${FASTBLOCK_DEPS_INSTALL_DIR}")

list(APPEND BASE_LD_FLAGS_LIST
  -L${FASTBLOCK_DEPS_INSTALL_DIR}/lib
  -L${FASTBLOCK_DEPS_INSTALL_DIR}/lib64
  )
set(PKG_CONFIG_PATH_LIST
  ${FASTBLOCK_DEPS_INSTALL_DIR}/lib64/pkgconfig
  ${FASTBLOCK_DEPS_INSTALL_DIR}/share/pkgconfig
  ${FASTBLOCK_DEPS_INSTALL_DIR}/lib/pkgconfig
  )

list(APPEND BASE_CXX_FLAGS_LIST -fPIC)
list(APPEND BASE_C_FLAGS_LIST -fPIC)

if ("${CMAKE_BUILD_TYPE}" MATCHES "Debug")
  set(COMMON_LINKOPTS "-lasan -lubsan")
  add_compile_definitions(DEBUG)

  # TODO(sunyifang):
  #   加上sanitizer，链接时就会链接动态的dpdk
  #   但是spdk的dpdk并没有导出，所以会报错无法找到对应的库
  # 解决方法：
  #   1. 修改spdk的dpdkbuild以及CONFIG，让dpdk安装到指定目录
  #   2. 或者使用单独的dpdk库，在spdk命令行指定dpdk路径
  #   3. 寻找原因，为什么加上sanitizer，就会链接动态的dpdk库
  #
  #    list(APPEND BASE_CXX_FLAGS_LIST
  #            -fsanitize=undefined
  #            -fsanitize=address
  #            )
  #    list(APPEND BASE_C_FLAGS_LIST
  #            -fsanitize=undefined
  #            -fsanitize=address
  #            )
  #    list(APPEND BASE_LD_FLAGS_LIST
  #            -fsanitize=undefined
  #            -fsanitize=address
  #            )
else()
  set(COMMON_LINKOPTS)
endif()


# join flag lists
string(JOIN " " BASE_C_FLAGS ${BASE_C_FLAGS_LIST})
string(JOIN " " BASE_CXX_FLAGS ${BASE_CXX_FLAGS_LIST})
string(JOIN " " BASE_LD_FLAGS ${BASE_LD_FLAGS_LIST})
string(JOIN ":" PKG_CONFIG_PATH ${PKG_CONFIG_PATH_LIST})


find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH}  "${PKG_CONFIG_PATH}")
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} ${BASE_CXX_FLAGS}")
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()
set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} ${BASE_C_FLAGS}")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Choose: Debug, Release, RelWithDebInfo, MinSizeRel." FORCE)
endif()


set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${BASE_LD_FLAGS} -pie")
set(CMAKE_SHARED_LINKER_FLAGS "${BASE_LD_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS "${BASE_LD_FLAGS}")

# don't export() the contents to registry
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE INTERNAL "" FORCE)

# disable system level registry /usr/local/share/cmake/*
set(CMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY ON CACHE INTERNAL "" FORCE)

# disable user package registry ~/.cmake/*
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON CACHE INTERNAL "" FORCE)
set(FASTBLOCK_DEPS_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps_build)

# build type specific flags
string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
set(CMAKE_C_FLAGS_BUILD_TYPE ${CMAKE_C_FLAGS_${BUILD_TYPE}})
set(CMAKE_CXX_FLAGS_BUILD_TYPE ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})
configure_file(cmake/ExternalLibrary.cmake.in ${FASTBLOCK_DEPS_BUILD_DIR}/CMakeLists.txt @ONLY)

# wire up dependency search paths.
set(COMMON_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/")
list(APPEND CMAKE_PREFIX_PATH "${FASTBLOCK_DEPS_INSTALL_DIR}")
set(BASE_LD_FLAGS "${BASE_LD_FLAGS} -L${FASTBLOCK_DEPS_INSTALL_DIR}/lib")
if(NOT FASTBLOCK_DEPS_SKIP_BUILD)
  execute_process(COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" .
    RESULT_VARIABLE result
    WORKING_DIRECTORY ${FASTBLOCK_DEPS_BUILD_DIR})
  if(result)
    message(FATAL_ERROR "CMake step for deps failed: ${result}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} --build .
    RESULT_VARIABLE result
    WORKING_DIRECTORY ${FASTBLOCK_DEPS_BUILD_DIR})
  if(result)
    message(FATAL_ERROR "Build step for build failed: ${result}")
  endif()
endif()


include(compile_library)
add_subdirectory(src)