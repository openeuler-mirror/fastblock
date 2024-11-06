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

#include <spdk/stdinc.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/event.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <random>

#include "base/core_sharded.h"
#include "raft_bench.h"
#include "localstore/blob_manager.h"
#include "localstore/storage_manager.h"
#include "utils/bdev_json.h"

typedef struct {
    std::string io_type;
    uint32_t     io_size;
    uint32_t     io_count;
    uint32_t     io_depth;
    uint64_t     pool_id;
    uint32_t     pg_num;
    std::string  bdev_type;
    std::string  bdev_addr;
    std::string  bdev_name;
} raft_bench_context;

static char* g_conf_path{nullptr};
static boost::property_tree::ptree g_pt{};
raft_bench_context g_rb_ctx{};
static int g_exit_code{0};
static int g_current_id = 1;

static std::shared_ptr<pg_manager> g_pm{nullptr};

size_t default_obj_size = 4_MB;

enum class stop_state {
    pg_manager,
    blobstore
};

static stop_state cur_stop_state{stop_state::pg_manager};

static void
raft_bench_usage(void) {
    printf("--------------- raft_bench options --------------------\n");
    printf("-C, --conf <json config file> path to json config file\n");
}

static struct option g_cmdline_opts[] = {
#define RAFT_BENCH_OPTION_CONF 'C'
	{
		.name = "conf",
		.has_arg = 1,
		.flag = NULL,
		.val = RAFT_BENCH_OPTION_CONF,
	},
	{
		.name = NULL
	}
};


static int
raft_bench_parse_arg(int ch, char *arg)
{
	switch (ch) {
    case RAFT_BENCH_OPTION_CONF:
        g_conf_path = arg;
        break;
	default:
        raft_bench_usage();
		return -EINVAL;
	}
	return 0;
}

