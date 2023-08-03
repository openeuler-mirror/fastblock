/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blob.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "localstore/object_store.h"
/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
using object_rw_complete = std::function<void (void *arg, int objerrno)>;


struct hello_context_t {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	uint8_t *read_buff;
	uint8_t *write_buff;
	uint64_t io_unit_size;
	int rc;
	//函数指针
	object_store *object_blob;
	object_rw_complete cb_fn;
  	void*    arg;
	spdk_blob_id t_bid;
};

/*
 * Free up memory that we allocated.
 */
static void
hello_cleanup(struct hello_context_t *hello_context)
{
	spdk_free(hello_context->read_buff);
	spdk_free(hello_context->write_buff);
	free(hello_context);
}

/*
 * Callback routine for the blobstore unload.
 */
static void
unload_complete(void *cb_arg, int bserrno)
{
	struct hello_context_t *hello_context =( hello_context_t *) cb_arg;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		SPDK_ERRLOG("Error %d unloading the bobstore\n", bserrno);
		hello_context->rc = bserrno;
	}

	spdk_app_stop(hello_context->rc);
}

/*
 * Unload the blobstore, cleaning up as needed.
 */
static void
unload_bs(struct hello_context_t *hello_context, const char *msg, int bserrno)
{
	if (bserrno) {
		SPDK_ERRLOG("%s (err %d)\n", msg, bserrno);
		hello_context->rc = bserrno;
	}
	if (hello_context->bs) {
		if (hello_context->channel) {
			spdk_bs_free_io_channel(hello_context->channel);
		}
		spdk_bs_unload(hello_context->bs, unload_complete, hello_context);
	} else {
		spdk_app_stop(bserrno);
	}
}

/*
 * Callback routine for the deletion of a blob.
 */
static void
delete_complete(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context = (hello_context_t *)arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error in delete completion",
			  bserrno);
		return;
	}

	/* We're all done, we can unload the blobstore. */
	unload_bs(hello_context, "", 0);
}

/*
 * Function for deleting a blob.
 */
static void
delete_blob(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context = (hello_context_t *)arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error in close completion",
			  bserrno);
		return;
	}

	spdk_bs_delete_blob(hello_context->bs, hello_context->blobid,
			    delete_complete, hello_context);
}

/*
 * Callback function for reading a blob.
 */
static void
read_complete(void *arg1, int bserrno)
{
	printf("进入读完成函数\n");
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	int match_res = -1;

	SPDK_NOTICELOG("entry\n");

	/* Now let's make sure things match. */
	match_res = memcmp(hello_context->write_buff, hello_context->read_buff,
			   hello_context->io_unit_size);
	if (match_res) {
		// unload_bs(hello_context, "Error in data compare", -1);
		printf("读缓冲区和写缓冲区不匹配\n");
		// return;
	} else {
		SPDK_NOTICELOG("read SUCCESS and data matches!  read和wirte的buff都是 %p   bbid %" PRIu64 "\n"  ,hello_context->read_buff,hello_context->blobid);
	}
	/* Now let's close it and delete the blob in the callback. */
	// spdk_blob_close(hello_context->blob, delete_blob, hello_context);
	//spdk_blob_close(hello_context->blob, delete_blob, hello_context);
	//每次读写之后，执行的回调函数。
	if(hello_context->cb_fn!=nullptr)
	hello_context->cb_fn(arg1, bserrno);

}

/*
 * Function for reading a blob.
 */
static void
read_blob(struct hello_context_t *hello_context,object_rw_complete cb_fn)
{
	SPDK_NOTICELOG("entry\n");

	hello_context->read_buff = (uint8_t *)spdk_malloc(hello_context->io_unit_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	if (hello_context->read_buff == NULL) {
		unload_bs(hello_context, "Error in memory allocation",
			  -ENOMEM);
		return;
	}
	 printf("spdk_blob_io_read\n");
	 hello_context->cb_fn= cb_fn;
	/* Issue the read and compare the results in the callback. */
	spdk_blob_io_read(hello_context->blob, hello_context->channel,
			  hello_context->read_buff, 0, 1, read_complete,
			  hello_context);
}

/*
 * Callback function for writing a blob.
 */
static void
write_complete(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context =(hello_context_t *) arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		// unload_bs(hello_context, "Error in write completion",
		// 	  bserrno);
		printf("错误 %s \n" ,spdk_strerror(bserrno));
		//return;
	}

	/* Now let's read back what we wrote and make sure it matches. */
	read_blob(hello_context,hello_context->cb_fn);
}

