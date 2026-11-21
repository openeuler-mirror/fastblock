/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/sock/connection.h"

#include "fastblock/msg/oob_payload.h"
#include "fastblock/msg/sock/quota.h"

#include <spdk/log.h>
#include <spdk/sock.h>
#include <spdk/thread.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

SPDK_LOG_REGISTER_COMPONENT(msg_sock)

namespace msg {
namespace sock {
namespace {

struct send_req {
    ::spdk_sock_request req{};
    ::iovec iov[1]{};
    std::string bytes;
    std::shared_ptr<connection> hold;
    connection* conn{nullptr};
};
static_assert(offsetof(send_req, iov) == sizeof(::spdk_sock_request),
              "iov must immediately follow spdk_sock_request");

} // namespace

connection::connection(::spdk_sock* sock, ::spdk_sock_group* group, std::shared_ptr<options> opts,
                       bool is_server, probe* pr, ::spdk_thread* thread)
  : _sock(sock)
  , _group(group)
  , _opts(std::move(opts))
  , _is_server(is_server)
  , _probe(pr)
  , _thread(thread) {
    _recv.resize(64 * 1024);
}

connection::~connection() noexcept {
    detach_and_close();
}

void connection::apply_sock_opts() {
    if (!_sock || !_opts) {
        return;
    }
    ::spdk_sock_set_recvbuf(_sock, static_cast<int>(_opts->recv_buf_size));
    ::spdk_sock_set_sendbuf(_sock, static_cast<int>(_opts->send_buf_size));
}

static void on_sock_event(void* arg, ::spdk_sock_group* /*group*/, ::spdk_sock* /*sock*/) {
    auto* c = static_cast<connection*>(arg);
    c->on_readable();
}

bool connection::attach() {
    if (!_sock || !_group || _in_group) {
        return _in_group;
    }
    apply_sock_opts();
    if (::spdk_sock_group_add_sock(_group, _sock, on_sock_event, this) != 0) {
        SPDK_ERRLOG("sock_group_add_sock failed\n");
        return false;
    }
    _in_group = true;
    return true;
}

void connection::detach_and_close() {
    if (_in_group && _group && _sock) {
        ::spdk_sock_group_remove_sock(_group, _sock);
        _in_group = false;
    }
    if (_sock) {
        ::spdk_sock_close(&_sock);
    }
}

int connection::async_shutdown() {
    if (_thread && _thread != ::spdk_get_thread()) {
        return ::spdk_thread_send_msg(
          _thread,
          [](void* arg) { static_cast<connection*>(arg)->teardown("async_shutdown"); },
          this);
    }
    teardown("async_shutdown");
    return 0;
}

void connection::teardown(const char* why) {
    if (_torn) {
        return;
    }
    _torn = true;
    // Fail closures (heartbeat, raft remove_connect) may drop the last
    // shared_ptr. Hold until inflight completions finish so complete_rpc
    // does not write into a destroyed pending_rpc / this.
    auto hold = shared_from_this();
    if (_probe) {
        ++_probe->conn_teardown;
    }
    if (!why || (std::strcmp(why, "eof") != 0 && std::strcmp(why, "async_shutdown") != 0)) {
        SPDK_NOTICELOG("sock connection teardown: %s\n", why ? why : "");
    }
    detach_and_close();
    std::vector<std::unique_ptr<pending_rpc>> dying;
    dying.reserve(_inflight.size());
    for (auto& [k, req] : _inflight) {
        dying.push_back(std::move(req));
    }
    _inflight.clear();
    for (auto& [k, as] : _assemblies) {
        if (_is_server && !as.a.response) {
            release_inflight(as.reserved);
            as.reserved = 0;
        }
    }
    _assemblies.clear();
    _priority_q.clear();
    _normal_q.clear();
    for (auto& req : dying) {
        complete_rpc(req.get(), rpc_fail::connection_dead);
    }
}

void connection::release_inflight(size_t reserved) {
    if (reserved) {
        process_quota::instance().release(reserved);
        if (_inflight_bytes >= reserved) {
            _inflight_bytes -= reserved;
        } else {
            _inflight_bytes = 0;
        }
    }
    if (_inflight_rpcs > 0) {
        --_inflight_rpcs;
    }
}

void connection::finish_server_rpc(size_t reserved) {
    release_inflight(reserved);
}

void connection::cancel_rpc(uint64_t correlation) {
    auto inf = _inflight.find(correlation);
    if (inf == _inflight.end()) {
        return;
    }
    complete_rpc(inf->second.get(), rpc_fail::canceled);
    _inflight.erase(inf);
}

void connection::complete_rpc(pending_rpc* req, rpc_fail why, const char* detail) {
    if (!req || req->completed) {
        if (req && req->completed && _probe) {
            ++_probe->closure_double;
        }
        return;
    }
    req->completed = true;
    _done_keys.remember(req->correlation);
    release_inflight(req->reserved);
    req->reserved = 0;
    switch (why) {
    case rpc_fail::ok:
        if (_probe) {
            ++_probe->complete_ok;
        }
        break;
    case rpc_fail::resource_exhausted:
        rdma::fail_rpc(req->ctrlr, "resource_exhausted", rdma::rpc_error_class::resource_exhausted);
        if (_probe) {
            ++_probe->exhausted;
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::timeout:
        rdma::fail_rpc(req->ctrlr, "timeout", rdma::rpc_error_class::timeout);
        if (_probe) {
            ++_probe->timeout;
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::request_timeout:
        rdma::fail_rpc(req->ctrlr, "request_timeout", rdma::rpc_error_class::request_timeout);
        if (_probe) {
            ++_probe->timeout;
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::too_large:
        rdma::fail_rpc(req->ctrlr, "too_large", rdma::rpc_error_class::too_large);
        if (_probe) {
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::connection_dead:
        rdma::fail_rpc(req->ctrlr, "connection terminated", rdma::rpc_error_class::connection);
        if (_probe) {
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::unserialize:
        rdma::fail_rpc(req->ctrlr, detail ? detail : "unserialize error",
                       rdma::rpc_error_class::application);
        if (_probe) {
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::other:
        rdma::fail_rpc(req->ctrlr, detail ? detail : "rpc error", rdma::rpc_error_class::application);
        if (_probe) {
            ++_probe->complete_fail;
        }
        break;
    case rpc_fail::canceled:
        rdma::fail_rpc(req->ctrlr, "canceled", rdma::rpc_error_class::canceled);
        if (_probe) {
            ++_probe->complete_fail;
        }
        break;
    }
    auto* cl = req->closure;
    req->closure = nullptr;
    if (cl) {
        cl->Run();
    }
}

void connection::on_readable() {
    if (_torn || !_sock) {
        return;
    }
    size_t got_this_tick = 0;
    while (got_this_tick < k_max_read_per_tick) {
        if (_recv.size() - _recv_off < 4096) {
            _recv.resize(_recv.size() + 64 * 1024);
        }
        ::iovec iov{};
        iov.iov_base = _recv.data() + _recv_off;
        iov.iov_len = _recv.size() - _recv_off;
        const ssize_t n = ::spdk_sock_readv(_sock, &iov, 1);
        if (n == 0) {
            teardown("eof");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            teardown("read error");
            return;
        }
        _recv_off += static_cast<size_t>(n);
        got_this_tick += static_cast<size_t>(n);
        if (!_partial) {
            _partial = true;
            _recv_deadline = std::chrono::system_clock::now() + _opts->rpc_timeout;
        }
        const auto st = ingest_bytes();
        if (tears_connection(st)) {
            if (_probe) {
                ++_probe->bad_hdr;
            }
            teardown("bad header");
            return;
        }
        if (_recv_off == 0) {
            _partial = false;
            if (_recv.size() > k_recv_shrink) {
                _recv.resize(64 * 1024);
            }
        }
    }
}

hdr_status connection::ingest_bytes() {
    while (true) {
        if (!_have_hdr) {
            const auto st = load_hdr(_recv.data(), _recv_off, _opts->max_frame_bytes, &_cur_hdr);
            if (st == hdr_status::need_more) {
                return hdr_status::ok;
            }
            if (st != hdr_status::ok) {
                return st;
            }
            _have_hdr = true;
            _cur_need = static_cast<uint32_t>(
              sizeof(sock_frame_hdr) + _cur_hdr.meta_bytes + _cur_hdr.proto_bytes +
              _cur_hdr.payload_bytes);
        }
        if (_recv_off < _cur_need) {
            return hdr_status::ok;
        }
        const char* p = _recv.data() + sizeof(sock_frame_hdr);
        std::string_view meta{p, _cur_hdr.meta_bytes};
        p += _cur_hdr.meta_bytes;
        std::string_view proto{p, _cur_hdr.proto_bytes};
        p += _cur_hdr.proto_bytes;
        std::string_view payload{p, _cur_hdr.payload_bytes};
        handle_complete_frame(_cur_hdr, meta, proto, payload);
        const size_t remain = _recv_off - _cur_need;
        if (remain > 0) {
            std::memmove(_recv.data(), _recv.data() + _cur_need, remain);
        }
        _recv_off = remain;
        _have_hdr = false;
        _cur_need = 0;
    }
}

void connection::handle_complete_frame(const sock_frame_hdr& hdr, std::string_view meta,
                                       std::string_view proto, std::string_view payload) {
    const bool rsp = (hdr.flags & k_flag_response) != 0;
    auto it = _assemblies.find(hdr.correlation);
    if (hdr.frame_seq == 0) {
        size_t inbound_reserved = 0;
        if (it != _assemblies.end()) {
            if (_is_server && !rsp) {
                send_status(hdr.correlation, rdma::status::bad_request_body);
            }
            return;
        }
        if (rsp) {
            if (_inflight.find(hdr.correlation) == _inflight.end()) {
                if (_done_keys.contains(hdr.correlation)) {
                    return;
                }
                if (_probe) {
                    ++_probe->unknown_key;
                }
                return;
            }
        } else if (_is_server) {
            if (_done_keys.contains(hdr.correlation)) {
                return;
            }
            if (meta.size() == sizeof(wire_request_meta)) {
                const auto rm = load_request_meta(meta.data());
                const size_t need = static_cast<size_t>(rm.data_size) + rm.payload_size;
                if (need > _opts->max_rpc_bytes ||
                    _inflight_rpcs >= _opts->max_inflight_rpcs ||
                    _inflight_bytes + need > _opts->max_inflight_bytes ||
                    !process_quota::instance().try_reserve(need)) {
                    send_status(hdr.correlation, rdma::status::resource_exhausted);
                    if (_probe) {
                        ++_probe->exhausted;
                    }
                    return;
                }
                inbound_reserved = need;
            }
        }
        live_assembly la{};
        const auto st = push_frame(&la.a, hdr, meta, proto, payload, true);
        if (st == assemble_status::payload_mismatch) {
            if (inbound_reserved) {
                process_quota::instance().release(inbound_reserved);
            }
            if (_is_server && !rsp) {
                send_status(hdr.correlation, rdma::status::resource_exhausted);
            }
            return;
        }
        if (_is_server && !rsp && !la.a.meta.empty()) {
            const auto rm = load_request_meta(la.a.meta.data());
            la.reserved = static_cast<size_t>(rm.data_size) + rm.payload_size;
            _inflight_bytes += la.reserved;
            ++_inflight_rpcs;
        }
        la.deadline = std::chrono::system_clock::now() + _opts->rpc_timeout;
        if (st == assemble_status::complete) {
            assembled_rpc done{};
            done.response = la.a.response;
            done.correlation = la.a.correlation;
            done.proto = std::move(la.a.proto);
            done.oob = std::move(la.a.oob);
            done.reserved = la.reserved;
            if (done.response) {
                done.reply_meta = load_reply_meta(la.a.meta.data());
            } else {
                done.req_meta = load_request_meta(la.a.meta.data());
            }
            if (done.response) {
                auto inf = _inflight.find(done.correlation);
                if (inf == _inflight.end()) {
                    return;
                }
                auto* req = inf->second.get();
                const auto rst = static_cast<rdma::status>(done.reply_meta.reply_status);
                if (rst == rdma::status::success || rst == rdma::status::no_content) {
                    if (req->response &&
                        (!req->response->ParseFromArray(done.proto.data(),
                                                        static_cast<int>(done.proto.size())) ||
                         !rdma::attach_rpc_payload(*req->response, done.oob))) {
                        complete_rpc(req, rpc_fail::unserialize);
                    } else {
                        complete_rpc(req, rpc_fail::ok);
                    }
                } else if (rst == rdma::status::resource_exhausted) {
                    complete_rpc(req, rpc_fail::resource_exhausted);
                } else if (rst == rdma::status::request_timeout) {
                    complete_rpc(req, rpc_fail::request_timeout);
                } else {
                    complete_rpc(req, rpc_fail::other, rdma::string_status(rst));
                }
                _inflight.erase(inf);
            } else if (_on_request) {
                _on_request(this, std::move(done));
            }
            return;
        }
        _assemblies.emplace(hdr.correlation, std::move(la));
        return;
    }

    if (it == _assemblies.end()) {
        if (_probe) {
            ++_probe->unknown_key;
        }
        return;
    }
    it->second.deadline = std::chrono::system_clock::now() + _opts->rpc_timeout;
    const auto st = push_frame(&it->second.a, hdr, meta, proto, payload, false);
    if (st == assemble_status::seq_hole) {
        if (_is_server && !rsp) {
            send_status(hdr.correlation, rdma::status::bad_request_body);
        } else {
            auto inf = _inflight.find(hdr.correlation);
            if (inf != _inflight.end()) {
                complete_rpc(inf->second.get(), rpc_fail::timeout);
                _inflight.erase(inf);
            }
        }
        release_inflight(it->second.reserved);
        it->second.reserved = 0;
        _assemblies.erase(it);
        return;
    }
    if (st == assemble_status::need_more) {
        return;
    }
    if (st != assemble_status::complete) {
        release_inflight(it->second.reserved);
        it->second.reserved = 0;
        _assemblies.erase(it);
        teardown("payload mismatch");
        return;
    }
    assembled_rpc done{};
    done.response = it->second.a.response;
    done.correlation = it->second.a.correlation;
    done.proto = std::move(it->second.a.proto);
    done.oob = std::move(it->second.a.oob);
    done.reserved = it->second.reserved;
    if (done.response) {
        done.reply_meta = load_reply_meta(it->second.a.meta.data());
    } else {
        done.req_meta = load_request_meta(it->second.a.meta.data());
    }
    _assemblies.erase(it);
    if (done.response) {
        auto inf = _inflight.find(done.correlation);
        if (inf == _inflight.end()) {
            return;
        }
        auto* req = inf->second.get();
        const auto rst = static_cast<rdma::status>(done.reply_meta.reply_status);
        if (rst == rdma::status::success || rst == rdma::status::no_content) {
            if (req->response &&
                (!req->response->ParseFromArray(done.proto.data(),
                                                static_cast<int>(done.proto.size())) ||
                 !rdma::attach_rpc_payload(*req->response, done.oob))) {
                complete_rpc(req, rpc_fail::unserialize);
            } else {
                complete_rpc(req, rpc_fail::ok);
            }
        } else if (rst == rdma::status::resource_exhausted) {
            complete_rpc(req, rpc_fail::resource_exhausted);
        } else if (rst == rdma::status::request_timeout) {
            complete_rpc(req, rpc_fail::request_timeout);
        } else {
            complete_rpc(req, rpc_fail::other, rdma::string_status(rst));
        }
        _inflight.erase(inf);
    } else if (_on_request) {
        _on_request(this, std::move(done));
    }
}

void connection::enqueue_frames(std::vector<encoded_frame> frames, bool priority) {
    auto& q = priority ? _priority_q : _normal_q;
    for (auto& fr : frames) {
        q.push_back(std::move(fr));
    }
    pump_send();
}

void connection::on_send_done(void* arg, int err) {
    auto* r = static_cast<send_req*>(arg);
    auto hold = std::move(r->hold);
    auto* conn = r->conn;
    delete r;
    if (!hold || !conn || conn->_torn) {
        return;
    }
    if (conn->_async_inflight > 0) {
        --conn->_async_inflight;
    }
    if (err < 0) {
        conn->teardown("send error");
        return;
    }
    conn->pump_send();
}

void connection::submit_one_frame(encoded_frame fr) {
    auto* r = new send_req{};
    r->bytes = std::move(fr.bytes);
    r->hold = shared_from_this();
    r->conn = this;
    r->req.cb_fn = on_send_done;
    r->req.cb_arg = r;
    r->req.iovcnt = 1;
    r->iov[0].iov_base = r->bytes.data();
    r->iov[0].iov_len = r->bytes.size();
    ++_async_inflight;
    ::spdk_sock_writev_async(_sock, &r->req);
}

void connection::pump_send() {
    if (_torn || !_sock) {
        return;
    }
    while (_async_inflight < 8) {
        encoded_frame fr;
        if (!_priority_q.empty()) {
            fr = std::move(_priority_q.front());
            _priority_q.pop_front();
        } else if (!_normal_q.empty()) {
            fr = std::move(_normal_q.front());
            _normal_q.pop_front();
        } else {
            break;
        }
        submit_one_frame(std::move(fr));
    }
    ::spdk_sock_flush(_sock);
}

void connection::CallMethod(const google::protobuf::MethodDescriptor* method,
                            google::protobuf::RpcController* ctrlr,
                            const google::protobuf::Message* request,
                            google::protobuf::Message* response,
                            google::protobuf::Closure* done) {
    if (_torn) {
        rdma::fail_rpc(ctrlr, "connection terminated", rdma::rpc_error_class::connection);
        if (done) {
            done->Run();
        }
        return;
    }
    auto& svc = method->service()->name();
    auto& mname = method->name();
    if (svc.size() > rdma::max_rpc_meta_string_size ||
        mname.size() > rdma::max_rpc_meta_string_size) {
        rdma::fail_rpc(ctrlr, "service/method name too long", rdma::rpc_error_class::application);
        if (done) {
            done->Run();
        }
        return;
    }

    // Peel a clone. fb_client retries the same write_request after NOT_LEADER;
    // mutating the caller's bytes field would send data_len>0 with empty OOB.
    std::unique_ptr<google::protobuf::Message> owned(request->New());
    owned->CopyFrom(*request);
    std::string oob;
    rdma::peel_rpc_payload(*owned, &oob);
    const auto proto_sz = owned->ByteSizeLong();
    if (proto_sz > k_proto_hard_cap) {
        rdma::fail_rpc(ctrlr, "too_large", rdma::rpc_error_class::too_large);
        if (done) {
            done->Run();
        }
        return;
    }
    const size_t rpc_sz = proto_sz + oob.size();
    if (rpc_sz > _opts->max_rpc_bytes) {
        const bool single_object = oob.size() > _opts->max_rpc_bytes;
        rdma::fail_rpc(ctrlr, single_object ? "too_large" : "resource_exhausted",
                       single_object ? rdma::rpc_error_class::too_large
                                     : rdma::rpc_error_class::resource_exhausted);
        if (_probe) {
            ++_probe->exhausted;
        }
        if (done) {
            done->Run();
        }
        return;
    }
    if (_inflight.size() >= static_cast<size_t>(_opts->rpc_batch_size) ||
        _inflight_bytes + rpc_sz > _opts->max_inflight_bytes ||
        !process_quota::instance().try_reserve(rpc_sz)) {
        rdma::fail_rpc(ctrlr, "resource_exhausted", rdma::rpc_error_class::resource_exhausted);
        if (_probe) {
            ++_probe->exhausted;
        }
        if (done) {
            done->Run();
        }
        return;
    }

    uint64_t key = 0;
    for (int n = 0; n < 8; ++n) {
        key = make_correlation(_key_gen, _key_idx);
        if (++_key_idx == 0) {
            ++_key_gen;
            _key_idx = 1;
        }
        if (!_done_keys.contains(key) && _inflight.find(key) == _inflight.end()) {
            break;
        }
    }

    wire_request_meta wm{};
    wm.service_name_size = static_cast<uint8_t>(svc.size());
    wm.method_name_size = static_cast<uint8_t>(mname.size());
    std::memcpy(wm.service_name, svc.data(), svc.size());
    std::memcpy(wm.method_name, mname.data(), mname.size());
    wm.data_size = static_cast<uint32_t>(proto_sz);
    wm.payload_size = static_cast<uint32_t>(oob.size());
    std::string meta(sizeof(wm), '\0');
    store_request_meta(meta.data(), wm);
    std::string proto(proto_sz, '\0');
    owned->SerializeToArray(proto.data(), static_cast<int>(proto_sz));

    std::vector<encoded_frame> frames;
    if (!split_rpc(false, key, meta, proto, oob, _opts->max_frame_bytes, &frames)) {
        process_quota::instance().release(rpc_sz);
        rdma::fail_rpc(ctrlr, "too_large", rdma::rpc_error_class::too_large);
        if (done) {
            done->Run();
        }
        return;
    }

    auto req = std::make_unique<pending_rpc>();
    req->correlation = key;
    req->ctrlr = ctrlr;
    req->response = response;
    req->closure = done;
    req->start_at = std::chrono::system_clock::now();
    req->reserved = rpc_sz;
    req->priority = mname == "heartbeat";
    _inflight_bytes += rpc_sz;
    ++_inflight_rpcs;
    auto* raw = req.get();
    _inflight.emplace(key, std::move(req));
    if (auto* rc = static_cast<rdma::rpc_controller*>(ctrlr)) {
        auto wp = std::weak_ptr<connection>(shared_from_this());
        rc->set_cancel_fn([wp, key]() {
            if (auto self = wp.lock()) {
                self->cancel_rpc(key);
            }
        });
        if (rc->IsCanceled()) {
            cancel_rpc(key);
            return;
        }
    }
    enqueue_frames(std::move(frames), raw->priority);
}

void connection::send_status(uint64_t correlation, rdma::status st) {
    wire_reply_meta rm{};
    rm.reply_status = static_cast<uint8_t>(st);
    std::string meta(sizeof(rm), '\0');
    store_reply_meta(meta.data(), rm);
    std::vector<encoded_frame> frames;
    split_rpc(true, correlation, meta, {}, {}, _opts->max_frame_bytes, &frames);
    enqueue_frames(std::move(frames), true);
}

void connection::send_reply(uint64_t correlation, rdma::status st, google::protobuf::Message* body) {
    std::string oob;
    std::string proto;
    if (body && st == rdma::status::success) {
        rdma::peel_rpc_payload(*body, &oob);
        proto.resize(body->ByteSizeLong());
        body->SerializeToArray(proto.data(), static_cast<int>(proto.size()));
    }
    const size_t rpc_sz = proto.size() + oob.size();
    if (rpc_sz > _opts->max_rpc_bytes) {
        send_status(correlation, rdma::status::resource_exhausted);
        if (_probe) {
            ++_probe->exhausted;
        }
        return;
    }
    wire_reply_meta rm{};
    rm.reply_status = static_cast<uint8_t>(st);
    rm.data_size = static_cast<uint32_t>(proto.size());
    rm.payload_size = static_cast<uint32_t>(oob.size());
    std::string meta(sizeof(rm), '\0');
    store_reply_meta(meta.data(), rm);
    std::vector<encoded_frame> frames;
    if (!split_rpc(true, correlation, meta, proto, oob, _opts->max_frame_bytes, &frames)) {
        send_status(correlation, rdma::status::resource_exhausted);
        return;
    }
    enqueue_frames(std::move(frames), false);
}

void connection::inject_unknown_frame() {
    sock_frame_hdr h{};
    h.magic = k_magic;
    h.version = k_version;
    h.flags = k_flag_response;
    h.frame_seq = 0;
    h.correlation = 0x0eadbeefULL;
    h.meta_bytes = sizeof(wire_reply_meta);
    encoded_frame fr;
    fr.hdr = h;
    fr.bytes.resize(sizeof(sock_frame_hdr) + sizeof(wire_reply_meta));
    store_hdr(fr.bytes.data(), h);
    wire_reply_meta rm{};
    rm.reply_status = static_cast<uint8_t>(rdma::status::success);
    store_reply_meta(fr.bytes.data() + sizeof(sock_frame_hdr), rm);
    enqueue_frames({std::move(fr)}, true);
}

bool connection::scan_timeouts(std::chrono::system_clock::time_point now) {
    auto hold = shared_from_this();
    bool busy = false;
    if (_partial && now > _recv_deadline) {
        teardown("recv_deadline");
        return true;
    }
    for (auto it = _assemblies.begin(); it != _assemblies.end();) {
        if (now <= it->second.deadline) {
            ++it;
            continue;
        }
        if (_probe) {
            ++_probe->assembly_timeout;
        }
        if (_is_server && !it->second.a.response) {
            send_status(it->first, rdma::status::request_timeout);
            release_inflight(it->second.reserved);
            it->second.reserved = 0;
        } else {
            auto inf = _inflight.find(it->first);
            if (inf != _inflight.end()) {
                complete_rpc(inf->second.get(), rpc_fail::timeout);
                _inflight.erase(inf);
            }
        }
        it = _assemblies.erase(it);
        busy = true;
    }
    for (auto it = _inflight.begin(); it != _inflight.end();) {
        auto* rc = static_cast<rdma::rpc_controller*>(it->second->ctrlr);
        if (rc && rc->IsCanceled()) {
            complete_rpc(it->second.get(), rpc_fail::canceled);
            it = _inflight.erase(it);
            busy = true;
            continue;
        }
        if (it->second->start_at + _opts->rpc_timeout < now) {
            complete_rpc(it->second.get(), rpc_fail::timeout);
            it = _inflight.erase(it);
            busy = true;
        } else {
            ++it;
        }
    }
    return busy;
}

} // namespace sock
} // namespace msg
