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
    RAFT_ERR_NO_CONNECTED = -147,
    RAFT_ERR_PG_DELETED = -148,
    OSD_DOWN = -149,
    OSD_STARTING = -150,
    RAFT_ERR_NO_FOUND_NODE = -151,
    RAFT_ERR_PG_INITIALIZING = -152,
    RAFT_ERR_MEMBERSHIP_CHANGING = -153,

    RAFT_ERR_UNKNOWN = -199,
    RAFT_ERR_LAST = -200,
};

inline const char *  string_status(int raft_errno) noexcept{
    switch (raft_errno) {
    case E_SUCCESS:
        return "success";
    case E_NOMEM:
    case E_BUSY:
    case E_NODEV:
    case E_INVAL:
    case E_ENOSPC:
        return strerror(-1 * raft_errno);
    case RAFT_ERR_NOT_LEADER:
        return "the osd is not the leader of the pg";
    case RAFT_ERR_ONE_VOTING_CHANGE_ONLY:
        return "";
    case RAFT_ERR_SHUTDOWN:
        return "has a seriously wrong";
    case RAFT_ERR_NOMEM:
        return "memory allocation failure";
    case RAFT_ERR_NEEDS_SNAPSHOT:
        return "need snapshot";
    case RAFT_ERR_SNAPSHOT_IN_PROGRESS:
        return "snapshot is in progress";
    case RAFT_ERR_SNAPSHOT_ALREADY_LOADED:
        return "snapshot is aleready loaded";
    case RAFT_ERR_INVALID_CFG_CHANGE:
        return "change config is invalid";
    case RAFT_ERR_NOT_FOUND_LEADER:
        return "No leader found";
    case RAFT_ERR_NOT_FOUND_PG:
        return "no pg found";
    case RAFT_ERR_LOG_NOT_MATCH:
        return "raft log is not match";
    case RAFT_ERR_PG_SHUTDOWN:
        return "pg is shutdown";
    case RAFT_ERR_NO_CONNECTED:
        return "No network connection was created";
    case RAFT_ERR_PG_DELETED:
        return "pg is deleted";
    case OSD_DOWN:
        return "osd is down";
    case OSD_STARTING:
        return "osd is initializing";
    case RAFT_ERR_NO_FOUND_NODE:
        return "node not found";
    case RAFT_ERR_PG_INITIALIZING:
        return "pg it initializing";
    case RAFT_ERR_MEMBERSHIP_CHANGING:
        return "the membership of pg is changing";
    default:
        return "unknown errno";
    }
}

}