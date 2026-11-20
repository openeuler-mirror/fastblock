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

#include "osd_stm.h"
#include "data_statistics.h"
#include "raft/raft.h"
#include "fastblock/rpc/osd_msg.pb.h"
#include "fastblock/utils/utils.h"
#include "fastblock/utils/err_num.h"
#include "localstore/aos/log_store.h"
#include "localstore/aos/osd_store.h"
#include "localstore/aos/types.h"
#include "localstore/aos/pg_store.h"
#include "localstore/aos/snapshot_ingest.h"
#include <errno.h>
#include <concepts>
#include <cstring>
#include <vector>

// Write 8 units at a time, i.e. 4K
#define BLOCK_UNITS 8

osd_stm::osd_stm()
: state_machine()
, _object_rw_lock(){
    uint32_t lcore = spdk_env_get_current_core();
    _sockid = spdk_env_get_socket_id(lcore);
}

osd_stm::~osd_stm() = default;

namespace {

const char* ingest_state_name(fastblock::aos::ingest_state s) {
    switch (s) {
    case fastblock::aos::ingest_state::idle:
        return "idle";
    case fastblock::aos::ingest_state::ingesting:
        return "ingesting";
    case fastblock::aos::ingest_state::sealed:
        return "sealed";
    case fastblock::aos::ingest_state::catchup:
        return "catchup";
    }
    return "unknown";
}

fastblock::aos::pg_store* recovery_pg(fastblock::aos::osd_store* aos,
                                      uint64_t pool_id, uint64_t pg_id) {
    if (aos == nullptr) {
        return nullptr;
    }
    auto* serving = aos->get_active_pg(pool_id, pg_id);
    if (serving != nullptr && serving->object_count() > 0) {
        return serving;
    }
    return aos->find_pg(pool_id, pg_id);
}

} // namespace

void osd_stm::attach_aos(fastblock::aos::osd_store* store, uint64_t pool_id,
                         uint64_t pg_id) {
    _aos = store;
    _pool_id = pool_id;
    _pg_id = pg_id;
    SPDK_NOTICELOG("osd_stm attach_aos pool %lu pg %lu (object path=AOS only)\n",
                   pool_id, pg_id);
}

bool osd_stm::uses_aos_objects() const { return _aos != nullptr; }

std::vector<std::string> osd_stm::list_live_objects() {
    std::vector<std::string> names;
    if (_aos == nullptr) {
        return names;
    }
    auto* pg = recovery_pg(_aos, _pool_id, _pg_id);
    if (pg == nullptr) {
        SPDK_NOTICELOG("pg %lu.%lu list_live_objects: no pg serving=%lu\n",
                       _pool_id, _pg_id,
                       _aos->serving_epoch(_pool_id, _pg_id));
        return names;
    }
    pg->list_live_object_names(&names);
    SPDK_NOTICELOG(
        "pg %lu.%lu list_live_objects n=%zu epoch=%lu serving=%lu latest=%lu "
        "objects=%lu\n",
        _pool_id, _pg_id, names.size(), pg->id().store_epoch,
        _aos->serving_epoch(_pool_id, _pg_id),
        _aos->latest_epoch(_pool_id, _pg_id),
        static_cast<unsigned long>(pg->object_count()));
    return names;
}

bool osd_stm::object_exists(const std::string& name) {
    if (_aos == nullptr) {
        return false;
    }
    auto* pg = _aos->get_active_pg(_pool_id, _pg_id);
    return pg != nullptr && pg->object_exists(name);
}

