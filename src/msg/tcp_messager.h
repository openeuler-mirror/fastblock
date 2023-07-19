#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#include "spdk/sock.h"
#include <string>
#include "spdk/env.h"
#include "src/osd/osd.h"
 
using namespace std;

struct spdk_poller* g_accept_poller;
struct spdk_sock_group* g_group;
struct spdk_sock* g_sock;

struct tcp_messager
{
    std::string ip;
    int port;
};


#endif // TCP_SOCKET_H