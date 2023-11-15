/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "osd_client.h"

void get_leader_source::process_response(){
    SPDK_NOTICELOG("leader of the pg %lu.%lu is %d\n", _request->pool_id(), _request->pg_id(), response.leader_id());
    _client->set_leader_id(response.leader_id());
    if(_node_id != response.leader_id()){
        _client->create_connect(response.leader_addr(), response.leader_port(), response.leader_id());
    }
    _fun();
    delete this;
}