/*
 * Function for writing to a blob.
 */
static void
blob_write(struct hello_context_t *hello_context,int context, object_rw_complete cb_fn)
{
	SPDK_NOTICELOG("entry\n");
	/*
	 * Buffers for data transfer need to be allocated via SPDK. We will
	 * transfer 1 io_unit of 4K aligned data at offset 0 in the blob.
	 */
	hello_context->write_buff = (uint8_t*)spdk_malloc(hello_context->io_unit_size,
						0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
						SPDK_MALLOC_DMA);
	if (hello_context->write_buff == NULL) {
		unload_bs(hello_context, "Error in allocating memory",
			  -ENOMEM);
		return;
	}
	memset(hello_context->write_buff, context, hello_context->io_unit_size);
	/* Now we have to allocate a channel. */
	hello_context->channel = spdk_bs_alloc_io_channel(hello_context->bs);
	if (hello_context->channel == NULL) {
		unload_bs(hello_context, "Error in allocating channel",
			  -ENOMEM);
		return;
	}
	hello_context->cb_fn=cb_fn;
	printf("spdk_blob_io_write\n");
	/* Let's perform the write, 1 io_unit at offset 0. */
	spdk_blob_io_write(hello_context->blob, hello_context->channel,
			   hello_context->write_buff,
			   0, 1, write_complete, hello_context);
}

/*
 * Callback function for syncing metadata.
 */
void protest(void *arg1);
static void
sync_complete(void *arg1, int bserrno)
{
	struct hello_context_t *hello_context =(hello_context_t *) arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error in sync callback",
			  bserrno);
		return;
	}

	/* Blob has been created & sized & MD sync'd, let's write to it. */
	//blob_write(hello_context);
    protest(hello_context);
}

static void
resize_complete(void *cb_arg, int bserrno)
{
	struct hello_context_t *hello_context = (hello_context_t *) cb_arg;
	uint64_t total = 0;

	if (bserrno) {
		unload_bs(hello_context, "Error in blob resize", bserrno);
		return;
	}

	total = spdk_blob_get_num_clusters(hello_context->blob);
	SPDK_NOTICELOG("resized blob now has USED clusters of %" PRIu64 "\n",
		       total);

	/*
	 * Metadata is stored in volatile memory for performance
	 * reasons and therefore needs to be synchronized with
	 * non-volatile storage to make it persistent. This can be
	 * done manually, as shown here, or if not it will be done
	 * automatically when the blob is closed. It is always a
	 * good idea to sync after making metadata changes unless
	 * it has an unacceptable impact on application performance.
	 */
	spdk_blob_sync_md(hello_context->blob, sync_complete, hello_context);
}

/*
 * Callback function for opening a blob.
 */
static void
open_complete(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	struct hello_context_t *hello_context = (hello_context_t *)cb_arg;
	uint64_t free = 0;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error in open completion",
			  bserrno);
		return;
	}


	hello_context->blob = blob;
	free = spdk_bs_free_cluster_count(hello_context->bs);
	SPDK_NOTICELOG("blobstore has FREE clusters of %" PRIu64 "\n",
		       free);

	/*
	 * Before we can use our new blob, we have to resize it
	 * as the initial size is 0. For this example we'll use the
	 * full size of the blobstore but it would be expected that
	 * there'd usually be many blobs of various sizes. The resize
	 * unit is a cluster.
	 */
	spdk_blob_resize(hello_context->blob, 10, resize_complete, hello_context); //free-10
}

/*
 * Callback function for creating a blob.
 */
static void
blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno)
{
	struct hello_context_t *hello_context =(hello_context_t *) arg1;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error in blob create callback",
			  bserrno);
		return;
	}

	hello_context->blobid = blobid;
	SPDK_NOTICELOG("new blob id %" PRIu64 "\n", hello_context->blobid);

	/* We have to open the blob before we can do things like resize. */
	spdk_bs_open_blob(hello_context->bs, hello_context->blobid,
			  open_complete, hello_context);
}

/*
 * Function for creating a blob.
 */
static void
create_blob(struct hello_context_t *hello_context)
{
	SPDK_NOTICELOG("entry\n");
    //这里创建了blob，回调函数主要目的为保存blobid号。
	spdk_bs_create_blob(hello_context->bs, blob_create_complete, hello_context);
}

