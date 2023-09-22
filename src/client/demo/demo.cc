#include "client/fb_client.h"

char *g_port;

static int demo_parse_arg(int ch, char *arg)
{
    switch (ch)
    {
    case 't':
        g_port = arg;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static void demo_usage(void)
{
    printf(" -t <port>                 port\n");
}

void demo_write_callback()
{
    std::cout << "write_callback finished " << std::endl;
    exit(0);
}

int connect_poller(void *arg)
{
    fblock_client *client = (fblock_client *)arg;
    if (client->connect_state() == true)
    {
        SPDK_NOTICELOG("connect success\n");

        // client->aio_write("image_name", "buf", 0, demo_write_callback);
        return SPDK_POLLER_BUSY;
    }
    return SPDK_POLLER_IDLE;
}

static void
demo_start(void *arg1)
{
    static fblock_client client;

    SPDK_NOTICELOG("start to connect\n");
    SPDK_POLLER_REGISTER(connect_poller, &client, 1000000);
    // client.aio_write("image_name", "buf", 0);

    printf("demo_start,really\n");
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "client_demo";
    opts.print_level = ::spdk_log_level::SPDK_LOG_NOTICE;
    int rc;
    if ((rc = spdk_app_parse_args(argc, argv, &opts, "t:", NULL, demo_parse_arg, demo_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS)
    {
        SPDK_ERRLOG("ERROR: spdk_app_parse_args failed\n");
        return rc;
    }
    rc = spdk_app_start(&opts, demo_start, NULL);
    spdk_app_fini();
    return rc;
}