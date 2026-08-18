/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/sock/server.h"

#include "fastblock/msg/rdma/oob_payload.h"
#include "fastblock/msg/sock/quota.h"

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/sock.h>
#include <spdk/thread.h>

#include <google/protobuf/stubs/callback.h>

#include <cerrno>
#include <cstring>

namespace msg {
namespace sock {

server::server(::spdk_thread* thread, std::shared_ptr<options> opts)
  : _thread(thread)
  , _opts(std::move(opts))
  , _poller(thread) {
    if (!_opts) {
        _opts = std::make_shared<options>();
    }
    process_quota::instance().set_limit(_opts->max_process_inflight_bytes);
}

server::server(std::string thread_name, ::spdk_cpuset* cpumask, std::shared_ptr<options> opts)
  : server(::spdk_thread_create(thread_name.c_str(), cpumask), std::move(opts)) {
    _owns_thread = _thread != nullptr;
}

server::~server() noexcept {
    for (auto& c : _conns) {
        if (c) {
            c->detach_and_close();
        }
    }
    if (_listener) {
        ::spdk_sock_close(&_listener);
    }
    if (_group) {
        ::spdk_sock_group_close(&_group);
    }
}

void server::create_listener(uint16_t port) {
    _opts->port = port;
    ::spdk_sock_opts opts{};
    opts.opts_size = sizeof(opts);
    ::spdk_sock_get_default_opts(&opts);
    opts.zcopy = _opts->zcopy;
    const char* ip = _opts->bind_address.empty() ? "0.0.0.0" : _opts->bind_address.c_str();
    _listener = ::spdk_sock_listen_ext(ip, port, "posix", &opts);
    if (!_listener) {
        throw bind_error(std::string("listen failed on ") + ip + ":" + std::to_string(port) +
                         " errno=" + std::to_string(errno));
    }
}

void server::start() {
    ::spdk_thread_send_msg(
      _thread, [](void* arg) { static_cast<server*>(arg)->handle_start(); }, this);
}

void server::handle_start() {
    if (_started) {
        return;
    }
    _group = ::spdk_sock_group_create(this);
    if (!_group) {
        SPDK_ERRLOG("server sock_group_create failed\n");
        return;
    }
    _started = true;
    _last_scan = std::chrono::system_clock::now();
    _poller.register_poller(
      [](void* arg) { return static_cast<server*>(arg)->poll(); }, this, 0, "rpc_sock_srv");
}

void server::stop(std::optional<std::function<void()>> cb) {
    _on_stop = std::move(cb);
    ::spdk_thread_send_msg(
      _thread,
      [](void* arg) {
          auto* s = static_cast<server*>(arg);
          s->_stopping = true;
          for (auto& c : s->_conns) {
              if (c) {
                  c->async_shutdown();
              }
          }
          s->_conns.clear();
          if (s->_listener) {
              ::spdk_sock_close(&s->_listener);
          }
          s->_poller.unregister_poller([s]() {
              if (s->_group) {
                  ::spdk_sock_group_close(&s->_group);
                  s->_group = nullptr;
              }
              if (s->_on_stop) {
                  (*s->_on_stop)();
              }
          });
      },
      this);
}

void server::add_service(google::protobuf::Service* svc) {
    struct ctx_t {
        server* self;
        google::protobuf::Service* svc;
    };
    auto* ctx = new ctx_t{this, svc};
    ::spdk_thread_send_msg(
      _thread,
      [](void* arg) {
          auto* ctx = static_cast<ctx_t*>(arg);
          auto* d = ctx->svc->GetDescriptor();
          service_rec rec;
          rec.svc = ctx->svc;
          for (int i = 0; i < d->method_count(); ++i) {
              auto* m = d->method(i);
              rec.methods.emplace(m->name(), m);
          }
          ctx->self->_services.emplace(d->name(), std::move(rec));
          delete ctx;
      },
      ctx);
}

void server::accept_one() {
    if (!_listener || _stopping) {
        return;
    }
    ::spdk_sock* s = ::spdk_sock_accept(_listener);
    if (!s) {
        return;
    }
    if (_conns.size() >= _opts->max_conns) {
        ++_probe.accept_drop;
        ::spdk_sock_close(&s);
        return;
    }
    auto conn = std::make_shared<connection>(s, _group, _opts, true, &_probe, _thread);
    conn->set_server(this);
    conn->set_on_request([this](connection* c, assembled_rpc rpc) { dispatch(c, std::move(rpc)); });
    if (!conn->attach()) {
        ::spdk_sock_close(&s);
        return;
    }
    _conns.push_back(std::move(conn));
}

void server::dispatch(connection* conn, assembled_rpc rpc) {
    const auto svc_name =
      std::string(rpc.req_meta.service_name, rpc.req_meta.service_name_size);
    const auto method_name =
      std::string(rpc.req_meta.method_name, rpc.req_meta.method_name_size);
    auto sit = _services.find(svc_name);
    if (sit == _services.end()) {
        conn->send_status(rpc.correlation, rdma::status::service_not_found);
        return;
    }
    auto mit = sit->second.methods.find(method_name);
    if (mit == sit->second.methods.end()) {
        conn->send_status(rpc.correlation, rdma::status::method_not_found);
        return;
    }
    auto* method = mit->second;
    auto* task = new rpc_task{};
    task->correlation = rpc.correlation;
    task->conn = conn;
    task->owner = this;
    task->reserved = rpc.reserved;
    task->ctrlr = std::make_unique<rdma::rpc_controller>();
    task->request.reset(sit->second.svc->GetRequestPrototype(method).New());
    task->response.reset(sit->second.svc->GetResponsePrototype(method).New());
    if (!task->request->ParseFromArray(rpc.proto.data(), static_cast<int>(rpc.proto.size())) ||
        !rdma::attach_rpc_payload(*task->request, rpc.oob)) {
        conn->send_status(rpc.correlation, rdma::status::bad_request_body);
        delete task;
        return;
    }
    auto* done = google::protobuf::NewCallback(&server::on_method_done, task);
    sit->second.svc->CallMethod(method, task->ctrlr.get(), task->request.get(),
                                task->response.get(), done);
}

void server::on_method_done(rpc_task* task) {
    if (!task || task->done) {
        return;
    }
    task->done = true;
    if (!task->conn || task->conn->is_terminated()) {
        delete task;
        return;
    }
    task->conn->remember_done(task->correlation);
    if (task->reserved) {
        process_quota::instance().release(task->reserved);
        task->reserved = 0;
    }
    if (task->ctrlr && task->ctrlr->Failed()) {
        task->conn->send_status(task->correlation, rdma::status::server_error);
    } else {
        task->conn->send_reply(task->correlation, rdma::status::success, task->response.get());
    }
    delete task;
}

int server::poll() {
    accept_one();
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
