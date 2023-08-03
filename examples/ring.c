/*
    使用spdk blobstore来实现一个all-in-one的localstore
    对外提供异步读写接口，内部使用spdk blobstore来实现
*/

#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/env.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/event.h"

struct spdk_blob_store *g_bs = NULL;
struct spdk_blob *g_blob = NULL;
struct spdk_poller *request_poller = NULL;


// 读写请求
struct request {
    spdk_blob_op_complete cb;
    void *cb_arg;
    spdk_blob_id blobid;
    void *payload;
    uint64_t offset;
    uint64_t length;
    bool read;
};
void write_cb(void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("cb_arg: %s\n", (char *)cb_arg);
    SPDK_NOTICELOG("bserrno: %d\n", bserrno);
}

void read_cb(void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("read_cb\n");
    SPDK_NOTICELOG("bserrno: %d\n", bserrno);
}


// 读写请求队列
struct spdk_ring *request_queue;


static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	
	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
        SPDK_ERRLOG("blob_open failed\n");
		return;
	}
	g_blob = blob;
}

// 从请求队列中取出一个请求并处理
static int
process_request(void *arg)
{
   
    struct request *req;
    int rc;
    rc = spdk_ring_dequeue(request_queue, (void *)&req, 10);
    
    if (rc == 0) {
        return 1;
    }

    // before we write to or read from the blob, we need to open it first
    spdk_blob_op_with_handle_complete cb_fn = NULL;
    void *cb_arg = NULL;
    spdk_blob_id blobid;
    spdk_bs_open_blob(g_bs, blobid, cb_fn, cb_arg);
    sleep(2);
    // open channel before write and read
    struct spdk_io_channel *chan;
    chan = spdk_bs_alloc_io_channel(g_bs);

    if (req->read) {
        spdk_blob_io_read(g_blob, chan, req->payload, req->offset, req->length, req->cb, req->cb_arg);
    } else {
        spdk_blob_io_write(g_blob, chan, req->payload, req->offset, req->length, req->cb, req->cb_arg);
    }

    if (rc != 0) {
        SPDK_ERRLOG("spdk_blob_io_read/write failed, rc=%d\n", rc);
    }

    free(req);
}

static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
     SPDK_NOTICELOG("entry\n");
     if (bserrno) {
         SPDK_ERRLOG("blobstore_init failed\n");
           return;
        }
        g_bs = bs;
        SPDK_NOTICELOG("blobstore_init success\n");
}


// blobstore创建完成的回调函数,打印返回结果
static void
localstore_create_bs_dev_callback(enum spdk_bdev_event_type dev_type, struct spdk_bdev * bdev, void * args)
{
    
}

// 分配一个malloc0的内存设备，用于blobstore的创建
static int
localstore_create_bs_dev(void)
{
    struct spdk_bs_dev *bs_dev;
    int rc;

    bs_dev = calloc(1, sizeof(*bs_dev));
    if (bs_dev == NULL) {
        SPDK_ERRLOG("calloc failed\n");
        return -ENOMEM;
    }

    bs_dev->blocklen = 4096;
    rc = spdk_bdev_create_bs_dev_ext("fakeDisk",localstore_create_bs_dev_callback, NULL, &bs_dev);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_bdev_create_bs_dev failed, rc=%d\n", rc);
        free(bs_dev);
        return rc;
    }
    spdk_bs_init(bs_dev,NULL,bs_init_complete, NULL);
    return 0;
}

static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct hello_context_t *hello_context = arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
        SPDK_ERRLOG("blob_create failed\n");
		return;
	}
}



void create_blob() {
    spdk_blob_op_with_id_complete cb_fn = NULL;
    void *cb_arg = NULL;
    spdk_blob_id blobid;
    int rc;

    spdk_bs_create_blob(g_bs, cb_fn, NULL);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_bs_create_blob failed, rc=%d\n", rc);
        return;
    }

    SPDK_NOTICELOG("create blob success, blobid=%lu\n", blobid);
}



void startpoller() {
    request_poller = SPDK_POLLER_REGISTER(process_request, NULL, 0);
  

    // before we write a blob, we need to create it first
    create_blob();

    request_queue = spdk_ring_create(SPDK_RING_TYPE_SP_SC, 1024, 1);
    struct request *testreq = malloc(sizeof(struct request));
    testreq->read = false;
    testreq->cb = write_cb;
    testreq->cb_arg = "write_haha";
    testreq->blobid = 1;
    testreq->payload = "haha";
    testreq->offset = 0;
    testreq->length = 4;
    spdk_ring_enqueue(request_queue, (void **)&testreq, 1, NULL);
}

int main() {
    int rc;

    rc = spdk_env_init(NULL);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_env_init failed, rc=%d\n", rc);
        return rc;
    }

    rc = localstore_create_bs_dev();
    if (rc != 0) {
        SPDK_ERRLOG("localstore_create_bs_dev failed, rc=%d\n", rc);
        return rc;
    }

    rc = spdk_app_start(NULL, startpoller, NULL);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_app_start failed, rc=%d\n", rc);
        return rc;
    }
    return 0;
}
