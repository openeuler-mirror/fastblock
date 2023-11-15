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

enum {
    ADD_NODE,
    REMOVE_NODE,
    CHANGE_NODES,
    CREATE_PG
};

int g_op_type = ADD_NODE;
int64_t g_pool_version = 0;
std::vector<raft_node_info> g_osd_list;

static void
fbbench_usage(void)
{
    printf(" -I <id>                   osd id\n");
    printf(" -o <osd_addr>             osd address\n");
    printf(" -t <osd_port>             osd port\n");
    printf(" -P <pool_id>              pool id\n");
    printf(" -G <pg_id>                pg id\n");
    printf(" -C <choice>               [add, remove, change, create]\n");
    printf(" -O <osd_list>             osd list, Separated by commas. example: '1:192.168.1.2:8888,2:192.168.1.2:8889'\n");
    printf(" -V <pool_version>         pool version\n");
}

static int
fbbench_parse_arg(int ch, char *arg)
{
    switch (ch)
    {
    case 'I':
        global_osd_id = spdk_strtol(arg, 10);
        break;
    case 'o':
        g_osd_addr = arg;
        break;
    case 't':
        g_osd_port = spdk_strtol(arg, 10);
        break;
    case 'P':
        g_pool_id = spdk_strtoll(arg, 10);
        break;
    case 'G':
        g_pg_id = spdk_strtoll(arg, 10);
        break;   
    case 'C':
    {
        if (!strcmp(arg, "add"))
            g_op_type = ADD_NODE;
        else if (!strcmp(arg, "remove"))
            g_op_type = REMOVE_NODE;
        else if (!strcmp(arg, "change"))
            g_op_type = CHANGE_NODES;  
        else if (!strcmp(arg, "create"))
            g_op_type = CREATE_PG;
        else{
            printf("choice should be add, remove or change");
            return -EINVAL;
        } 
        break;
    }
    case 'O':
    {
        char *str = strdup(arg);
        char *p = nullptr;
        char *tmp = str;

        auto split_osd = [](char *strs){
            char* p = nullptr;
            int i = 0;
            raft_node_info info;
            while(nullptr != (p = spdk_strsepq(&strs, ":"))){
                switch (i){
                case 0:
                    info.set_node_id(spdk_strtoll(p, 10));
                    break;
                case 1:
                    info.set_addr(p);
                    break;
                case 2:
                    info.set_port(spdk_strtoll(p, 10));
                    break;
                default:
                    break;    
                }
                i++;
            }
            g_osd_list.emplace_back(std::move(info));
        }; 

        while(nullptr != (p = spdk_strsepq(&tmp, ","))){
            split_osd(p);
        }
        free(str);
        if(g_osd_list.size() == 0){
            printf("osd_list should be provided\n");
            return -EINVAL;
        }
        break;
    }
    case 'V':
    {
       g_pool_version = spdk_strtoll(arg, 10);
       break;
    }
    default:
        return -EINVAL;
    }
    return 0;
}

static void
raft_membership(void *arg1)
{
    server_t *server = (server_t *)arg1;

    SPDK_NOTICELOG("------block start, cpu count : %u \n", spdk_env_get_core_count());
    osd_client *cli = new osd_client(server);
    cli->create_connect(server->osd_addr, server->osd_port, server->node_id);
    switch (g_op_type){
    case ADD_NODE:
    {
        auto add_node_func = [cli, server, osd_list = g_osd_list](){
            cli->add_node(server->pool_id, server->pg_id, osd_list[0].node_id(), osd_list[0].addr(), osd_list[0].port());
        };
        cli->get_leader(server->node_id, server->pool_id, server->pg_id, add_node_func);
        break;
    }
    case REMOVE_NODE:
    {
        auto remove_node_func = [cli, server, osd_list = g_osd_list](){
            cli->remove_node(server->pool_id, server->pg_id, osd_list[0].node_id(), osd_list[0].addr(), osd_list[0].port());
        };
        cli->get_leader(server->node_id, server->pool_id, server->pg_id, remove_node_func);
        break;
    }
    case CHANGE_NODES:
    {
        auto change_node_func = [cli, server, osd_list = g_osd_list](){
            cli->change_nodes(server->pool_id, server->pg_id, std::move(osd_list));
        };
        cli->get_leader(server->node_id, server->pool_id, server->pg_id, change_node_func);
        break;
    }
    case CREATE_PG:
    {
        cli->create_pg(server->node_id, server->pool_id, server->pg_id, g_pool_version);
        break;
    }
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    server_t server = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "raft_membership";

    // tracing is memory consuming
    opts.num_entries = 0;

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "I:o:t:P:G:C:O:V:", NULL,
                                  fbbench_parse_arg, fbbench_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS)
    {
        exit(rc);
    }

    server.node_id = global_osd_id;
    server.osd_addr = g_osd_addr;
    server.osd_port = g_osd_port;
    server.pool_id = g_pool_id;
    server.pg_id = g_pg_id;

    /* Blocks until the application is exiting */
    rc = spdk_app_start(&opts, raft_membership, &server);

    spdk_app_fini();

    return rc;
}
