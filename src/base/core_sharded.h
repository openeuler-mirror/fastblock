#pragma once
#include <vector>
#include <string>
#include "spdk/thread.h"
#include "spdk/env.h"

class core_context {
protected:
    virtual void run_task() = 0;
public:
    core_context() {}
    virtual ~core_context() {}       
    static void func(void *arg) {
        core_context *con = (core_context *)arg;
        con->run_task();  
        delete con;
    }  
};

class core_sharded{
public:
    core_sharded(const core_sharded&) = delete;
    core_sharded& operator=(const core_sharded&) = delete;

    int invoke_on(uint32_t shard_id, core_context *context){
        uint32_t core = _shard_cores[shard_id];
        if(core == spdk_env_get_current_core()){
            core_context::func((void *)context);
            return 0;
        }else
            return spdk_thread_send_msg(_threads[shard_id], &core_context::func, (void *)context);
    }

    static core_sharded& get_core_sharded(){
        static core_sharded s_sharded("osd");
        return s_sharded;
    }

    uint32_t count(){
        return _shard_cores.size();
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