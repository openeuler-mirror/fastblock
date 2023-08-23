#include "rpcbench.h"

static void
block_usage(void)
{
    printf(" -f <path>                 save pid to file under given path\n");
    printf(" -I <id>                   save osd id\n");
    printf(" -o <osd_addr>             osd address\n");
    printf(" -t <osd_port>             osd port\n");
    printf(" -S <io_size>              io_size\n");
    printf(" -k <total_seconds>        seconds to run this bench\n");
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
    osd::bench_request *request = new osd::bench_request();
    char sz[g_io_size + 1];
    sz[g_io_size] = 0;
    request->set_req(sz);
    cli->send_bench_request(server->node_id, request);
}

static const double _cutoffs[] = {
    0.1,
    0.5,
    0.90,
    0.95,
    0.99,
    0.999,
    -1,
};

static void
_check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
              uint64_t total, uint64_t so_far)
{
    double so_far_pct;
    double **cutoff = (double **)ctx;

    if (count == 0)
    {
        return;
    }

    so_far_pct = (double)so_far / total;
    while (so_far_pct >= **cutoff && **cutoff > 0)
    {
        printf("%f%:%fus ", **cutoff * 100, (double)end * 1000 * 1000 / spdk_get_ticks_hz());
        (*cutoff)++;
    }
}

int _rpcbench_print_stats(void *p)
{
    g_seconds++;
    SPDK_NOTICELOG("last second processed: %d\r\n", g_counter - g_counter_last_value);
    g_counter_last_value = g_counter;

    if (g_total_seconds <= g_seconds)
    {
	printf("ops is: %d, average latency is %fus\r\n", (g_counter / g_seconds), double(1000 * 1000 / (g_counter / g_seconds)));
        printf("latency histogram: min:%fus ", double(g_latency_min * 1000 * 1000 / spdk_get_ticks_hz()));
        const double *cutoff = _cutoffs;
        spdk_histogram_data_iterate(g_histogram, _check_cutoff, &cutoff);
        printf("max:%fus", double(g_latency_max * 1000 * 1000 / spdk_get_ticks_hz()));
        printf("\n");

        exit(0);
    }
    return SPDK_POLLER_BUSY;
}

static void
block_started(void *arg1)
{
    // for simpicity, print every second
    _rpcbench_poller_printer = SPDK_POLLER_REGISTER(_rpcbench_print_stats, nullptr, 1000000);
    g_histogram = spdk_histogram_data_alloc();
    server_t *server = (server_t *)arg1;
    SPDK_NOTICELOG("------block start, cpu count : %u \n", spdk_env_get_core_count());
    client *cli = new client(server);
    cli->create_connect(server->osd_addr, server->osd_port, server->node_id);
    usleep(3000);
    _send_request(server, cli);
}

void rpcbench_source::process_response()
{
    // SPDK_NOTICELOG("got response, size is %lu\r\n", response.ByteSizeLong());
    g_counter++;
    auto tsc_diff = spdk_get_ticks() - _submit_tsc;
    if (tsc_diff > g_latency_max) {
        g_latency_max = tsc_diff;
    }
    if (tsc_diff < g_latency_min) {
        g_latency_min = tsc_diff;
    }


    spdk_histogram_data_tally(g_histogram, tsc_diff);

    _send_request(_s, _c);
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    server_t server = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "block";

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:S:I:o:t:k:", NULL,
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