/*
 * Callback function for initializing the blobstore.
 */
static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{
	struct hello_context_t *hello_context =(hello_context_t *) cb_arg;

	SPDK_NOTICELOG("entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error initing the blobstore",
			  bserrno);
		return;
	}

	hello_context->bs = bs;
	SPDK_NOTICELOG("blobstore: %p\n", hello_context->bs);
	/*
	 * We will use the io_unit size in allocating buffers, etc., later
	 * so we'll just save it in out context buffer here.
	 */
	hello_context->io_unit_size = spdk_bs_get_io_unit_size(hello_context->bs);

	/*
	 * The blobstore has been initialized, let's create a blob.
	 * Note that we could pass a message back to ourselves using
	 * spdk_thread_send_msg() if we wanted to keep our processing
	 * time limited.
	 */
	create_blob(hello_context);
}


static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	SPDK_WARNLOG("Unsupported bdev event: type %d\n", type);
	return ;
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1)
{
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	struct spdk_bs_dev *bs_dev = NULL;
	int rc;

	SPDK_NOTICELOG("entry\n");

	/*
	 * In this example, use our malloc (RAM) disk configured via
	 * hello_blob.json that was passed in when we started the
	 * SPDK app framework.
	 *
	 * spdk_bs_init() requires us to fill out the structure
	 * spdk_bs_dev with a set of callbacks. These callbacks
	 * implement read, write, and other operations on the
	 * underlying disks. As a convenience, a utility function
	 * is provided that creates an spdk_bs_dev that implements
	 * all of the callbacks by forwarding the I/O to the
	 * SPDK bdev layer. Other helper functions are also
	 * available in the blob lib in blob_bdev.c that simply
	 * make it easier to layer blobstore on top of a bdev.
	 * However blobstore can be more tightly integrated into
	 * any lower layer, such as NVMe for example.
	 */
	rc = spdk_bdev_create_bs_dev_ext("Malloc0", base_bdev_event_cb, NULL, &bs_dev);
	if (rc != 0) {
		SPDK_ERRLOG("Could not create blob bdev, %s!!\n",
			    spdk_strerror(-rc));
		return;
	}
	spdk_bs_init(bs_dev, NULL, bs_init_complete, hello_context);
}
void load_snap(void *arg1,int objerrno);
void re_write(void *arg1,int objerrno); 
void re_write_snap(void *arg1, struct spdk_blob *blb,int objerrno) ;
void del_snap(void *arg1,int objerrno);
void result(void *arg1,int objerrno);
void find_b(void *arg1,int objerrno);
void close_snap(void *arg1,int objerrno);
void re_write2(void *arg1,int objerrno) ;
void load_snap(void *arg1,int objerrno);
void re_read(void *arg1,int objerrno);

void create_snap(void *arg1,int objerrno);
void protest(void *arg1) {
    struct hello_context_t *hello_context = (hello_context_t *)arg1;
    // //首先创建创建对象
		hello_context->object_blob->bs=hello_context->bs;
    	//blob_store的table进行插入
		printf("原本的内容 %" PRIu64 "\n   %p",hello_context->blobid,hello_context->write_buff );
        blob_write(hello_context,0x21,create_snap);
		hello_context->t_bid=hello_context->blobid;	
}

//创建快照
void create_snap(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	SPDK_NOTICELOG("第一次写入完成，准备创建快照  原内容 %p  \n",hello_context->read_buff);
	std::string name ="word_blob";
	struct object_store::fb_blob* fb = new object_store::fb_blob ();
	fb->blob =  hello_context->blob;
	fb->blobid = hello_context->blobid;
	hello_context->object_blob->table.insert({name,fb});
	//执行快照后回调
	hello_context->object_blob->snap_look(name,re_write,hello_context);  
}

//覆写 
void re_write(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	re_read(hello_context,0);
}

//读取创建快照的内容
void re_read(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	hello_context->cb_fn=load_snap;
	read_blob(hello_context,re_write2);
}

//载入快照前，对原blob进行写入
void re_write2(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	hello_context->cb_fn=load_snap;
	printf("原blob进行覆写 的 bbid %" PRIu64 " %p  \n",hello_context->blobid,hello_context->read_buff);
	//对原数据读写
	blob_write(hello_context,0x31,load_snap);
}

