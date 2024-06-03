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

#include "localstore/object_name.h"
#include "osd_stm.h"
#include "data_statistics.h"
#include "raft/raft.h"
#include "rpc/osd_msg.pb.h"
#include "utils/utils.h"
#include "utils/err_num.h"
#include <errno.h>
#include <concepts>

// 每次写8个units，就是4k
#define BLOCK_UNITS 8

osd_stm::osd_stm()
: state_machine()
, _object_rw_lock(){
    uint32_t lcore = spdk_env_get_current_core();
    _sockid = spdk_env_get_socket_id(lcore);
}

void osd_stm::apply(std::shared_ptr<raft_entry_t> entry, utils::context *complete){
    if(entry->type() == RAFT_LOGTYPE_WRITE){
        osd::write_cmd write;
        write.ParseFromString(entry->meta());
        write_obj(write.mutable_object_name(), write.offset(), entry->mutable_data(), complete);
    }else if(entry->type() == RAFT_LOGTYPE_DELETE){
        osd::delete_cmd del;
        del.ParseFromString(entry->meta());
        delete_obj(del.object_name(), complete);
    }else{
        complete->complete(err::E_SUCCESS);
    }
}

struct write_obj_ctx{
    osd_stm* stm{nullptr};
    std::string* obj_name{};
    char* buf{nullptr};
    utils::context *complete{nullptr};
    std::optional<std::function<void()>> snap_create_cb{std::nullopt};
};

void write_obj_done(void *arg, int obj_errno) {
    write_obj_ctx * ctx = (write_obj_ctx *)arg;
    ctx->stm->unlock(*(ctx->obj_name), operation_type::WRITE);
    if (ctx->snap_create_cb) {
        try {
            (ctx->snap_create_cb.value())();
        } catch (const std::exception& e) {
            SPDK_ERRLOG(
              "crate snapshot of object %s error on writing object done, %s\n",
              ctx->obj_name->c_str(), e.what());
        }
    }

    ctx->complete->complete(obj_errno);
    spdk_free(ctx->buf);
    delete ctx;
}