void osd_stm::pin_recovery_view(uint64_t raft_apply_index, uint64_t raft_term,
                                object_rw_complete cb_fn, void* arg) {
    if (_aos == nullptr) {
        if (cb_fn) {
            cb_fn(arg, -ENODEV);
        }
        return;
    }
    auto* pg = recovery_pg(_aos, _pool_id, _pg_id);
    if (pg == nullptr) {
        if (cb_fn) {
            cb_fn(arg, -ENOENT);
        }
        return;
    }
    _recovery_snap_index = raft_apply_index;
    _recovery_pinned = true;
    if (raft_apply_index == 0) {
        // Empty PG / nothing applied: still OK for install of empty state.
        if (cb_fn) {
            cb_fn(arg, 0);
        }
        return;
    }
    pg->create_raft_snapshot(raft_apply_index, raft_term, std::move(cb_fn), arg);
}

void osd_stm::unpin_recovery_view(object_rw_complete cb_fn, void* arg) {
    if (_aos == nullptr || !_recovery_pinned) {
        _recovery_pinned = false;
        _recovery_snap_index = 0;
        if (cb_fn) {
            cb_fn(arg, 0);
        }
        return;
    }
    auto* pg = recovery_pg(_aos, _pool_id, _pg_id);
    const uint64_t idx = _recovery_snap_index;
    _recovery_pinned = false;
    _recovery_snap_index = 0;
    if (pg == nullptr || idx == 0) {
        if (cb_fn) {
            cb_fn(arg, 0);
        }
        return;
    }
    pg->release_raft_snapshot(idx, std::move(cb_fn), arg);
}

void osd_stm::read_recovery_object(const std::string& object_name, char* buf,
                                   uint64_t len, object_rw_complete cb_fn,
                                   void* arg) {
    if (_aos == nullptr) {
        if (cb_fn) {
            cb_fn(arg, -ENODEV);
        }
        return;
    }
    // Prefer pinned recovery view; else head. Same PG as list_live_objects.
    const uint64_t raft_idx = _recovery_pinned ? _recovery_snap_index : 0;
    auto* pg = recovery_pg(_aos, _pool_id, _pg_id);
    if (pg == nullptr) {
        if (cb_fn) {
            cb_fn(arg, -ENOENT);
        }
        return;
    }
    pg->read(object_name, 0, buf, len, raft_idx, /*image*/ 0, std::move(cb_fn),
             arg);
}

void osd_stm::apply(std::shared_ptr<raft_entry_t> entry, utils::context *complete){
    const uint64_t raft_idx = static_cast<uint64_t>(entry->idx());
    if (fastblock::aos::unified_log_enabled() && _aos != nullptr &&
        (entry->type() == RAFT_LOGTYPE_WRITE ||
         entry->type() == RAFT_LOGTYPE_DELETE)) {
        // P6: data already on disk from append_as_log; apply only publishes.
        std::string name;
        utils::operation_type unlock_type = utils::operation_type::WRITE;
        if (entry->type() == RAFT_LOGTYPE_WRITE) {
            osd::write_cmd write;
            write.ParseFromString(entry->meta());
            name = write.object_name();
            unlock_type = utils::operation_type::WRITE;
        } else {
            osd::delete_cmd del;
            del.ParseFromString(entry->meta());
            name = del.object_name();
            unlock_type = utils::operation_type::DELETE;
        }
        struct pub_ctx {
            osd_stm* stm;
            std::string name;
            utils::operation_type unlock_type;
            utils::context* complete;
        };
        auto* ctx = new pub_ctx{this, std::move(name), unlock_type, complete};
        _aos->publish_commit(
            _pool_id, _pg_id, raft_idx,
            [](void* a, int err) {
                auto* c = static_cast<pub_ctx*>(a);
                c->stm->unlock(c->name, c->unlock_type);
                c->complete->complete(err);
                delete c;
            },
            ctx);
        return;
    }
    if(entry->type() == RAFT_LOGTYPE_WRITE){
        osd::write_cmd write;
        write.ParseFromString(entry->meta());
        // Prefer per-entry image_snap_seq from log meta; fall back to STM field.
        const uint64_t img =
            write.image_snap_seq() != 0 ? write.image_snap_seq()
                                       : _image_snap_seq;
        write_obj(write.object_name(), write.offset(), entry->data(), raft_idx,
                  img, complete);
    }else if(entry->type() == RAFT_LOGTYPE_DELETE){
        osd::delete_cmd del;
        del.ParseFromString(entry->meta());
        const uint64_t img =
            del.image_snap_seq() != 0 ? del.image_snap_seq() : _image_snap_seq;
        delete_obj(del.object_name(), raft_idx, img, complete);
    }else{
        complete->complete(err::E_SUCCESS);
    }
}