static void on_blob_unloaded([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("The blob has been unloaded, return code is %d, thread id %lu\n",
            bserrno, utils::get_spdk_thread_id());
    auto& sharded_service = core_sharded::get_core_sharded();
    sharded_service.stop();
    SPDK_NOTICELOG("Stop the spdk app\n");
    spdk_app_stop(g_exit_code);
}

static void on_blob_closed([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG(
      "The bdev has been closed, return code is %d, thread id %lu\n",
      bserrno, utils::get_spdk_thread_id());
    SPDK_NOTICELOG("Start unloading bdev\n");
    blobstore_fini(on_blob_unloaded, nullptr);
}

static void on_app_stop() noexcept;

static void on_pm_closed([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("The partition manager has been closed, return code is %d\n", bserrno);
    on_app_stop();
}

static void raft_bench_stop(void *arg) noexcept {
    SPDK_NOTICELOG("Stop the raft bench, thread id %lu\n", utils::get_spdk_thread_id());

    switch (cur_stop_state) {
    case stop_state::pg_manager: {
        cur_stop_state = stop_state::blobstore;
        if(g_pm){
            g_pm->stop(on_pm_closed, nullptr);
            return;
        }
        [[fallthrough]];
    }
    case stop_state::blobstore:{
        storage_fini(on_blob_closed, nullptr);
        return;
    }
    }
}

static void on_app_stop() noexcept {
    auto app_thread = spdk_thread_get_app_thread();
    auto cur_thread = spdk_get_thread();
    if(cur_thread == app_thread){
        raft_bench_stop(nullptr);
    } else {
        spdk_thread_send_msg(app_thread, raft_bench_stop, nullptr);
    }
}

struct task_context{
    uint64_t start_tick;
    uint64_t end_tick;
    std::string type;
    uint32_t io_size;
    uint32_t io_depth;
    uint64_t io_count;
    uint64_t run_io_count;
    uint64_t done_io_count;
    std::string data;
    uint32_t shard_id;
    utils::context *ctx;

    std::vector<std::string> pgs;
};

static std::vector<struct task_context *> g_task_ctx;

static double tick_to_us(const double tick) noexcept {
    return tick * 1000 * 1000 / spdk_get_ticks_hz();
}

static void _run_done(void *arg, int rerror){
    raft_bench_context *ctx = (raft_bench_context *)arg;

    SPDK_WARNLOG("run done.\n");
    for(uint32_t shard_id = 0; shard_id < g_task_ctx.size(); shard_id++){
        auto task_ctx = g_task_ctx[shard_id];
        SPDK_WARNLOG("in shard %u, run_io_count %lu, done_io_count %lu, io_count %lu\n", 
                task_ctx->shard_id, task_ctx->run_io_count, task_ctx->done_io_count, task_ctx->io_count);
    }

    uint64_t tatal_iops = 0;
    double lats = 0;
    SPDK_WARNLOG("----------------------------------------------------------------------\n");
    for(uint32_t shard_id = 0; shard_id < g_task_ctx.size(); shard_id++){
        auto task_ctx = g_task_ctx[shard_id];
        auto ticks = static_cast<double>(task_ctx->end_tick - task_ctx->start_tick);
        auto time_us = tick_to_us(ticks);
        uint64_t iops = (uint64_t)(task_ctx->io_count * 1000 * 1000 / time_us);
        tatal_iops += iops;
        double lat = time_us / task_ctx->io_count;
        lats += lat;
        SPDK_WARNLOG("core %u, io_count: %lu,  iops: %lu   mean: %f\n", 
                shard_id,  task_ctx->io_count, iops, lat);
        delete task_ctx;
        g_task_ctx[shard_id] = nullptr;
    }
    SPDK_WARNLOG("total      , io_count: %u,  iops: %lu   mean: %f\n", 
            ctx->io_count, tatal_iops, lats / g_task_ctx.size());

    SPDK_WARNLOG("----------------------------------------------------------------------\n");

    g_exit_code = 0;
    std::raise(SIGINT);     
}

class write_context : public utils::context {
public:
    write_context(task_context *task_ctx, uint32_t shard_id, uint64_t obj_count)
    : utils::context(false)
    , _task_ctx(task_ctx)
    , _shard_id(shard_id)
    , _obj_count(obj_count) {}

    void finish_del(int r) override {
		if(r != 0){
            SPDK_ERRLOG("write osd failed: %s\n", spdk_strerror(r));
		}

        _task_ctx->done_io_count++;
        if(_task_ctx->done_io_count == _task_ctx->io_count){
            _task_ctx->end_tick = spdk_get_ticks();
            _task_ctx->ctx->complete(r);
            delete this;
            return;
        }  

        if(_task_ctx->run_io_count < _task_ctx->io_count){
            _task_ctx->run_io_count++;
            int index = rand() % _task_ctx->pgs.size();
            std::string pg_name = _task_ctx->pgs[index];
            
            auto & pg_group = g_pm->get_pg_group();
            auto raft = pg_group.get_pg(_shard_id, pg_name);

            uint64_t obj_index = rand() % _obj_count;
            std::string obj_name = pg_name + "__blk_data___" + std::to_string(obj_index);
            // SPDK_WARNLOG("obj_index %lu, obj_name %s\n", obj_index, obj_name.c_str());

            auto osd_stm = g_pm->get_osd_stm(_shard_id, pg_name);
            uint64_t offset = rand() % (default_obj_size - _task_ctx->io_size);
            osd_stm->write_and_wait(obj_name, offset, _task_ctx->data, this); 
        }     
    }

    void finish(int ) override {}
private:
    task_context *_task_ctx;
    uint32_t _shard_id;
    uint64_t _obj_count;
};

static void _run_task(uint32_t shard_id, task_context *task_ctx){
    auto & pg_group = g_pm->get_pg_group();
    task_ctx->pgs = pg_group.get_pgs_name(shard_id);
    auto bs = global_blobstore(shard_id);

    uint64_t bs_size = spdk_bs_get_cluster_size(bs) * spdk_bs_free_cluster_count(bs);
    uint64_t obj_count = (bs_size - rolling_blob::huge_blob_size * task_ctx->pgs.size() - 4_MB * 2) / default_obj_size;
    obj_count = obj_count / 10;

    task_ctx->start_tick = spdk_get_ticks();
    
    auto write_ctx = new write_context(task_ctx, shard_id, obj_count);

    SPDK_WARNLOG("start in shard %u, obj_count %lu, task_ctx->io_depth %u\n", shard_id, obj_count, task_ctx->io_depth);
    for(uint32_t i = 0; i < task_ctx->io_depth; i++){
        int index = rand() % task_ctx->pgs.size();
        std::string pg_name = task_ctx->pgs[index];
        auto raft = pg_group.get_pg(shard_id, pg_name);

        uint64_t obj_index = rand() % obj_count;
        std::string obj_name = pg_name + "__blk_data___"  + std::to_string(obj_index);
        // SPDK_WARNLOG("obj_index %lu, obj_name %s\n", obj_index, obj_name.c_str());

        auto osd_stm = g_pm->get_osd_stm(shard_id, pg_name);
        uint64_t offset = rand() % (default_obj_size - task_ctx->io_size);
        task_ctx->run_io_count++;
        osd_stm->write_and_wait(obj_name, offset, task_ctx->data, write_ctx);
    }
}


static void  bench_start(void *arg, int res){
    raft_bench_context *ctx = (raft_bench_context *)arg;
    if(res != 0){
        g_exit_code = res;
        std::raise(SIGINT);
		return;        
    }
    SPDK_NOTICELOG("create pool done.\n");

    srand((int)time(0)); 

    auto &shard = core_sharded::get_core_sharded();
    auto cur_thread = spdk_get_thread();
    auto shard_num = core_sharded::get_core_sharded().count();
    g_task_ctx.resize(shard_num);

    auto sc_ctx = new utils::switch_core_context{.cb_fn = std::move(_run_done), .arg = arg, .thread = cur_thread, .serror = 0};
    utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, sc_ctx);

    auto total_io_count = ctx->io_count;
    auto io_count_per_core = total_io_count / shard_num;
    for (uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
        auto task_ctx = new task_context();
        g_task_ctx[shard_id] = task_ctx;

        task_ctx->type = ctx->io_type;
        task_ctx->io_size = ctx->io_size;
        if(shard_id == shard_num - 1){
            task_ctx->io_count = total_io_count;
        }else{
            task_ctx->io_count = io_count_per_core;
        }
        total_io_count -= io_count_per_core;
        task_ctx->io_depth = std::min((uint64_t)ctx->io_depth, task_ctx->io_count);
        task_ctx->run_io_count = 0;
        task_ctx->done_io_count = 0;
        task_ctx->data = std::string(task_ctx->io_size, 0x55);
        task_ctx->ctx = complete;
        task_ctx->shard_id = shard_id;
        shard.invoke_on(
          shard_id,
          [shard_id, task_ctx](){
            _run_task(shard_id, task_ctx);
          }
        );
    }
}

struct make_log_context{
    uint64_t pool_id;
    uint64_t pg_id;
    std::vector<utils::osd_info_t> osds;
    uint32_t shard_id;
    pg_manager* pm;
    utils::context *ctx;
};

static void make_log_done(void *arg, struct disk_log* dlog, int rberrno) {
    make_log_context* mlc = (make_log_context*)arg;

    if(rberrno){
        SPDK_ERRLOG("pg %lu.%lu make_disk_log failed: %s\n", mlc->pool_id, mlc->pg_id, spdk_strerror(rberrno));
        mlc->ctx->complete(rberrno);
        delete mlc;
        return;
    }   

    SPDK_NOTICELOG("create pg %lu.%lu in shard %u\n", mlc->pool_id, mlc->pg_id, mlc->shard_id);
    pg_manager* pm = mlc->pm;
    auto sm = std::make_shared<osd_stm>();
    auto pg = pg_id_to_name(mlc->pool_id, mlc->pg_id);
    sm->set_pg(pg);
    pm->add_osd_stm(mlc->pool_id, mlc->pg_id, mlc->shard_id, sm);
    pm->get_pg_group().create_pg(sm, mlc->shard_id, mlc->pool_id, mlc->pg_id, std::move(mlc->osds), dlog);
    
    mlc->ctx->complete(rberrno);
    delete mlc;
}

void pg_manager::create_pg(uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> osds, uint32_t shard_id, utils::context *ctx){
    make_log_context *ml_ctx = new make_log_context{.pool_id = pool_id, .pg_id = pg_id, .osds = move(osds), 
            .shard_id = shard_id, .pm = this, .ctx = ctx};

    std::string pg = pg_id_to_name(pool_id, pg_id);
    make_disk_log(global_blobstore(shard_id), global_io_channel(shard_id), pg, make_log_done, ml_ctx, shard_id);
}

static void create_pool(raft_bench_context *ctx, std::shared_ptr<pg_manager> pm){
    auto &shard = core_sharded::get_core_sharded();
    auto cur_thread = spdk_get_thread();

    SPDK_NOTICELOG("blobstore init, from thread id: %lu\n", utils::get_spdk_thread_id());
    auto shard_num = core_sharded::get_core_sharded().count();

    auto sc_ctx = new utils::switch_core_context{.cb_fn = bench_start, .arg = ctx, .thread = cur_thread, .serror = 0};
    utils::multi_complete *complete = new utils::multi_complete(ctx->pg_num, shard_num, utils::switch_core_func, sc_ctx);
    for(uint32_t pg_id = 0; pg_id < ctx->pg_num; pg_id++){
        uint32_t shard_id = pm->get_next_shard_id();
        shard.invoke_on(
          shard_id,
          [pool_id = ctx->pool_id, pg_id, shard_id, complete, pm](){
            std::vector<utils::osd_info_t> osds;
            utils::osd_info_t osd{.node_id = g_current_id, .isin = true, .isup = true};
            osds.emplace_back(std::move(osd));
            pm->create_pg(pool_id, pg_id, std::move(osds), shard_id, complete);
          }
        );
    }
}

class pm_init_context : public utils::context {
public:
    pm_init_context(raft_bench_context *ctx, std::shared_ptr<pg_manager> pm)
    : _ctx(ctx)
    , _pm(pm) {}

    void finish(int r) override {
		if(r != 0){
            SPDK_ERRLOG("pm init failed: %s\n", spdk_strerror(r));
            g_exit_code = r;
            std::raise(SIGINT);
			return;
		}
        SPDK_NOTICELOG("pm init done.\n");
        create_pool(_ctx, _pm);
    }
private:
    raft_bench_context *_ctx;
    std::shared_ptr<pg_manager> _pm;
};

static void pm_init(void *arg) {
    raft_bench_context *ctx = (raft_bench_context *)arg;

    g_pm = std::make_shared<pg_manager>(g_current_id);
    
    pm_init_context *pm_ctx = new pm_init_context(ctx, g_pm);
    g_pm->start(pm_ctx);
}

static void storage_init_complete(void *arg, int rberrno) {
    if(rberrno != 0){
		SPDK_ERRLOG("Failed to initialize the storage system: %s. thread id %lu\n",
            spdk_strerror(rberrno), utils::get_spdk_thread_id());
        g_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    pm_init(arg);
}

static void blobstore_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the disk: %s. thread id %lu\n",
            err::string_status(rberrno), utils::get_spdk_thread_id());
        g_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    SPDK_INFOLOG(osd, "Initialize the blobstore completed, thread id %lu\n",
                       utils::get_spdk_thread_id());
    storage_init(storage_init_complete, arg);
}

static void on_app_start(void* ) {
    auto core_begin = core_sharded::system::begin();
    auto n_core = std::max(
      core_sharded::system::size_type{1},
      core_sharded::system::capacity());
    core_sharded::construct(core_begin, n_core, "block_bench");

    utils::remove_bdev_json_file();

    if(g_rb_ctx.bdev_type == "nvme")
        g_rb_ctx.bdev_name = g_rb_ctx.bdev_name + "n1";
    
    buffer_pool_init();
    blobstore_init(g_rb_ctx.bdev_name, "1", true, blobstore_init_complete, &g_rb_ctx);
}

static void read_conf_file(){
    boost::property_tree::read_json(std::string(g_conf_path), g_pt);
    
    g_rb_ctx.io_type = g_pt.get_child("io_type").get_value<std::string>();
    g_rb_ctx.io_size = g_pt.get_child("io_size").get_value<uint32_t>();
    g_rb_ctx.io_count = g_pt.get_child("io_count").get_value<uint32_t>();
    g_rb_ctx.io_depth = g_pt.get_child("io_depth").get_value<uint32_t>();
    g_rb_ctx.pool_id = g_pt.get_child("pool_id").get_value<uint64_t>();
    g_rb_ctx.pg_num = g_pt.get_child("pg_num").get_value<uint32_t>();
    g_rb_ctx.bdev_type = g_pt.get_child("bdev_type").get_value<std::string>();
    g_rb_ctx.bdev_addr = g_pt.get_child("bdev_addr").get_value<std::string>();
    g_rb_ctx.bdev_name = g_rb_ctx.bdev_type;
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "raft bench";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
    opts.shutdown_cb = on_app_stop;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:", g_cmdline_opts,
				      raft_bench_parse_arg, raft_bench_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	} 
    
    read_conf_file();
    utils::remove_bdev_json_file();
    std::string bdev_json_file = utils::get_bdev_json_file_name();
    utils::save_bdev_json(bdev_json_file, g_rb_ctx.bdev_type, g_rb_ctx.bdev_name, g_rb_ctx.bdev_addr);
    opts.json_config_file = bdev_json_file.c_str();

    std::string sock_path = "/var/tmp/fastblock-raft-bench.sock";
    opts.rpc_addr = sock_path.c_str();

    rc = spdk_app_start(&opts, on_app_start, nullptr);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
        utils::remove_bdev_json_file();
    }

    spdk_app_fini();
    return rc;
}