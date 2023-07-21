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
    core_sharded(uint32_t core_num, std::string thread_name = "osd")
    : _core_num(core_num) {
        for(auto i = 0; i < _core_num; i++){
            struct spdk_cpuset	tmp_cpumask = {};
	        spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
            std::string _thread_name = thread_name + std::to_string(i);
            struct spdk_thread *thread = spdk_thread_create(_thread_name.c_str(), &tmp_cpumask);   
            _threads.push_back(thread); 
        }
    }

    uint32_t get_core_num(){
        return _core_num;
    }

    int invoke_on(uint32_t core_id, core_context *context){
        if(core_id == spdk_env_get_current_core()){
            core_context::func((void *)context);
            return 0;
        }else
            return spdk_thread_send_msg(_threads[core_id], &core_context::func, (void *)context);
    }

private:
    const uint32_t _core_num;
    std::vector<struct spdk_thread *> _threads;
};