struct write_obj_ctx{
    osd_stm* stm;
    std::string obj_name;
    utils::context *complete;
};

static void write_obj_done(void *arg, int obj_errno){
    write_obj_ctx * ctx = (write_obj_ctx *)arg;
    SPDK_DEBUGLOG(osd, "write obj %s pg: %s done in core: %u err %d\n",
        ctx->obj_name.c_str(), ctx->stm->get_pg_name().c_str(),
        core_sharded::get_core_sharded().this_shard_id(), obj_errno);
    ctx->stm->unlock(ctx->obj_name, utils::operation_type::WRITE);
    ctx->complete->complete(obj_errno);
    delete ctx;
}

void osd_stm::write_obj(const std::string& obj_name, uint64_t offset,
                        const std::string& data, uint64_t raft_write_index,
                        uint64_t image_snap_seq, utils::context *complete){
    if (_aos == nullptr) {
        SPDK_ERRLOG("write_obj without AOS pg %s\n", get_pg_name().c_str());
        complete->complete(-ENODEV);
        return;
    }

    write_obj_ctx * ctx = new write_obj_ctx{this, obj_name, complete};
    SPDK_DEBUGLOG(osd, "write obj %s pg: %s raft_idx %lu image_seq %lu aos core %u\n",
        obj_name.c_str(), get_pg_name().c_str(), raft_write_index, image_snap_seq,
        core_sharded::get_core_sharded().this_shard_id());

    _aos->apply(_pool_id, _pg_id, obj_name, offset, data.data(), data.size(),
                raft_write_index, image_snap_seq, write_obj_done, ctx);
}

void osd_stm::delete_obj(const std::string& obj_name, uint64_t raft_write_index,
                         uint64_t image_snap_seq, utils::context *complete){
    if (_aos == nullptr) {
        _object_rw_lock.unlock(obj_name, utils::operation_type::DELETE);
        complete->complete(-ENODEV);
        return;
    }
    struct del_ctx {
        osd_stm* stm;
        std::string name;
        utils::context* complete;
    };
    auto* d = new del_ctx{this, obj_name, complete};
    _aos->remove(
        _pool_id, _pg_id, obj_name, raft_write_index, image_snap_seq,
        [](void* a, int err) {
            auto* c = static_cast<del_ctx*>(a);
            c->stm->unlock(c->name, utils::operation_type::DELETE);
            c->complete->complete(err);
            delete c;
        },
        d);
}

void osd_stm::create_image_snapshot(uint64_t image_snap_seq,
                                    object_rw_complete cb, void* arg) {
    if (_aos == nullptr) {
        if (cb) {
            cb(arg, -ENODEV);
        }
        return;
    }
    _aos->create_image_snapshot(_pool_id, _pg_id, image_snap_seq, std::move(cb),
                                arg);
}

void osd_stm::release_image_snapshot(uint64_t image_snap_seq,
                                     object_rw_complete cb, void* arg) {
    if (_aos == nullptr) {
        if (cb) {
            cb(arg, -ENODEV);
        }
        return;
    }
    _aos->release_image_snapshot(_pool_id, _pg_id, image_snap_seq,
                                 std::move(cb), arg);
}

void osd_stm::flatten_image(uint64_t parent_image_seq, uint64_t new_image_seq,
                            uint64_t raft_write_index, object_rw_complete cb,
                            void* arg) {
    if (_aos == nullptr) {
        if (cb) {
            cb(arg, -ENODEV);
        }
        return;
    }
    _aos->flatten_image(_pool_id, _pg_id, parent_image_seq, new_image_seq,
                        raft_write_index, std::move(cb), arg);
}

