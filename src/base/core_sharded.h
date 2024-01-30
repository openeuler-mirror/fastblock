/* Copyright (c) 2024 ChinaUnicom
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
#include <vector>
#include <string>
#include <tuple>
#include <limits>

#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/thread.h>
#include <spdk/util.h>
#include <spdk/log.h>

class core_context {
protected:
    virtual void run_task() = 0;
public:
    core_context() {}
    virtual ~core_context() {}
    static void run(void *arg) {
        core_context *con = (core_context *)arg;
        con->run_task();
        delete con;
    }
};

template <typename Func, typename... Args>
class lambda_ctx : public core_context {
public:
  lambda_ctx(lambda_ctx* l) = delete;
  lambda_ctx(Func func, Args&&... args)
  : func(std::move(func))
  , args(std::make_tuple(std::forward<Args>(args)...))
  { }

  virtual void run_task() override {
    std::apply(func, args);
  }

private:
  Func func;
  std::tuple<Args...> args;
};

class core_sharded{
public:
    core_sharded(const core_sharded&) = delete;
    core_sharded& operator=(const core_sharded&) = delete;

    template <typename Func, typename... Args>
    int invoke_on(uint32_t shard_id, Func func, Args&&... args){
        auto lambda = new lambda_ctx(std::move(func), std::forward<Args>(args)...);

        uint32_t core = _shard_cores[shard_id];
        if(core == spdk_env_get_current_core()){
            core_context::run((void *)lambda);
            return 0;
        }else
            return spdk_thread_send_msg(_threads[shard_id], &core_context::run, (void *)lambda);
    }

    static core_sharded& get_core_sharded(){
        static core_sharded s_sharded("osd");
        return s_sharded;
    }

    uint32_t count(){
        return _shard_cores.size();
    }

    uint32_t this_shard_id() {
        for(uint32_t id = 0; id < _shard_cores.size(); id++) {
            if (_shard_cores[id] == spdk_env_get_current_core()) {
                return id;
            }
        }
        return std::numeric_limits<uint32_t>::max();
    }

private:
    core_sharded(std::string app_name = "osd"){
        uint32_t lcore;
        struct spdk_cpuset cpumask;

        SPDK_ENV_FOREACH_CORE(lcore){
            _shard_cores.push_back(lcore);

            spdk_cpuset_zero(&cpumask);
            spdk_cpuset_set_cpu(&cpumask, lcore, true);
            std::string thread_name = app_name + std::to_string(lcore);

            struct spdk_thread *thread = spdk_thread_create(app_name.c_str(), &cpumask);
            _threads.push_back(thread);
        }
    }
    std::vector<uint32_t> _shard_cores;
    std::vector<struct spdk_thread *> _threads;
};

extern std::vector<uint32_t> get_shard_cores();