#pragma once

#include <map>
#include <string>
#include "src/localstore/object_store.h"
#include "src/msg/messages.pb-c.h"


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