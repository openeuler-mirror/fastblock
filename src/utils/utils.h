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
#pragma once
#include <chrono>

#include <random>
#include <string>
#include <functional>

#include "spdk/env.h"
#include "spdk/thread.h"
namespace utils {

constexpr int32_t MIN_OSD_PORT = 9000;
constexpr int32_t MAX_OSD_PORT = 10000;

constexpr int32_t default_monitor_port = 3333;

constexpr uint32_t default_blobstore_core = 0;

class context {
protected:
    virtual void finish(int r) = 0;
    virtual void finish_del(int r) {}
public:
    context(bool needs_delete = true)
    : _needs_delete(needs_delete) {}

    virtual ~context() {}
    virtual void complete(int r) {
        if(_needs_delete){
            finish(r);
            delete this;
        }else{
            finish_del(r);
        }
    }
private:
    bool _needs_delete;
};

using complete_fun = std::function<void (void *arg, int res)>;
struct multi_complete : public context{
    int count;
    int num;
    pthread_mutex_t mutex;
    complete_fun fun;
    void *arg;
    int core_num;
    int merrno;

    multi_complete(int _count, int _core_num, complete_fun _fun, void *_arg)
    : context(false)
    , count(_count)
    , num(0)
    , mutex(PTHREAD_MUTEX_INITIALIZER)
    , fun(_fun)
    , arg(_arg) 
    , core_num(_core_num)
    , merrno(0) {}

    void finish_del(int rerrno) override{
        if(_need_mutex())
            pthread_mutex_lock(&mutex);
        if(rerrno != 0)
            merrno = rerrno;
        num++;
        if(num == count){
            if(_need_mutex())
                pthread_mutex_unlock(&mutex);
            fun(arg, merrno);
            delete this;
        }else{
            if(_need_mutex())
                pthread_mutex_unlock(&mutex);
        }
    }

    void finish(int ) override {}
private:
    bool _need_mutex(){
        if(count > 1 && core_num > 1)
            return true;
        return false;
    }    
};

inline void complete_done(void *arg, int serrno) {
    utils::multi_complete *complete = (utils::multi_complete *)arg;
    complete->complete(serrno);    
}

struct osd_info_t
{
	int node_id;
	bool isin;
	bool isup;
	bool ispendingcreate;
	int port;
	std::string address;
};

struct pg_info_type {
    uint64_t pg_id{0};
    int64_t   version{0};
    std::vector<int> osds{};
};

template <typename T>
inline constexpr
T align_up(T v, T align) {
    return (v + align - 1) & ~(align - 1);
}

template <typename T>
inline constexpr
T align_down(T v, T align) {
    return v & ~(align - 1);
}

inline int64_t get_time(){
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    return now_ms.time_since_epoch().count();
}

template<typename T>
static T random_int(const T min, const T max) {
    std::random_device rd{};
    std::uniform_int_distribution<T> dist{min, max};
    return dist(rd);
}

static std::string random_string(const size_t length) {
    static std::string chars{
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "1234567890"
      "!@#$%^&*()"
      "`~-_=+[{]}\\|;:'\",<.>/? "};

    std::random_device rd{};
    std::uniform_int_distribution<decltype(chars)::size_type> index_dist{0, chars.size() - 1};
    std::string ret(length, ' ');
    for (size_t i{0}; i < length; ++i) {
        ret[i] = chars[index_dist(rd)];
    }

    return ret;
}

static int get_random_port(){
    int port = random_int(MIN_OSD_PORT, MAX_OSD_PORT);
    return port;
}

static uint64_t  get_spdk_thread_id(){
    auto thread = spdk_get_thread();
    if(thread)
        return spdk_thread_get_id(thread);
    return 0;
}

enum class operation_type : uint16_t {
  NONE = 0,
  READ,
  WRITE,
  DELETE
};

struct cluster_io{
    uint64_t read_ios;
    uint64_t read_bytes;
    uint64_t write_ios;
    uint64_t write_bytes;  
    int64_t  objects;  
};

struct switch_core_context{
  utils::complete_fun cb_fn;
  void *arg;
  spdk_thread *thread;
  int serror;
};

static inline void _switch_core_func(void *arg){
  switch_core_context *ctx = (switch_core_context *)arg;
  ctx->cb_fn(ctx->arg, ctx->serror);
  delete ctx;
}

static void switch_core_func(void *arg, int rerrno){
  switch_core_context *ctx = (switch_core_context *)arg;
  auto cur_thread = spdk_get_thread();
  ctx->serror = rerrno;
  if(cur_thread != ctx->thread){
    spdk_thread_send_msg(ctx->thread, _switch_core_func, ctx);
  }else{
    _switch_core_func(ctx);
  }
}

}