void osd_stm::write_install_object(const std::string& obj_name, char* buf,
                                   std::size_t size, uint64_t snapshot_index,
                                   object_rw_complete cb_fn, void* arg) {
    if (_aos == nullptr) {
        if (cb_fn) {
            cb_fn(arg, -ENODEV);
        }
        return;
    }
    if (_ingest == nullptr ||
        _ingest->state() != fastblock::aos::ingest_state::ingesting) {
        SPDK_ERRLOG(
            "pg %lu.%lu write_install_object %s while ingest state=%s\n",
            _pool_id, _pg_id, obj_name.c_str(),
            _ingest ? ingest_state_name(_ingest->state()) : "null");
        if (cb_fn) {
            cb_fn(arg, -EINVAL);
        }
        return;
    }
    (void)snapshot_index;
    _ingest->apply_chunk(obj_name, /*offset*/ 0, buf, size, /*crc*/ 0,
                         std::move(cb_fn), arg);
}

void osd_stm::begin_install_snapshot(uint64_t snapshot_index,
                                     uint64_t snapshot_term,
                                     object_rw_complete cb_fn, void* arg) {
    if (cb_fn == nullptr) {
        return;
    }
    if (_aos == nullptr) {
        cb_fn(arg, 0);
        return;
    }
    if (_ingest == nullptr) {
        _ingest = std::make_unique<fastblock::aos::snapshot_ingest>(
            _aos, _pool_id, _pg_id);
    }
    const auto st = _ingest->state();
    if (st == fastblock::aos::ingest_state::ingesting &&
        _ingest->snapshot_index() == snapshot_index) {
        cb_fn(arg, 0);
        return;
    }
    if (st != fastblock::aos::ingest_state::idle) {
        SPDK_NOTICELOG(
            "pg %lu.%lu snapshot ingest abort state=%s old_snap=%lu new_snap=%lu\n",
            _pool_id, _pg_id, ingest_state_name(st),
            _ingest->snapshot_index(), snapshot_index);
        _ingest->abort(
            [this, snapshot_index, snapshot_term, cb_fn, arg](void*, int aerr) {
                if (aerr != 0) {
                    cb_fn(arg, aerr);
                    return;
                }
                begin_install_snapshot(snapshot_index, snapshot_term, cb_fn,
                                       arg);
            },
            nullptr);
        return;
    }
    _ingest->begin(
        snapshot_index, snapshot_term,
        [this, snapshot_index, snapshot_term, cb_fn, arg](void*, int err) {
            SPDK_NOTICELOG(
                "pg %lu.%lu snapshot ingest idle -> %s snap=%lu term=%lu "
                "serving=%lu staging=%lu err=%d\n",
                _pool_id, _pg_id,
                _ingest ? ingest_state_name(_ingest->state()) : "null",
                snapshot_index, snapshot_term,
                _ingest ? _ingest->active_epoch() : 0,
                _ingest ? _ingest->staging_epoch() : 0, err);
            cb_fn(arg, err);
        },
        nullptr);
}

void osd_stm::bind_raft_log_to_serving() {
    if (_aos == nullptr) {
        return;
    }
    auto* raft = get_raft();
    if (raft == nullptr || !raft->raft_get_log()) {
        return;
    }
    auto* lg = _aos->get_active_log(_pool_id, _pg_id);
    if (lg == nullptr) {
        SPDK_ERRLOG("pg %lu.%lu bind_raft_log_to_serving: no serving log "
                    "serving=%lu\n",
                    _pool_id, _pg_id, _aos->serving_epoch(_pool_id, _pg_id));
        return;
    }
    raft->raft_get_log()->retarget_aos_log(lg);
    SPDK_NOTICELOG(
        "pg %lu.%lu retarget raft log to serving epoch %lu last_index=%lu\n",
        _pool_id, _pg_id, lg->id().store_epoch, lg->last_index());
}

