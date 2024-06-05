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
, _object_rw_lock()
{}

void osd_stm::apply(std::shared_ptr<raft_entry_t> entry, utils::context *complete){
    if(entry->type() == RAFT_LOGTYPE_WRITE){
        osd::write_cmd write;
        write.ParseFromString(entry->meta());
        write_obj(write.object_name(), write.offset(), entry->data(), complete);
    }else if(entry->type() == RAFT_LOGTYPE_DELETE){
        osd::delete_cmd del;
        del.ParseFromString(entry->meta());
        delete_obj(del.object_name(), complete);
    }else{
        complete->complete(err::E_SUCCESS);
    }
}

struct write_obj_ctx{
    osd_stm* stm;
    std::string obj_name;
    char* buf;
    utils::context *complete;
};

void write_obj_done(void *arg, int obj_errno){
    write_obj_ctx * ctx = (write_obj_ctx *)arg;
    ctx->stm->unlock(ctx->obj_name, operation_type::WRITE);
    ctx->complete->complete(obj_errno);
    spdk_free(ctx->buf);
    delete ctx;
}

void osd_stm::write_obj(const std::string& obj_name, uint64_t offset, const std::string& data, utils::context *complete){
    uint64_t len = utils::align_up<uint64_t>(data.size(), 512 * BLOCK_UNITS);
    char* buf = (char*)spdk_zmalloc(len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    memcpy(buf, data.c_str(), data.size());
    write_obj_ctx * ctx = new write_obj_ctx{this, obj_name, buf, complete};
    std::map<std::string, xattr_val_type> xattr;
    xattr["type"] = blob_type::object;
    xattr["pg"] = get_pg_name();
    SPDK_INFOLOG_EX(osd, "write obj %s xattr type: %u pg: %s\n", obj_name.c_str(), (uint32_t)blob_type::object, get_pg_name().c_str());
    _store.write(xattr, obj_name, offset, buf, data.size(), write_obj_done, ctx);
}

void osd_stm::delete_obj(const std::string& obj_name, utils::context *complete){
    //delete object

    _object_rw_lock.unlock(obj_name, operation_type::DELETE);
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

    osd_service_complete(osd_stm* _stm, std::string _obj_name, rsp_type* _response, google::protobuf::Closure* _done)
    : response(_response)
    , done(_done)
    , stm(_stm)
    , obj_name(std::move(_obj_name)) {}

    void finish(int r) override {
        SPDK_DEBUGLOG_EX(osd, "process osd service done.\n");
        if(r != 0){
            SPDK_ERRLOG_EX("process osd service failed: %d\n", r);
            if(std::is_same_v<type, osd::write_reply>){
                stm->unlock(obj_name, operation_type::WRITE);
            }else if(std::is_same_v<type, osd::delete_reply>){
                stm->unlock(obj_name, operation_type::DELETE);
            }
        }
        if(std::is_same_v<type, osd::read_reply>){
            stm->unlock(obj_name, operation_type::READ);
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
                    new osd_service_complete<osd::write_reply>(this, request->object_name(), response, done);

    auto write_func = [this, request, write_complete](){
        osd::write_cmd cmd;
        cmd.set_object_name(request->object_name());
        cmd.set_offset(request->offset());
        std::string buf;
        cmd.SerializeToString(&buf);

        SPDK_INFOLOG_EX(osd, "process write_request , pool %lu pg %lu object_name %s offset %lu len %lu\n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str(), request->offset(),
                     request->data().size());

        auto entry_ptr = std::make_shared<raft_entry_t>();
        entry_ptr->set_type(RAFT_LOGTYPE_WRITE);
        entry_ptr->set_meta(std::move(buf));
        entry_ptr->set_data(std::move(request->data()));

        get_raft()->raft_write_entry(entry_ptr, write_complete);
    };

    lock_complete *complete = new lock_complete(std::move(write_func));
    _object_rw_lock.lock(request->object_name(), operation_type::WRITE, complete);
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
                    new osd_service_complete<osd::read_reply>(this, request->object_name(), response, done);

    auto read_func = [this, request, response, read_complete](){
        //Whether to need to wait until first commit applied in the new term？ todo

        if(!linearization()){
            SPDK_INFOLOG_EX(osd, "!linearization\n");
            read_complete->complete(err::RAFT_ERR_NOT_LEADER);
            return;
        }

        SPDK_INFOLOG_EX(osd, "process read_request , pool %lu pg %lu object_name %s offset %lu len %lu\n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str(), request->offset(),
                     request->length());

        uint64_t len = utils::align_up<uint64_t>(request->length(), 512 * BLOCK_UNITS);
        char* buf = (char*)spdk_zmalloc(len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
        read_obj_ctx * ctx = new read_obj_ctx{buf, read_complete, response, request->length()};

        std::map<std::string, xattr_val_type> xattr;
        xattr["type"] = blob_type::object;
        xattr["pg"] = get_pg_name();
        _store.read(xattr, request->object_name(), request->offset(), buf, request->length(), read_obj_done, ctx);
    };

    lock_complete *complete = new lock_complete(std::move(read_func));
    _object_rw_lock.lock(request->object_name(), operation_type::READ, complete);
}

void osd_stm::delete_and_wait(
            const osd::delete_request* request,
            osd::delete_reply* response,
            google::protobuf::Closure* done){
    osd_service_complete<osd::delete_reply> *delete_complete =
                    new osd_service_complete<osd::delete_reply>(this, request->object_name(), response, done);

    auto delete_func = [this, request, delete_complete](){
        osd::delete_cmd cmd;
        cmd.set_object_name(request->object_name());
        std::string buf;
        cmd.SerializeToString(&buf);

        SPDK_INFOLOG_EX(osd, "process delete_request , pool %lu pg %lu object_name %s \n",
                     request->pool_id(), request->pg_id(), request->object_name().c_str());

        auto entry_ptr = std::make_shared<raft_entry_t>();
        entry_ptr->set_type(RAFT_LOGTYPE_DELETE);
        entry_ptr->set_meta(std::move(buf));

        get_raft()->raft_write_entry(entry_ptr, delete_complete);
    };

    lock_complete *complete = new lock_complete(std::move(delete_func));
    _object_rw_lock.lock(request->object_name(), operation_type::DELETE, complete);
}

void osd_stm::destroy_objects(object_complete cb_fn, void *arg){
    _store.destroy(std::move(cb_fn), arg);
}
