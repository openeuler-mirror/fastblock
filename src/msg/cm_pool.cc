/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/sock/cm_pool.h"

#include <spdk/log.h>
#include <spdk/sock.h>
#include <spdk/thread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace msg {
namespace sock {
namespace {

struct job {
    std::string ip;
    uint16_t port{0};
    uint64_t generation{0};
    ::spdk_thread* target{nullptr};
    cm_pool::done_fn cb;
};

struct handoff {
    ::spdk_sock* sock{nullptr};
    uint64_t generation{0};
    cm_pool::done_fn cb;
};

void on_handoff(void* arg) {
    auto* h = static_cast<handoff*>(arg);
    h->cb(h->sock, h->generation);
    delete h;
}

class pool_impl {
public:
    // The pool is a global with joinable worker threads.  Nothing called
    // stop() on the shutdown path, so the workers stayed parked on _cv and
    // were still joinable when the global was destroyed at exit, which is
    // std::terminate() -> "terminate called without an active exception".
    ~pool_impl() { stop(); }

    void ensure_started(int workers, uint32_t queue_depth) {
        std::lock_guard<std::mutex> lk(_mu);
        if (_running) {
            return;
        }
        _queue_depth = queue_depth == 0 ? 64 : queue_depth;
        _running = true;
        const int n = workers > 0 ? workers : 4;
        _threads.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            _threads.emplace_back([this]() { worker(); });
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(_mu);
            if (!_running) {
                return;
            }
            _running = false;
        }
        _cv.notify_all();
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _threads.clear();
        std::lock_guard<std::mutex> lk(_mu);
        while (!_q.empty()) {
            auto j = std::move(_q.front());
            _q.pop_front();
            fail_job(std::move(j));
        }
    }

    bool submit(std::string ip, uint16_t port, uint64_t generation,
                ::spdk_thread* target, cm_pool::done_fn cb) {
        {
            std::lock_guard<std::mutex> lk(_mu);
            if (!_running || _q.size() >= _queue_depth) {
                return false;
            }
            _q.push_back(job{std::move(ip), port, generation, target, std::move(cb)});
        }
        _cv.notify_one();
        return true;
    }

    void cancel(uint64_t generation) {
        std::lock_guard<std::mutex> lk(_mu);
        _cancelled.insert(generation);
    }

    bool is_cancelled(uint64_t generation) {
        std::lock_guard<std::mutex> lk(_mu);
        return _cancelled.find(generation) != _cancelled.end();
    }

    void forget(uint64_t generation) {
        std::lock_guard<std::mutex> lk(_mu);
        _cancelled.erase(generation);
    }

private:
    void fail_job(job j) {
        if (!j.target || !j.cb) {
            return;
        }
        auto* h = new handoff{nullptr, j.generation, std::move(j.cb)};
        if (::spdk_thread_send_msg(j.target, on_handoff, h) != 0) {
            delete h;
        }
    }

    void worker() {
        while (true) {
            job j;
            {
                std::unique_lock<std::mutex> lk(_mu);
                _cv.wait(lk, [this]() { return !_running || !_q.empty(); });
                if (!_running && _q.empty()) {
                    return;
                }
                if (_q.empty()) {
                    continue;
                }
                j = std::move(_q.front());
                _q.pop_front();
                if (_cancelled.erase(j.generation) > 0) {
                    lk.unlock();
                    fail_job(std::move(j));
                    continue;
                }
            }

            ::spdk_sock_opts opts{};
            opts.opts_size = sizeof(opts);
            ::spdk_sock_get_default_opts(&opts);
            opts.zcopy = false;
            ::spdk_sock* s = ::spdk_sock_connect_ext(j.ip.c_str(), j.port, "posix", &opts);
            if (is_cancelled(j.generation)) {
                forget(j.generation);
                if (s) {
                    ::spdk_sock_close(&s);
                }
                fail_job(std::move(j));
                continue;
            }
            auto* h = new handoff{s, j.generation, std::move(j.cb)};
            if (::spdk_thread_send_msg(j.target, on_handoff, h) != 0) {
                if (h->sock) {
                    ::spdk_sock_close(&h->sock);
                }
                delete h;
            }
        }
    }

    std::mutex _mu;
    std::condition_variable _cv;
    std::deque<job> _q;
    std::unordered_set<uint64_t> _cancelled;
    std::vector<std::thread> _threads;
    uint32_t _queue_depth{64};
    bool _running{false};
};

pool_impl g_pool;

} // namespace

cm_pool& cm_pool::instance() {
    static cm_pool p;
    return p;
}

void cm_pool::ensure_started(int workers, uint32_t queue_depth) {
    g_pool.ensure_started(workers, queue_depth);
}

void cm_pool::stop() { g_pool.stop(); }

bool cm_pool::submit(std::string ip, uint16_t port, uint64_t generation,
                     ::spdk_thread* target, done_fn cb) {
    return g_pool.submit(std::move(ip), port, generation, target, std::move(cb));
}

void cm_pool::cancel(uint64_t generation) { g_pool.cancel(generation); }

void cm_pool::forget(uint64_t generation) { g_pool.forget(generation); }

} // namespace sock
} // namespace msg