void osd_stm::write_obj_directly(
  std::string* obj_name,
  uint64_t offset,
  std::string* data,
  utils::context* complete,
  std::optional<std::function<void()>> snap_create_cb = std::nullopt) {
    uint64_t len = utils::align_up<uint64_t>(data->size(), 512 * BLOCK_UNITS);
    char* buf = (char*)spdk_zmalloc(len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    memcpy(buf, data->c_str(), data->size());
    write_obj_ctx * ctx = new write_obj_ctx{this, obj_name, buf, complete, std::move(snap_create_cb)};
    std::map<std::string, xattr_val_type> xattr;
    xattr["type"] = blob_type::object;
    xattr["pg"] = get_pg_name();

    SPDK_INFOLOG_EX(
      osd,
      "write obj %s xattr type: %u pg: %s\n",
      obj_name->c_str(),
      static_cast<std::underlying_type_t<blob_type>>(blob_type::object),
      get_pg_name().c_str());
    _store.write(xattr, *obj_name, offset, buf, data->size(), write_obj_done, ctx);
}

static void on_snapshot_created(void* arg, int objerrno) {}

void osd_stm::write_obj(std::string* obj_name, uint64_t offset, std::string* data, utils::context* complete) {
    auto latest_snap_epoch = _store.snapshot_latest_epoch(*obj_name);
    decltype(latest_snap_epoch)::value_type latest_epoch_val{};
    if (not latest_snap_epoch) {
        SPDK_INFOLOG(
          osd,
          "cant find the latest epoch of %s's snapshot, will fetch from monitor\n",
          obj_name->c_str());
        latest_epoch_val = -1;
    } else {
        latest_epoch_val = *latest_snap_epoch;
    }

    auto result = localstore::object_name::get_image_pool_name(*obj_name);
    if (not result) {
        SPDK_ERRLOG("cant get image name and pool id from object name %s\n", obj_name);
        throw std::runtime_error{FMT_1("cant get image name and pool id from object name %1%", *obj_name)};
    }
    auto& [image_name, pool_id] = *result;
    SPDK_DEBUGLOG(
      osd,
      "obj_name is %s, image_name is %s, pool_id is %ld\n",
      obj_name->c_str(), image_name.c_str(), pool_id);

    auto mon = _raft->get_monitor();
    SPDK_DEBUGLOG(
      osd,
      "start sending list snapshots request of image %s, pool id %ld\n",
      image_name.c_str(), pool_id);

    mon->emplace_list_snapshot_request(
      pool_id, image_name, latest_epoch_val, -1,
      [this, obj_name, pool_id, latest_epoch_val, data, offset, complete]
      (const monitor::client::response_status s, monitor::client::request_context* mon_ctx) mutable {
          if (s != monitor::client::response_status::ok) {
              throw std::runtime_error{FMT_1("list snapshots of %1% error", *obj_name)};
          }

          auto& snaps = std::get<std::unique_ptr<std::list<monitor::client::snapshot_info>>>(mon_ctx->response_data);
          SPDK_DEBUGLOG(
            osd,
            "list snapshots of object %s done, size is %ld\n",
            obj_name->c_str(), snaps->size());

          auto xattr = std::make_shared<std::map<std::string, xattr_val_type>>();
          xattr->emplace("type", blob_type::object_snap);
          xattr->emplace("pg", get_pg_name());
          auto create_snap_actor = [this, xattr, obj_name, snaps = std::move(snaps)] () {
              if (snaps->empty()) {
                  return;
              }

              auto it = std::find_if(
                snaps->begin(),
                snaps->end(),
                [] (const decltype(snaps)::element_type::value_type& v) {
                    return v.status == msg::SnapshotInfoStatus::snapshotCreated;
                }
              );

              if (it == snaps->end()) {
                  SPDK_INFOLOG(osd, "all latest snapshots of object %s has been deleted\n", obj_name->c_str());
                  return;
              }

              _store.snap_create(
                *xattr, obj_name, &(it->name), it->epoch,
                [obj_name, it, &snaps] (void* arg, int objerrno) mutable {
                    if (objerrno != 0) {
                        SPDK_ERRLOG(
                          "create snapshot %s of object %s error, %s",
                          obj_name->c_str(), it->name.c_str(),
                          ::spdk_strerror(objerrno));
                        throw std::runtime_error{FMT_3(
                          "create snapshot %1% of object %2% error, %3%",
                          *obj_name, it->name, ::spdk_strerror(objerrno))};
                    }
                },
                nullptr);
          };

          if (_store.table.contains(*obj_name)) {
              write_obj_directly(obj_name, offset, data, complete, std::move(create_snap_actor));
          } else {
              create_snap_actor();
          }
      }
    );
}

void osd_stm::delete_obj(const std::string& obj_name, utils::context *complete){
    //delete object

    _object_rw_lock.unlock(obj_name, utils::operation_type::DELETE);
    complete->complete(err::E_SUCCESS);
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
        std::string buf;
        cmd.SerializeToString(&buf);

        SPDK_INFOLOG(osd, "process write_request , pg %lu.%lu object_name %s offset %lu len %lu\n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str(), request->offset(),
                     request->data().size());

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
    char* buf;
    utils::context *complete;
    osd::read_reply* response;
    uint64_t size;
};

void read_obj_done(void *arg, int obj_errno){
    read_obj_ctx * ctx = (read_obj_ctx *)arg;
    if(obj_errno == 0){
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
        //Whether to need to wait until first commit applied in the new term？ todo

        if(!linearization()){
            SPDK_INFOLOG(osd, "!linearization\n");
            read_complete->complete(err::RAFT_ERR_NOT_LEADER);
            return;
        }

        SPDK_INFOLOG(osd, "process read_request , pool %lu pg %lu object_name %s offset %lu len %lu\n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str(), request->offset(),
                     request->length());

        uint64_t len = utils::align_up<uint64_t>(request->length(), 512 * BLOCK_UNITS);
        char* buf = (char*)spdk_zmalloc(len, 0x1000, NULL, _sockid, SPDK_MALLOC_DMA);
        read_obj_ctx * ctx = new read_obj_ctx{buf, read_complete, response, request->length()};

        std::map<std::string, xattr_val_type> xattr;
        xattr["type"] = blob_type::object;
        xattr["pg"] = get_pg_name();
        _store.read(xattr, request->object_name(), request->offset(), buf, request->length(), read_obj_done, ctx);
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

void osd_stm::destroy_objects(object_complete cb_fn, void *arg){
    uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
    auto destroy_done = [cb_fn = std::move(cb_fn), shard_id](void *arg, int objerrno){
        core_sharded::get_core_sharded().invoke_on(
          shard_id,
          [cb_fn = std::move(cb_fn), arg, objerrno](){
            cb_fn(arg, objerrno);
          });
    };

    core_sharded::get_core_sharded().invoke_on(
      utils::default_blobstore_core,
      [this, destroy_done = std::move(destroy_done), arg](){
        _store.destroy(std::move(destroy_done), arg);
      });
}
