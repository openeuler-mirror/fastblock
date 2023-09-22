#include "client/fb_client.h"

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/event.h"

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
    printf("write_callback finished\n");
    exit(0);
}

int connect_poller(void *arg)
{

    if (fblockConnectState() == true)
    {
        SPDK_NOTICELOG("connect success\n");
        sleep(3);
        fblockAioWrite("image_name", "buf", 0, demo_write_callback);
        sleep(5);
        return SPDK_POLLER_BUSY;
    }
    return SPDK_POLLER_IDLE;
}

static void
demo_start(void *arg1)
{
    fblockInit();

    SPDK_NOTICELOG("start to connect\n");
    SPDK_POLLER_REGISTER(connect_poller, NULL, 1000000);
    // client.aio_write("image_name", "buf", 0);

    printf("demo_start,really\n");
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "client_demo";
    opts.print_level = SPDK_LOG_NOTICE;
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