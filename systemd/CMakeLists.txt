find_package(PkgConfig QUIET REQUIRED)
pkg_get_variable(SYSTEMD_SYSTEM_UNIT_DIR "systemd" "systemd_system_unit_dir")
if(NOT SYSTEMD_SYSTEM_UNIT_DIR)
  pkg_get_variable(SYSTEMD_SYSTEM_UNIT_DIR "systemd" "systemdsystemunitdir")
  if(NOT SYSTEMD_SYSTEM_UNIT_DIR)
    set(SYSTEMD_SYSTEM_UNIT_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/system")
  endif()
endif()
set(SYSTEMD_SYSTEM_UNIT_DIR "${SYSTEMD_SYSTEM_UNIT_DIR}"
  CACHE PATH "Location for systemd system unit files")

set(FAST_BLOCK_SYSTEMD_ENV_DIR "/etc/sysconfig"
  CACHE PATH "Location for systemd service environmental variable settings files")
foreach(service
    fastblock-mon@
    fastblock-osd@)
  configure_file(
    ${service}.service
    @ONLY)
  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/${service}.service
    DESTINATION ${SYSTEMD_SYSTEM_UNIT_DIR})
endforeach()

install(FILES
  fastblock.target
  fastblock-mon.target
  fastblock-osd.target
  DESTINATION ${SYSTEMD_SYSTEM_UNIT_DIR})