void osd_stm::finish_install_snapshot(object_rw_complete cb_fn, void* arg) {
    if (cb_fn == nullptr) {
        return;
    }
    if (_ingest == nullptr ||
        _ingest->state() == fastblock::aos::ingest_state::idle) {
        cb_fn(arg, 0);
        return;
    }

    auto finish_idle = [this, cb_fn, arg](void*, int err) {
        if (err == 0) {
            bind_raft_log_to_serving();
        }
        if (err == 0 && _ingest) {
            _ingest->finish_catchup(
                [this, cb_fn, arg](void*, int ferr) {
                    SPDK_NOTICELOG(
                        "pg %lu.%lu snapshot ingest catchup -> %s serving=%lu "
                        "err=%d\n",
                        _pool_id, _pg_id,
                        _ingest ? ingest_state_name(_ingest->state()) : "null",
                        _aos ? _aos->serving_epoch(_pool_id, _pg_id) : 0,
                        ferr);
                    cb_fn(arg, ferr);
                },
                nullptr);
            return;
        }
        cb_fn(arg, err);
    };

    auto do_activate = [this, finish_idle](void*, int err) {
        if (err != 0) {
            finish_idle(nullptr, err);
            return;
        }
        SPDK_NOTICELOG("pg %lu.%lu snapshot ingest %s -> activating\n",
                       _pool_id, _pg_id, ingest_state_name(_ingest->state()));
        _ingest->activate(finish_idle, nullptr);
    };

    if (_ingest->state() == fastblock::aos::ingest_state::ingesting &&
        _ingest->chunk_count() == 0) {
        // Leader sent done=1 with no objects. Do not promote an empty
        // staging epoch over the existing serving generation.
        SPDK_NOTICELOG(
            "pg %lu.%lu snapshot ingest empty (no chunks); abort, keep serving=%lu\n",
            _pool_id, _pg_id, _aos->serving_epoch(_pool_id, _pg_id));
        _ingest->abort(
            [cb_fn, arg](void*, int aerr) { cb_fn(arg, aerr); }, nullptr);
        return;
    }
    if (_ingest->state() == fastblock::aos::ingest_state::catchup) {
        finish_idle(nullptr, 0);
        return;
    }
    if (_ingest->state() == fastblock::aos::ingest_state::sealed) {
        do_activate(nullptr, 0);
        return;
    }
    SPDK_NOTICELOG("pg %lu.%lu snapshot ingest %s -> sealing snap=%lu\n",
                   _pool_id, _pg_id, ingest_state_name(_ingest->state()),
                   _ingest->snapshot_index());
    _ingest->seal(do_activate, nullptr);
}


template<class rsp_type>
concept rsp_type_valid = (
       std::is_same_v<rsp_type, osd::write_reply>    ||
       std::is_same_v<rsp_type, osd::read_reply>      ||
       std::is_same_v<rsp_type, osd::delete_reply>
    );

template<class rsp_type>
requires rsp_type_valid<rsp_type>
struct osd_service_complete : public utils::context{
    using type = std::remove_reference_t<std::decay_t<rsp_type>>;

    rsp_type* response;
    google::protobuf::Closure* done;
    osd_stm* stm;
    std::string obj_name;
    uint64_t length;

    osd_service_complete(osd_stm* _stm, std::string _obj_name,
            uint64_t _length, rsp_type* _response, google::protobuf::Closure* _done)
    : response(_response)
    , done(_done)
    , stm(_stm)
    , obj_name(std::move(_obj_name))
    , length(_length){}

