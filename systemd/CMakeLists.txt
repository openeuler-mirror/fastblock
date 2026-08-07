# Copyright (c) 2023-2024 ChinaUnicom
# fastblock is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

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
    fastblock-osd@
    fastblock-vhost
    fastblock-nvmf-tgt)
  configure_file(
    ${service}.service
    @ONLY)
  install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/${service}.service
    DESTINATION ${SYSTEMD_SYSTEM_UNIT_DIR})
endforeach()

install(FILES
  fastblock.target
  fastblock-mon.target
  fastblock-osd.target
  fastblock-vhost.target
  fastblock-nvmf-tgt.target
  DESTINATION ${SYSTEMD_SYSTEM_UNIT_DIR})
