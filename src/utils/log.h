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
#include <string>
#include <stdio.h>
#include "spdk/log.h"

extern int g_id;

// expand SPDK_XXLOG, we prepend a daemon_id/clientid, so we can do separate osd/client log from rsysylog's output.

#define SPDK_WARNLOG_EX(fmt, ...)                  \
    do                                                      \
    {                                                       \
        std::string new_fmt;                                \
        new_fmt = "daemon_id: " + std::to_string(g_id) + " ";  \
        new_fmt += fmt;                                     \
        SPDK_WARNLOG(new_fmt.c_str(), ##__VA_ARGS__); \
    } while (0)

#define SPDK_ERRLOG_EX(fmt, ...)                  \
    do                                                      \
    {                                                       \
        std::string new_fmt;                                \
        new_fmt = "daemon_id: " + std::to_string(g_id) + " ";  \
        new_fmt += fmt;                                     \
        SPDK_ERRLOG(new_fmt.c_str(), ##__VA_ARGS__); \
    } while (0)


#define SPDK_NOTICELOG_EX(fmt, ...)                  \
    do                                                      \
    {                                                       \
        std::string new_fmt;                                \
        new_fmt = "daemon_id: " + std::to_string(g_id) + " ";  \
        new_fmt += fmt;                                     \
        SPDK_NOTICELOG(new_fmt.c_str(), ##__VA_ARGS__); \
    } while (0)


#define SPDK_INFOLOG_EX(FLAG, fmt, ...)                  \
    do                                                      \
    {                                                       \
        std::string new_fmt;                                \
        new_fmt = "daemon_id: " + std::to_string(g_id) + " ";  \
        new_fmt += fmt;                                     \
        SPDK_INFOLOG(FLAG, new_fmt.c_str(), ##__VA_ARGS__); \
    } while (0)

#define SPDK_DEBUGLOG_EX(FLAG, fmt, ...)                  \
    do                                                       \
    {                                                        \
        std::string new_fmt;                                 \
        new_fmt = "daemon_id: " + std::to_string(g_id) + " ";   \
        new_fmt += fmt;                                      \
        SPDK_DEBUGLOG(FLAG, new_fmt.c_str(), ##__VA_ARGS__); \
    } while (0)
