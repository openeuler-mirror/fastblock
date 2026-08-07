# Copyright (c) 2023-2024 ChinaUnicom
# fastblock is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

find_package(spdk REQUIRED)
find_package(absl REQUIRED)

add_subdirectory(base)
add_subdirectory(msg)
add_subdirectory(rpc)
add_subdirectory(raft)
add_subdirectory(localstore)
add_subdirectory(osd)
add_subdirectory(monclient)
add_subdirectory(client)
add_subdirectory(bdev)
add_subdirectory(tools)
add_subdirectory(test)
add_subdirectory(utils)