    void finish(int r) override {
        SPDK_DEBUGLOG(osd, "process osd service done.\n");
        if(r != 0){
            SPDK_ERRLOG("process osd service failed: %d\n", r);
            if(std::is_same_v<type, osd::write_reply>){
                stm->unlock(obj_name, utils::operation_type::WRITE);
            }else if(std::is_same_v<type, osd::delete_reply>){
                stm->unlock(obj_name, utils::operation_type::DELETE);
            }
        }
        if(std::is_same_v<type, osd::read_reply>){
            stm->unlock(obj_name, utils::operation_type::READ);
            if(r == 0 && stm->raft_is_leader())
                g_data_statistics->add_data(utils::operation_type::READ, stm->get_pg_name(), length);
        } else if(std::is_same_v<type, osd::write_reply>){
            if(r == 0 && stm->raft_is_leader())
                g_data_statistics->add_data(utils::operation_type::WRITE, stm->get_pg_name(), length);
        }
        response->set_state(r);
        done->Run();
    }
};

using lock_complete_func = std::function<void ()>;

struct lock_complete : public utils::context{
    lock_complete_func func;
    lock_complete(lock_complete_func&& _func)
    : func(std::move(_func)) {}

    void finish(int ) override {
        func();
    }
};

void osd_stm::write_and_wait(
            const osd::write_request* request,
            osd::write_reply* response,
            google::protobuf::Closure* done){

    osd_service_complete<osd::write_reply> *write_complete =
      new osd_service_complete<osd::write_reply>(this, request->object_name(),
        request->data().size(), response, done);

    auto write_func = [this, request, write_complete](){
        osd::write_cmd cmd;
        cmd.set_object_name(request->object_name());
        cmd.set_offset(request->offset());
        if (request->image_snap_seq() != 0) {
            cmd.set_image_snap_seq(request->image_snap_seq());
        } else if (_image_snap_seq != 0) {
            cmd.set_image_snap_seq(_image_snap_seq);
        }
        std::string buf;
        cmd.SerializeToString(&buf);

        SPDK_DEBUGLOG(osd, "process write_request , pg %lu.%lu object_name %s offset %lu len %lu image_seq %lu\n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str(), request->offset(),
                     request->data().size(), cmd.image_snap_seq());

        auto entry_ptr = std::make_shared<raft_entry_t>();
        entry_ptr->set_type(RAFT_LOGTYPE_WRITE);
        entry_ptr->set_meta(std::move(buf));
        entry_ptr->set_data(std::move(request->data()));

        get_raft()->raft_write_entry(entry_ptr, write_complete);
    };

    lock_complete *complete = new lock_complete(std::move(write_func));
    _object_rw_lock.lock(request->object_name(), utils::operation_type::WRITE, complete);
}

struct read_obj_ctx{
    char* buf{nullptr};
    utils::context *complete{nullptr};
    osd::read_reply* response{nullptr};
    uint64_t size{0};
};

void read_obj_done(void *arg, int obj_errno){
    read_obj_ctx * ctx = (read_obj_ctx *)arg;
    if (obj_errno == 0) {
        ctx->response->set_data(ctx->buf, ctx->size);
    }
    ctx->complete->complete(obj_errno);
    spdk_free(ctx->buf);
    delete ctx;
}

void osd_stm::read_and_wait(
            const osd::read_request* request,
            osd::read_reply* response,
            google::protobuf::Closure* done){

    osd_service_complete<osd::read_reply> *read_complete =
      new osd_service_complete<osd::read_reply>(this, request->object_name(),
        request->length(), response, done);

    auto read_func = [this, request, response, read_complete](){
        if(!linearization()){
            SPDK_INFOLOG(osd, "!linearization\n");
            read_complete->complete(err::RAFT_ERR_NOT_LEADER);
            return;
        }

        SPDK_INFOLOG(osd, "process read_request , pool %lu pg %lu object_name %s offset %lu len %lu\n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str(), request->offset(),
                     request->length());

        if (_aos == nullptr) {
            read_complete->complete(-ENODEV);
            return;
        }

        uint64_t len = utils::align_up<uint64_t>(request->length(), 512 * BLOCK_UNITS);
        char* buf = (char*)spdk_zmalloc(len, 0x1000, NULL, _sockid, SPDK_MALLOC_DMA);
        if (buf == nullptr) {
            read_complete->complete(-ENOMEM);
            return;
        }
        read_obj_ctx * ctx = new read_obj_ctx{};
        ctx->buf = buf;
        ctx->complete = read_complete;
        ctx->response = response;
        ctx->size = request->length();

        _aos->read(_pool_id, _pg_id, request->object_name(),
                   request->offset(), buf, request->length(),
                   request->raft_read_index(), request->image_read_seq(),
                   read_obj_done, ctx);
    };

    lock_complete *complete = new lock_complete(std::move(read_func));
    _object_rw_lock.lock(request->object_name(), utils::operation_type::READ, complete);
}

