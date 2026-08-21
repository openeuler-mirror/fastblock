/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/sock/client.h"

#include "fastblock/msg/rdma/numeric_addr.h"
#include "fastblock/msg/sock/cm_pool.h"
#include "fastblock/msg/sock/quota.h"

#include <spdk/log.h>
#include <spdk/sock.h>
#include <spdk/thread.h>

#include <algorithm>
#include <chrono>
#include <netinet/in.h>
#include <vector>

namespace msg {
namespace sock {

client::client(std::string name, ::spdk_thread* thread, std::shared_ptr<options> opts)
  : _name(std::move(name))
  , _thread(thread)
  , _opts(std::move(opts))
  , _poller(thread) {
    if (!_opts) {
        _opts = std::make_shared<options>();
    }
    process_quota::instance().set_limit(_opts->max_process_inflight_bytes);
    cm_pool::instance().ensure_started(_opts->connect_workers, _opts->connect_queue_depth);
}

client::~client() noexcept {
    if (_group) {
        ::spdk_sock_group_close(&_group);
    }
}

void client::start(std::optional<std::function<void()>> on_start) {
    auto* self = this;
    auto cb = std::move(on_start);
    ::spdk_thread_send_msg(
      _thread,
      [](void* arg) {
          auto* ctx = static_cast<std::pair<client*, std::optional<std::function<void()>>>*>(arg);
          ctx->first->handle_start();
          if (ctx->second) {
              (*ctx->second)();
          }
          delete ctx;
      },
      new std::pair<client*, std::optional<std::function<void()>>>{self, std::move(cb)});
}

void client::handle_start() {
    if (_started) {
        return;
    }
    _group = ::spdk_sock_group_create(this);
    if (!_group) {
        SPDK_ERRLOG("spdk_sock_group_create failed\n");
        return;
    }
    _started = true;
    _last_scan = std::chrono::system_clock::now();
    _poller.register_poller(
      [](void* arg) { return static_cast<client*>(arg)->poll(); }, this, 0, "rpc_sock_cli");
}

void client::stop(std::optional<std::function<void()>> on_stop) {
    _on_stop = std::move(on_stop);
    ::spdk_thread_send_msg(
      _thread,
      [](void* arg) { static_cast<client*>(arg)->handle_stop(); },
      this);
}

void client::handle_stop() {
    if (_stopping) {
        return;
    }
    _stopping = true;
    for (auto& [gen, p] : _pending_connects) {
        cm_pool::instance().cancel(gen);
        if (p.cb) {
            p.cb(false, nullptr);
        }
    }
    _pending_connects.clear();
    for (auto& c : _conns) {
        if (c) {
            c->async_shutdown();
        }
    }
    _conns.clear();
    _poller.unregister_poller();
    if (_group) {
        ::spdk_sock_group_close(&_group);
        _group = nullptr;
    }
    if (_on_stop) {
        (*_on_stop)();
    }
}

void client::emplace_connection(std::string addr, uint16_t port,
                                std::function<void(bool, std::shared_ptr<connection>)> cb) {
    auto* self = this;
    struct ctx_t {
        client* self;
        std::string addr;
        uint16_t port;
        std::function<void(bool, std::shared_ptr<connection>)> cb;
    };
    auto* ctx = new ctx_t{self, std::move(addr), port, std::move(cb)};
    ::spdk_thread_send_msg(
      _thread,
      [](void* arg) {
          auto* ctx = static_cast<ctx_t*>(arg);
          ::sockaddr_storage ss{};
          if (!rdma::try_numeric_sockaddr(ctx->addr, ctx->port, ss)) {
              ++ctx->self->_probe.connect_fail;
              ctx->cb(false, nullptr);
              delete ctx;
              return;
          }
          const auto gen = ctx->self->_generation++;
          auto cb = std::move(ctx->cb);
          auto* self = ctx->self;
          const auto port = ctx->port;
          auto addr = std::move(ctx->addr);
          delete ctx;
          self->_pending_connects[gen] = pending_connect{
            .cb = std::move(cb),
            .deadline = std::chrono::system_clock::now() + self->_opts->connect_timeout,
          };
          if (!cm_pool::instance().submit(
                std::move(addr), port, gen, self->_thread,
                [self](::spdk_sock* s, uint64_t g) { self->on_connected(s, g); })) {
              self->fail_pending_connect(gen, false);
          }
      },
      ctx);
}

void client::fail_pending_connect(uint64_t generation, bool cancel_cm) {
    auto it = _pending_connects.find(generation);
    if (it == _pending_connects.end()) {
        return;
    }
    if (cancel_cm) {
        cm_pool::instance().cancel(generation);
    }
    auto cb = std::move(it->second.cb);
    _pending_connects.erase(it);
    ++_probe.connect_fail;
    if (cb) {
        cb(false, nullptr);
    }
}

void client::scan_connect_timeouts(std::chrono::system_clock::time_point now) {
    std::vector<uint64_t> expired;
    expired.reserve(_pending_connects.size());
    for (const auto& [gen, p] : _pending_connects) {
        if (now > p.deadline) {
            expired.push_back(gen);
        }
    }
    for (auto gen : expired) {
        fail_pending_connect(gen, true);
    }
}

void client::on_connected(::spdk_sock* s, uint64_t generation) {
    cm_pool::instance().forget(generation);
    auto it = _pending_connects.find(generation);
    if (it == _pending_connects.end()) {
        if (s) {
            ::spdk_sock_close(&s);
        }
        return;
    }
    auto cb = std::move(it->second.cb);
    _pending_connects.erase(it);
    if (_stopping || !s) {
        if (s) {
            ::spdk_sock_close(&s);
        }
        ++_probe.connect_fail;
        if (cb) {
            cb(false, nullptr);
        }
        return;
    }
    auto conn = std::make_shared<connection>(s, _group, _opts, false, &_probe, _thread);
    conn->set_client(this);
    if (!conn->attach()) {
        ++_probe.connect_fail;
        if (cb) {
            cb(false, nullptr);
        }
        return;
    }
    _conns.push_back(conn);
    if (cb) {
        cb(true, conn);
    }
}

void client::remove_connection(std::shared_ptr<connection> conn, std::function<void(bool)> cb) {
    if (conn) {
        conn->async_shutdown();
        std::erase(_conns, conn);
    }
    if (cb) {
        cb(true);
    }
}

int client::poll() {
    std::erase_if(_conns, [](const std::shared_ptr<connection>& c) {
        return !c || c->is_terminated();
    });
    int rc = SPDK_POLLER_IDLE;
    if (_group) {
        const int n = ::spdk_sock_group_poll(_group);
        if (n > 0) {
            rc = SPDK_POLLER_BUSY;
        }
    }
    for (auto& c : _conns) {
        if (c && !c->is_terminated()) {
            c->pump_send();
        }
    }
    const auto now = std::chrono::system_clock::now();
    if (now - _last_scan >= _opts->timeout_scan) {
        _last_scan = now;
        scan_connect_timeouts(now);
        for (auto& c : _conns) {
            if (c && !c->is_terminated()) {
                c->scan_timeouts(now);
            }
        }
    }
    return rc;
}

} // namespace sock
} // namespace msg
