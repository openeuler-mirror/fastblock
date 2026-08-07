# Copyright (c) 2023-2024 ChinaUnicom
# fastblock is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

EXECUTE_PROCESS(
        COMMAND go build -o fastblock-mon ./cmd/fastblock-mon
        WORKING_DIRECTORY monitor
)

install(FILES fastblock-mon DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
        GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)

EXECUTE_PROCESS(
        COMMAND go build -o fastblock-client ./cmd/fastblock-client
        WORKING_DIRECTORY monitor
)

install(FILES fastblock-client DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
        GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)
