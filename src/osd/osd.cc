
#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/env.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "osd.h"

struct spdk_blob_store *g_bs = NULL;
struct spdk_poller *request_poller = NULL;


// // 读写请求
// struct request {
//     spdk_blob_op_complete cb;
//     void *cb_arg;
//     spdk_blob_id blobid;
//     void *payload;
//     uint64_t offset;
//     uint64_t length;
//     bool read;
// };



static int 
process_request(void* arg)
{
     Msg__Request *msg = (Msg__Request *)arg;
     switch (msg->union_case)
     {
        case MSG__REQUEST__UNION_DELETE_PG_REQUEST:
        // handle create pg        
        // first we print the message
            SPDK_NOTICELOG("delete pg\n");
        //    g_osd->create_pg(msg->create_pg);
            break; 
        case MSG__REQUEST__UNION_CREATE_PG_REQUEST:
        // handle delete pg
        // first we print the message
            SPDK_NOTICELOG("create pg\n");
        //    g_osd->delete_pg(msg->delete_pg);
            break;
        default:
            SPDK_NOTICELOG("default\n");
            break;
     }
    SPDK_NOTICELOG("process_request\n");
    return 0;
 };



static void 
startpoller(void *arg1) {
    g_osd = new osd();
    request_poller = SPDK_POLLER_REGISTER(process_request, NULL, 0);
};

int main() {
    sleep(1);
    printf("hello world\n");
    SPDK_NOTICELOG("starting osd...\n");
    int rc;

    rc = spdk_env_init(NULL);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_env_init failed, rc=%d\n", rc);
        return rc;
    }


    rc = spdk_app_start(NULL, startpoller, NULL);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_app_start failed, rc=%d\n", rc);
        return rc;
    }
    return 0;
}
