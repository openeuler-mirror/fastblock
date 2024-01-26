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

#include "fbbench.h"

static const char* g_json_conf{nullptr};

static void
fbbench_usage(void)
{
    printf(" -C <osd json config file> path to json config file\n");
    printf(" -f <path>                 save pid to file under given path\n");
    printf(" -I <id>                   save osd id\n");
    printf(" -o <osd_addr>             osd address\n");
    printf(" -t <osd_port>             osd port\n");
    printf(" -T <io_type>              type of bench request, support write, rpc\n");
    printf(" -S <io_size>              io_size\n");
    printf(" -k <total_seconds>        total_seconds\n");
    printf(" -Q <queue_depth>          max inflight ios we can submit to cluster\n");
}

static int
fbbench_parse_arg(int ch, char *arg)
{
    switch (ch)
    {
    case 'C':
        g_json_conf = arg;
        break;
    case 'E':
        g_total_ios = spdk_strtol(arg, 10);
        break;
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
    case 'T':
        if (!strcmp(arg, "rpc") && !strcmp(arg, "write"))
        {
            printf("io_type should be rpc or write");
            return -EINVAL;
        }
        if (strcmp(arg, "rpc") == 0)
        {
            g_bench_type = 0;
        }
        else
        {
            g_bench_type = 1;
        }

        break;
    case 'S':
        g_io_size = spdk_strtol(arg, 10);
        break;
    case 'Q':
        g_queue_depth = spdk_strtol(arg, 10);
        break;
    case 'k':
        g_total_seconds = spdk_strtol(arg, 10);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int print_stats(void *p)
{
    g_seconds++;

    SPDK_NOTICELOG("last second processed: %d\n", g_counter - g_counter_last_value);
    g_counter_last_value = g_counter;
    if (g_total_seconds && g_total_seconds <= g_seconds)
    {
        printf("ops is: %d, average latency is %fus\r\n", (g_counter / g_seconds), double(1000 * 1000 * 1.0 / (g_counter / g_seconds)));
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
fbbench_started(void *arg1)
{
    poller_printer = SPDK_POLLER_REGISTER(print_stats, nullptr, 1000000);
    g_histogram = spdk_histogram_data_alloc();

    server_t *server = (server_t *)arg1;

    SPDK_NOTICELOG("------block start, cpu count : %u \n", spdk_env_get_core_count());
    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(
      server->pt.get_child("msg").get_child("client"),
      server->pt.get_child("msg").get_child("rdma"));
    
    client *cli = new client(server, g_bench_type, &cpumask, opts);
    auto connect_done = [cli, server](bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn){
        if (g_total_ios)
        {
            g_first_io_tsc = spdk_get_ticks();
        }
    
        cli->send_request(server->node_id);
    };

    cli->create_connect(server->osd_addr, server->osd_port, server->node_id, std::move(connect_done));
}

static void process_response(client *c, uint64_t id)
{
    if (c->get_type() == 1)
    {
        if (c->get_wr().state() != 0)
        {
            SPDK_ERRLOG("write request failed, state is %d, most likely this node is not the raft leader\r\n", c->get_wr().state());
            exit(-1);
        }
    }

    auto tsc_diff = spdk_get_ticks() - c->get_op_tsc(id);
    if (tsc_diff > g_latency_max)
    {
        g_latency_max = tsc_diff;
    }
    if (tsc_diff < g_latency_min)
    {
        g_latency_min = tsc_diff;
    }
    c->remove_op_from_inflight_list(id);

    spdk_histogram_data_tally(g_histogram, tsc_diff);
    c->decrease_inflight_ops();
    g_counter++;

    // stop sending more ios
    if (g_total_ios && g_counter >= g_total_ios)
    {
        auto tsc_diff_2 = spdk_get_ticks() - g_first_io_tsc;
        printf(
          "ops is: %d, average latency is %fus\r\n",
          int(g_counter / float((1.0 * tsc_diff_2 / spdk_get_ticks_hz()))),
          float(1000.0 * 1000.0 / (g_counter / float(1.0 * tsc_diff_2 / spdk_get_ticks_hz()))));
        printf("latency histogram: min:%fus ", double(g_latency_min * 1000 * 1000 / spdk_get_ticks_hz()));
        const double *cutoff = _cutoffs;
        spdk_histogram_data_iterate(g_histogram, _check_cutoff, &cutoff);
        printf("max:%fus", double(g_latency_max * 1000 * 1000 / spdk_get_ticks_hz()));
        printf("\n");
        exit(-1);
    }

    c->send_request(c->get_server()->node_id);
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    server_t server = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "fbbench";

    // tracing is memory consuming
    opts.num_entries = 0;

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:E:f:I:o:t:S:T:k:Q:", NULL,
                                  fbbench_parse_arg, fbbench_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS)
    {
        exit(rc);
    }

    if ((g_total_ios == 0 && g_total_seconds == 0) || (g_total_ios != 0 && g_total_seconds != 0))
    {
        printf("you should configure either total ios or total seconds");
        exit(-1);
    }

    server.node_id = global_osd_id;
    server.osd_addr = g_osd_addr;
    server.osd_port = g_osd_port;

    SPDK_NOTICELOG("config file is %s\n", g_json_conf);
    boost::property_tree::read_json(std::string(g_json_conf), server.pt);

    /* Blocks until the application is exiting */
    rc = spdk_app_start(&opts, fbbench_started, &server);

    spdk_app_fini();

    return rc;
}
