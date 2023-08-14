#include "opbench.h"

static void
block_usage(void)
{
    printf(" -f <path>                 save pid to file under given path\n");
    printf(" -I <id>                   save osd id\n");
    printf(" -o <osd_addr>             osd address\n");
    printf(" -t <osd_port>             osd port\n");
    printf(" -S <io_size>              io_size\n");
    printf(" -k <total_seconds>        total_seconds\n");
}

static int
block_parse_arg(int ch, char *arg)
{
    switch (ch)
    {
    case 'f':
        g_pid_path = arg;
        break;
    case 'I':
        global_osd_id = spdk_strtol(arg, 10);
        break;
    case 'o':
        g_osd_addr = arg;
        break;
    case 't':
        g_osd_port = spdk_strtol(arg, 10);
        break;
    case 'S':
        g_io_size = spdk_strtol(arg, 10);
        break;
    case 'k':
        g_total_seconds = spdk_strtol(arg, 10);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

void _send_request(server_t *server, client *cli)
{
    osd::write_request *request = new osd::write_request();
    request->set_pool_id(1);
    request->set_pg_id(0);
    request->set_object_name("obj");
    request->set_offset(0);
    char sz[g_io_size + 1];
    memset(sz, 0x55, g_io_size);
    sz[4096] = 0;
    request->set_data(sz);
    cli->send_write_request(server->node_id, request);
}

int print_stats(void *p)
{
    g_seconds++;

    SPDK_NOTICELOG("last second processed: %d\r\n", g_counter - g_counter_last_value);
    g_counter_last_value = g_counter;
    if (g_total_seconds <= g_seconds)
    {
        SPDK_NOTICELOG("ops is: %d\r\n", (g_counter / g_seconds));
        exit(0);
    }

    return SPDK_POLLER_BUSY;
}

static void
block_started(void *arg1)
{
    poller_printer = SPDK_POLLER_REGISTER(print_stats, nullptr, 1000000);

    server_t *server = (server_t *)arg1;

    SPDK_NOTICELOG("------block start, cpu count : %u \n", spdk_env_get_core_count());
    client *cli = new client(server);
    cli->create_connect(server->osd_addr, server->osd_port, server->node_id);

    //(fixme)wait connection, should make it as a callback
    usleep(3000);
    _send_request(server, cli);
}

void opbench_source::process_response()
{
    // SPDK_NOTICELOG("got response, size is %lu\r\n", response.ByteSizeLong());
    if (response.state() != 0)
    {
        SPDK_NOTICELOG("write request failed, state is %d", response.state());
    }

    g_counter++;

    _send_request(_s, _c);
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    server_t server = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "block";

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:I:o:t:S:k:", NULL,
                                  block_parse_arg, block_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS)
    {
        exit(rc);
    }

    server.node_id = global_osd_id;
    server.osd_addr = g_osd_addr;
    server.osd_port = g_osd_port;

    /* Blocks until the application is exiting */
    rc = spdk_app_start(&opts, block_started, &server);

    spdk_app_fini();

    return rc;
}