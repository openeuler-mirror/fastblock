/* Copyright (c) 2023-2024 ChinaUnicom
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

#include <errno.h>
namespace err{

/*
 1 - 133是errno.h中的错误码
 自己定义的从135开始
*/
enum {
    E_SUCCESS = 0,
    E_NOMEM = -ENOMEM,        /* -12  Out of memory */
    E_BUSY  = -EBUSY,		  /*  -16 Device or resource busy */
    E_NODEV = -ENODEV,		  /*  -19  No such device */
    E_INVAL = -EINVAL,       /*  -22  Invalid argument */
    E_ENOSPC = -ENOSPC,      /* -28  No space left on device */

    RAFT_ERR_NOT_LEADER = -135,
    RAFT_ERR_ONE_VOTING_CHANGE_ONLY = -136,
    RAFT_ERR_SHUTDOWN = -137,
    RAFT_ERR_NOMEM = -138,
    RAFT_ERR_NEEDS_SNAPSHOT = -139,
    RAFT_ERR_SNAPSHOT_IN_PROGRESS = -140,
    RAFT_ERR_SNAPSHOT_ALREADY_LOADED = -141,
    RAFT_ERR_INVALID_CFG_CHANGE = -142,
    RAFT_ERR_NOT_FOUND_LEADER = -143,
    RAFT_ERR_NOT_FOUND_PG  = -144,
    RAFT_ERR_LOG_NOT_MATCH = -145,
    RAFT_ERR_PG_SHUTDOWN = -146,

    RAFT_ERR_UNKNOWN = -199,
    RAFT_ERR_LAST = -200,
};

}