//进行快照载入
void load_snap(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	int sn_sz1 = hello_context->object_blob->table.find("word_blob")->second->sp_list.size();
	SPDK_NOTICELOG("准备载入快照  快照链长度 %d \n",sn_sz1);
	hello_context->object_blob->load_snap("word_blob", 1,close_snap,hello_context);
}

//关闭快照blob
void close_snap(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	hello_context->blob=hello_context->object_blob->table.find("word_blob")->second->blob;
	hello_context->blobid=hello_context->object_blob->table.find("word_blob")->second->blobid;
	hello_context->cb_fn=nullptr;
	hello_context->cb_fn=del_snap;
	int sn_sz2 = hello_context->object_blob->table.find("word_blob")->second->sp_list.size();
	printf("载入快照前内容 %p  快照链长度 %d \n " ,hello_context->read_buff,sn_sz2);
	del_snap(hello_context,0);
}

//删除快照
void del_snap(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	//输出快照内容
	int sn_sz1 = hello_context->object_blob->table.find("word_blob")->second->sp_list.size();
	printf("快照内容 %p,快照链长度 %ld \n" ,hello_context->read_buff,sn_sz1);
	//首先获取快照链的长度。
	int sn_sz = hello_context->object_blob->table.find("word_blob")->second->sp_list.size();
	printf("删除快照前长度 %ld \n" ,sn_sz);
	//删除快照
	hello_context->object_blob->delete_snap("word_blob",1,find_b,hello_context);
	hello_context->blob=hello_context->object_blob->table.find("word_blob")->second->blob;
	hello_context->blobid=hello_context->object_blob->table.find("word_blob")->second->blobid;
}

//先找到原来的blob，通过bid
void find_b(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	hello_context->cb_fn=load_snap;
	printf("进行覆写 的 bbid %" PRIu64 "   \n",hello_context->blobid);
	//打开
	spdk_bs_open_blob(hello_context->object_blob->bs,hello_context->t_bid ,re_write_snap,hello_context);
}

//删除快照后，原始blob能否写入。
void re_write_snap(void *arg1, struct spdk_blob *blb,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	hello_context->cb_fn=load_snap;
	hello_context->blob=blb;
	blob_write(hello_context,0x12,result);
}

//完成
void result(void *arg1,int objerrno) {
	struct hello_context_t *hello_context = (hello_context_t *)arg1;
	int sn_sz = hello_context->object_blob->table.find("word_blob")->second->sp_list.size();
	printf("snap list of size after delted %ld\n" ,sn_sz);
	spdk_app_stop(0);
}


int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context_t *hello_context = NULL;

	SPDK_NOTICELOG("entry\n");

	/* Set default values in opts structure. */
	spdk_app_opts_init(&opts, sizeof(opts));

	/*
	 * Setup a few specifics before we init, for most SPDK cmd line
	 * apps, the config file will be passed in as an arg but to make
	 * this example super simple we just hardcode it. We also need to
	 * specify a name for the app.
	 */
	opts.name = "hello_blob";
	opts.json_config_file = argv[1];


	/*
	 * Now we'll allocate and initialize the blobstore itself. We
	 * can pass in an spdk_bs_opts if we want something other than
	 * the defaults (cluster size, etc), but here we'll just take the
	 * defaults.  We'll also pass in a struct that we'll use for
	 * callbacks so we've got efficient bookkeeping of what we're
	 * creating. This is an async operation and bs_init_complete()
	 * will be called when it is complete.
	 */
	hello_context = (hello_context_t *)calloc(1, sizeof(struct hello_context_t));
	if (hello_context != NULL) {
		/*
		 * spdk_app_start() will block running hello_start() until
		 * spdk_app_stop() is called by someone (not simply when
		 * hello_start() returns), or if an error occurs during
		 * spdk_app_start() before hello_start() runs.
		 */

		object_store *temp = new object_store (hello_context->bs,hello_context->channel);
		hello_context->object_blob =temp;

		rc = spdk_app_start(&opts, hello_start, hello_context);
		if (rc) {
			SPDK_NOTICELOG("ERROR!\n");
		} else {
			SPDK_NOTICELOG("SUCCESS!\n");
		}
		/* Free up memory that we allocated */
        //释放读写缓冲区和结构题
		SPDK_NOTICELOG("释放\n");
		hello_cleanup(hello_context);
	} else {
		SPDK_ERRLOG("Could not alloc hello_context struct!!\n");
		rc = -ENOMEM;
	}

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}