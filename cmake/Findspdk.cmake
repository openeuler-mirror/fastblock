find_package (PkgConfig REQUIRED)

if(spdk_FIND_COMPONENTS)
  if(NOT bdev IN_LIST spdk_FIND_COMPONENTS)
    list (APPEND spdk_FIND_COMPONENTS bdev)
  endif()
else()
  set(spdk_FIND_COMPONENTS
    bdev
    event_bdev
    event_accel
    event_vhost_blk
    event_vhost_scsi
    event_vmd
    event_sock
    accel
    init
    blobfs
    blob
    env_dpdk
    event
    ftl
    iscsi
    json
    jsonrpc
    log
    lvol
    nvme
    syslibs
    thread
    vhost)
endif()

include (FindPackageHandleStandardArgs)
set (spdk_INCLUDE_DIR)
set (spdk_LINK_DIRECTORIES)

set (_spdk_env_dpdk_deps archive)
set (_spdk_bdev_aio_deps aio)
set (_spdk_util_deps uuid)

foreach (component ${spdk_FIND_COMPONENTS})
  pkg_check_modules (spdk_${component} spdk_${component}) # QUIET
  set (prefix spdk_${component}_STATIC)
  list (APPEND spdk_lib_vars ${prefix}_LIBRARIES)
  if (NOT spdk_${component}_FOUND)
    continue ()
  endif ()
  add_library (spdk::${component} INTERFACE IMPORTED)

  message(STATUS "\nspdk::component: " spdk::${component})
  message(STATUS "prefix: " ${prefix})
  message(STATUS ${prefix} "_LIBRARIES: " ${${prefix}_LIBRARIES})

  # add the dependencies of the linked SPDK libraries if any
  foreach (spdk_lib ${${prefix}_LIBRARIES} )
    foreach (dep ${_${spdk_lib}_deps})
      find_package (${dep} QUIET)
      if (NOT ${dep}_FOUND)
        continue ()
      endif ()
      if (NOT ${dep} IN_LIST "${${prefix}_LIBRARIES}")
        list (APPEND ${prefix}_LIBRARIES ${dep})
      endif ()
    endforeach ()
  endforeach ()

  set_target_properties (spdk::${component}
    PROPERTIES
      INTERFACE_COMPILE_OPTIONS ${${prefix}_CFLAGS}
      INTERFACE_INCLUDE_DIRECTORIES ${${prefix}_INCLUDE_DIRS}
      INTERFACE_LINK_OPTIONS "-Wl,--whole-archive;${${prefix}_LDFLAGS};-Wl,--no-whole-archive"
      INTERFACE_LINK_LIBRARIES "${${prefix}_LIBRARIES}"
      INTERFACE_LINK_DIRECTORIES "${${prefix}_LIBRARY_DIRS}")

  message(STATUS ${prefix} "_CFLAGS: " ${${prefix}_CFLAGS})
  message(STATUS ${prefix} "_INCLUDE_DIRS: " ${${prefix}_INCLUDE_DIRS})
  message(STATUS ${prefix} "_LDFLAGS: " ${${prefix}_LDFLAGS})
  message(STATUS ${prefix} "_LIBRARIES: " ${${prefix}_LIBRARIES})

  if (NOT spdk_INCLUDE_DIR)
    set (spdk_INCLUDE_DIR ${${prefix}_INCLUDE_DIRS})
  endif ()

  message(STATUS ${prefix} "_INCLUDE_DIRS: " ${${prefix}_INCLUDE_DIRS})

  if (NOT spdk_LINK_DIRECTORIES)
    set (spdk_LINK_DIRECTORIES ${${prefix}_LIBRARY_DIRS})
  endif ()
  list (APPEND spdk_link_opts "${${prefix}_LDFLAGS}")
  list (APPEND spdk_libs ${${prefix}_LIBRARIES})
endforeach ()

if (spdk_INCLUDE_DIR AND EXISTS "${spdk_INCLUDE_DIR}/spdk/version.h")
  foreach(ver "MAJOR" "MINOR" "PATCH")
    file(STRINGS "${spdk_INCLUDE_DIR}/spdk/version.h" spdk_VER_${ver}_LINE
      REGEX "^#define[ \t ]+SPDK_VERSION_${ver}[ \t]+[0-9]+$")
    string(REGEX REPLACE "^#define[ \t]+SPDK_VERSION_${ver}[ \t]+([0-9]+)$"
      "\\1" spdk_VERSION_${ver} "${spdk_VER_${ver}_LINE}")
    unset(${spdk_VER_${ver}_LINE})
  endforeach()
  set(spdk_VERSION_STRING
    "${spdk_VERSION_MAJOR}.${spdk_VERSION_MINOR}.${spdk_VERSION_PATCH}")
endif ()

list(REMOVE_DUPLICATES spdk_lib_vars)
find_package_handle_standard_args (spdk
  REQUIRED_VARS
    spdk_INCLUDE_DIR
    spdk_LINK_DIRECTORIES
    ${spdk_lib_vars}
  VERSION_VAR
    spdk_VERSION_STRING)

if (spdk_FOUND AND NOT (TARGET spdk::spdk))
  set (spdk_LIBRARIES ${spdk_libs})
  set (whole_archive_link_opts
    -Wl,--whole-archive -Wl,-Bstatic ${spdk_link_opts} -Wl,--no-whole-archive -Wl,-Bdynamic -luuid -laio)
  add_library (spdk::spdk INTERFACE IMPORTED)
  set_target_properties (spdk::spdk
    PROPERTIES
      INTERFACE_COMPILE_OPTIONS "${spdk_PC_STATIC_bdev_CFLAGS}"
      INTERFACE_INCLUDE_DIRECTORIES "${spdk_INCLUDE_DIR}"
      INTERFACE_LINK_OPTIONS "${whole_archive_link_opts}"
      INTERFACE_LINK_LIBRARIES "${spdk_LIBRARIES}"
      INTERFACE_LINK_DIRECTORIES "${spdk_LINK_DIRECTORIES}")
endif ()
