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

#include <map>
#include <string>
#include "src/localstore/object_store.h"


using namespace std;

struct spdk_ring *g_request_queue;
class osd;
class pg;
osd *g_osd;

class osd
{
private:
    object_store *os;
    std::map<int, pg> *pg_map;

public:
    osd(/* args */);
    ~osd();
};


osd::osd(/* args */)
{
    //FIXME: 这里的参数需要修改
    //下面两行原本注释点的。
    // object_store *os = new object_store();
    // pgmap *pg_map = new pgmap();
    g_request_queue = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 1024, SPDK_ENV_SOCKET_ID_ANY);
};

osd::~osd()
{
    delete os;
    delete pg_map;
};

struct pg
{
    int pgid;
// std:vector<int>  osd_list;
//    raft *raft_group;
};