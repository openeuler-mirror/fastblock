#include "osd_sm.h"
#include "localstore/blob_manager.h"
#include "raft/raft.h"
#include "rpc/osd_msg.pb.h"
#include "utils/utils.h"
#include "utils/err_num.h"
#include <errno.h>

// 每次写8个units，就是4k
#define BLOCK_UNITS 8

osd_sm::osd_sm(std::string base_data_dir)
: state_machine()
, _base_data_dir(base_data_dir) 
#ifdef ENABLE_OBJSTORE
, _store(global_blobstore(), global_io_channel())
#else
, _store(nullptr, nullptr)
#endif
{}

void osd_sm::apply(std::shared_ptr<raft_entry_t> entry, context *complete){
    std::string obj_name = entry->obj_name();
    if(entry->type() == RAFT_LOGTYPE_WRITE){
        osd::write_cmd write;
        write.ParseFromString(entry->meta());
        write_obj(write.object_name(), write.offset(), entry->data(), complete);
    }else if(entry->type() == RAFT_LOGTYPE_DELETE){
        delete_obj(obj_name, complete);
    }else{
        complete->complete(err::E_SUCCESS);
    }
}

struct write_obj_ctx{
    char* buf;
    context *complete;
};

void write_obj_done(void *arg, int objerrno){
    write_obj_ctx * ctx = (write_obj_ctx *)arg;
    ctx->complete->complete(objerrno);
    spdk_free(ctx->buf);
    delete ctx;
}

void osd_sm::write_obj(const std::string& obj_name, uint64_t offset, const std::string& data, context *complete){
#ifdef ENABLE_OBJSTORE
    uint64_t len = align_up<uint64_t>(data.size(), 512 * BLOCK_UNITS);
    char* buf = (char*)spdk_zmalloc(len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA); 
    memcpy(buf, data.c_str(), data.size());
    write_obj_ctx * ctx = new write_obj_ctx{buf, complete};
    _store.write(obj_name, offset, buf, data.size(), write_obj_done, ctx);
#else
    complete->complete(err::E_SUCCESS);
#endif
}

void osd_sm::delete_obj(const std::string& obj_name, context *complete){
    //delete object 
    complete->complete(err::E_SUCCESS);
}