void osd_stm::delete_and_wait(
            const osd::delete_request* request,
            osd::delete_reply* response,
            google::protobuf::Closure* done){
    osd_service_complete<osd::delete_reply> *delete_complete =
      new osd_service_complete<osd::delete_reply>(this, request->object_name(), 0,
        response, done);

    auto delete_func = [this, request, delete_complete](){
        osd::delete_cmd cmd;
        cmd.set_object_name(request->object_name());
        if (_image_snap_seq != 0) {
            cmd.set_image_snap_seq(_image_snap_seq);
        }
        std::string buf;
        cmd.SerializeToString(&buf);

        SPDK_INFOLOG(osd, "process delete_request , pool %lu pg %lu object_name %s \n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str());

        auto entry_ptr = std::make_shared<raft_entry_t>();
        entry_ptr->set_type(RAFT_LOGTYPE_DELETE);
        entry_ptr->set_meta(std::move(buf));

        get_raft()->raft_write_entry(entry_ptr, delete_complete);
    };

    lock_complete *complete = new lock_complete(std::move(delete_func));
    _object_rw_lock.lock(request->object_name(), utils::operation_type::DELETE, complete);
}

void osd_stm::image_snap_and_wait(const osd::image_snap_request* request,
                                  osd::image_snap_reply* response,
                                  google::protobuf::Closure* done) {
    if (_aos == nullptr) {
        response->set_state(-ENODEV);
        done->Run();
        return;
    }
    if (!linearization()) {
        response->set_state(err::RAFT_ERR_NOT_LEADER);
        done->Run();
        return;
    }

    auto* reply = response;
    auto* clos = done;
    auto finish = [reply, clos](void* /*a*/, int err) {
        reply->set_state(err);
        clos->Run();
    };

    switch (request->op()) {
    case osd::IMAGE_SNAP_CREATE:
        create_image_snapshot(request->image_snap_seq(), finish, nullptr);
        break;
    case osd::IMAGE_SNAP_CLONE:
        clone_image(request->image_snap_seq(), finish, nullptr);
        break;
    case osd::IMAGE_SNAP_RELEASE:
        release_image_snapshot(request->image_snap_seq(), finish, nullptr);
        break;
    case osd::IMAGE_SNAP_FLATTEN:
        flatten_image(request->image_snap_seq(), request->new_image_seq(),
                      request->raft_write_index() == 0 ? 1
                                                       : request->raft_write_index(),
                      finish, nullptr);
        break;
    default:
        response->set_state(-EINVAL);
        done->Run();
        break;
    }
}

void osd_stm::destroy_objects(object_complete cb_fn, void *arg){
    // Object data lives in AOS; drop PG epoch. Blob object_store is unused.
    if (_aos != nullptr) {
        _aos->delete_pg(
            _pool_id, _pg_id,
            [cb_fn = std::move(cb_fn)](void* a, int err) {
                if (cb_fn) {
                    cb_fn(a, err);
                }
            },
            arg);
        return;
    }
    if (cb_fn) {
        cb_fn(arg, 0);
    }
}

void osd_stm::stop(object_rw_complete cb_fn, void* arg) {
    state_machine::stop();
    if (cb_fn) {
        cb_fn(arg, 0);
    }
}
