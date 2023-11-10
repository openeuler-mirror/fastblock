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
#pragma once

#include <queue>
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"

class raft_server_t;

class append_entries_buffer{
public:
    struct item_type{
        const msg_appendentries_t* request;
        msg_appendentries_response_t* response;
        utils::context* complete;
    };

    append_entries_buffer(raft_server_t* raft)
    : _raft(raft)
    , _in_progress(false) {}

    void enqueue(const msg_appendentries_t* request,
                msg_appendentries_response_t* response,
                utils::context* complete);

    void start();

    static int buffer_flush(void *arg){
        append_entries_buffer* buff = (append_entries_buffer *)arg;
        buff->do_flush();
        return 0;
    }

    void do_flush();

    void stop();
    void set_in_progress(bool is_in_progress){
        _in_progress = is_in_progress;
    }
private:
    raft_server_t* _raft;
    std::queue<item_type> _request;
    struct spdk_poller * _timer;
    bool _in_progress;
};