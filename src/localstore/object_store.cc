/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2. 
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2 
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, 
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, 
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.  
 * See the Mulan PSL v2 for more details.  
 */

#include <spdk/log.h>

#include "object_store.h"
#include <functional>

SPDK_LOG_REGISTER_COMPONENT(object_store)

struct blob_rw_ctx {
  bool is_read;
  struct spdk_blob *blob;
  struct spdk_io_channel *channel;

  // 读写参数列表
  std::string object_name;
  uint64_t offset;
  char*    buf;
  uint64_t len;
  object_rw_complete cb_fn;
  void*    arg;

  // 非对齐时，会用到这些变量
  bool     is_aligned;
  uint64_t start_lba;
  uint64_t num_lba;
  char*    pin_buf;
  uint32_t blocklen;
};

struct blob_create_ctx {
  bool is_read;
  struct object_store *mgr;

  // 读写参数列表
  std::string object_name;
  uint64_t offset;
  char*    buf;
  uint64_t len;
  object_rw_complete cb_fn;
  void*    arg;

  // create之后，然后调用open时使用
  spdk_blob_id blobid;
};

struct blob_close_ctx {
  struct object_store *mgr;
  // 用来保存 close 到哪一个元素了
  object_store::iterator it;

  object_rw_complete cb_fn;
  void*    arg;
};

//快照的回调函数的参数

struct snap_create_ctx {
  //原始数据块序号，可以知道是哪个快照失败了
  object_store::fb_blob *pre_blob;
  //快照数据块指针
  object_store:: fb_blob *snap_blob;
  //快照的bbid
  //block的table的key
  std::string object_name;

  spdk_blob_store* bs;
  object_store::container * hashlist;
  //回调函数
  object_rw_complete cb_fn;
  void*  arg;
};

//读取快照时函数的上下文状态
struct snap_load_ctx {
  //保存spdk_blob的指针
  spdk_blob *blob;
  //保存快照的bid
  spdk_blob_id blob_id;
  //保存的hashlist
  object_store::container * hashlist;
  //table快照的迭代器
  std::list< object_store::snap_Node*>::iterator it;
  std::string object_name;
  object_store::iterator itb;
  object_rw_complete cb_fn;
  void*  arg;
};

//读取快照时函数的上下文状态
struct snap_delete_ctx {
  //保存spdk_blob的指针
  spdk_blob *blob;
  //保存快照的bid
  spdk_blob_id blob_id;
  //保存的hashlist
  object_store::container * hashlist;
  std::list< object_store::snap_Node*>::iterator it;
  object_store::iterator itb;
  std::string object_name;
  object_rw_complete cb_fn;
  void*  arg;
};



void object_store::write(std::string object_name, 
                      uint64_t offset, char* buf, uint64_t len, 
                      object_rw_complete cb_fn, void* arg)
{
    readwrite(object_name, offset, buf, len, cb_fn, arg, 0);
}

void object_store::read(std::string object_name, 
                     uint64_t offset, char* buf, uint64_t len, 
                     object_rw_complete cb_fn, void* arg) 
{
    readwrite(object_name, offset, buf, len, cb_fn, arg, 1);
}
  void object_store::snap_look(std::string object_name, object_rw_complete cb_fn, void* arg)
{
  
  snap_create(object_name, cb_fn, arg);
}

//生成快照,此函数功能是将现有manage管理的块都通过调用spdk进行快照存储。
void object_store::snap_create(std::string object_name, object_rw_complete cb_fn, void* arg)
{
  //首先在table中找到原始数据块
  auto it = table.find(object_name);
  if (it!=table.end()) {
    //找到块并提取出对应块
    //利用spdk的opt对快照扩展参数进行初始化
    struct spdk_blob_opts opts;
    //用于标识快照函数的上下文信息。
    struct snap_create_ctx* ctx = new snap_create_ctx();
    spdk_blob_opts_init(&opts, sizeof(opts));
    //初始化上下文信息，这里目前不确定哪些信息有效，先空着，目前只有版本信息。
    //ctx.snap_version = xxxxxx;
    ctx->pre_blob = it->second;
    ctx->object_name = object_name;
    ctx->bs = bs;
    ctx->hashlist=&table;
    ctx->snap_blob=new object_store:: fb_blob ();
    ctx->cb_fn =cb_fn;
    ctx->arg=arg;
    //调用spdk的spdk_bs_create_snapshot函数，创建快照，并通过snap_done返回创建的结果。
    // const struct spdk_blob_xattr_opts te = &(opts.xattrs); 
    spdk_bs_create_snapshot(bs, it->second->blobid,&(opts.xattrs),snap_done, ctx);
  }else {
    SPDK_ERRLOG("the fb_blob name is not exist :%s\n",object_name.c_str());
}
}

//snap_done 用于判断上面函数的回调，根据错误码进行判断，spdk是否快照完成。
void object_store::snap_done(void *arg, spdk_blob_id snap_id, int objerrno)
{
  //首先根据错误码，错误的话写入到日志中。
  struct snap_create_ctx* ctx = (struct snap_create_ctx*)arg;
  if (objerrno) 
  {
    //非零情况下，调用的spdk创建快照失败，日志记录。
    SPDK_ERRLOG("blobid:%lu failed:%s\n", ctx->pre_blob->blobid, spdk_strerror(objerrno));
    return ;
  }
  ctx->snap_blob->blobid = snap_id;
  //想要通过blobid找到blob块，但是没到直接的函数，这里通过open打开，保存在snap-ctx中。
  spdk_bs_open_blob(ctx->bs, snap_id,snap_add,ctx);
  SPDK_DEBUGLOG(object_store, "object %s of snap is created successfully\n",ctx->object_name.c_str());
}

//快照链添加函数
void object_store::snap_add(void *arg, spdk_blob* blob, int objerrno)
{
  struct snap_create_ctx* ctx = (struct snap_create_ctx*)arg;
  if (objerrno) 
  {
    //非零情况下，调用的spdk创建快照失败，日志记录。
    SPDK_ERRLOG("snapshot of the name is  :%s failed:%s\n", ctx->object_name.c_str(), spdk_strerror(objerrno));
    return ;
  }
  //构建快照list的结点
  ctx->snap_blob->blob = blob;
  struct snap_Node* snap_node = new snap_Node ();
  snap_node ->snap_fb = ctx->snap_blob;
  //插入到对应的objectname的packet的链表中
  auto temp = ctx->hashlist->find(ctx->object_name)->second;
  //进行头插
  temp->sp_list.push_front(snap_node);
  SPDK_DEBUGLOG(object_store, "the snapshot is add to the list of snaplist, object %s\n",ctx->object_name.c_str());
  //关闭快照
  spdk_blob_close(ctx->snap_blob->blob, close_snap, ctx);
}


//接口，通过object_name获取这个块的快照数量。
int object_store::get_snapsize(std::string object_name) 
{
  return table.find(object_name)->second->sp_list.size();
}

//读取快照接口,这里读取对应objec_name的第几个快照。头部(0)快照为最新。
void object_store::load_snap(std::string object_name, int version, object_rw_complete cb_fn, void* arg){
  //根据快照名和version进行寻找到快照的块
  struct snap_load_ctx* ctx = new snap_load_ctx();
  auto it = table.find(object_name)->second->sp_list.begin();
  //首先判断version是否存在，如果不存在，要报错。
  if (version >get_snapsize(object_name))
  {
    SPDK_ERRLOG("the version of snap is not exits , version should be <= %d",get_snapsize(object_name));
  }
  //替换table中的原始数据的指针为快照指针，这里不对原始数据的block块进行记录，关闭块ref-1。如果想要恢复必须在载入相应的快照时，对载入之前的状态进行快照存储。
  auto itb = table.find(object_name);
  for(int i =1 ; i <version; ++i){
    ++it;
  }

  //保存指向快照的结点。
  ctx->it=it;
  ctx->object_name=object_name;
  ctx->itb=itb;
  ctx->cb_fn=cb_fn;
  ctx->arg=arg;
  spdk_blob_close(itb->second->blob,snap_add_statues,ctx);
  SPDK_DEBUGLOG(object_store, "snap : %s is load successful\n",object_name.c_str());
}
//读取快照的函数，主要替换是否成功
void object_store::snap_add_statues(void *arg, int objerrno) {
  struct snap_load_ctx* ctx = (struct snap_load_ctx*)arg;
    if (objerrno) 
  {
    //非零情况下，调用的spdk创建快照失败，日志记录。
    SPDK_ERRLOG("snapshot install of raw data is failed:%s\n", spdk_strerror(objerrno));
    return ;
  }
  //ctx->hashlist->find(ctx->object_name)->second->sp_list.erase(ctx->it);
  ctx->itb->second->blob = (*ctx->it) ->snap_fb->blob;
  ctx->itb->second->blobid = (*ctx->it) ->snap_fb->blobid;
  SPDK_DEBUGLOG(object_store, "snapshot install of raw data is successful\n");
  //关闭快照
  //spdk_blob_close(ctx->itb->second->blob, close_snap, ctx);
  ctx->cb_fn(ctx->arg, objerrno);
  delete ctx;
}

 void object_store::close_snap (void *arg, int objerrno) 
  {
    struct snap_create_ctx* ctx = (struct snap_create_ctx*)arg;
    if (objerrno) 
  {
    //非零情况下，调用的spdk创建快照失败，日志记录。
    SPDK_ERRLOG("snapshot close is failed:%s\n", spdk_strerror(objerrno));
    return ;
  }
    SPDK_DEBUGLOG(object_store, "snapshot close is successful\n");
    if(ctx->snap_blob->blob==nullptr) {
      SPDK_NOTICELOG("回调blob 为零 \n");
    }
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
  }

//删除快照函数
void object_store::delete_snap(std::string object_name,int version,object_rw_complete cb_fn, void* arg)
{
  struct snap_delete_ctx* ctx = new snap_delete_ctx();
  //删除快照时，仅仅删除快照链中的结点，而不做数据的替换。
    if (version >get_snapsize(object_name))
  {
    
    SPDK_ERRLOG("the version of snap is not exits , version should be <= %d",get_snapsize(object_name));
  }

  auto it = table.find(object_name)->second->sp_list.begin();
  auto itb = table.find(object_name);
  for(int i = 1; i<version; ++i){
    ++it;
  }
  //保存快照结点迭代器
  ctx->it=it;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->itb=itb;
  ctx->object_name=object_name;
  ctx->hashlist=&table;
  //然后通过函数，删除快照保存的块
  spdk_bs_delete_blob(bs,(*it)->snap_fb->blobid, del_done, ctx);
  SPDK_DEBUGLOG(object_store, "snapshot listnode delete is successful\n");
}

//快照删除的回调函数
void object_store::del_done(void *arg, int objerrno) 
{ 
  struct snap_delete_ctx* ctx = (struct snap_delete_ctx*)arg;
  if (objerrno) 
  {
    //非零情况下，调用的spdk创建快照失败，日志记录。
    SPDK_ERRLOG("snapshot delete is failed:%s\n", spdk_strerror(objerrno));
    return ;
  }
  SPDK_DEBUGLOG(object_store, "snapshot delete is successful\n");
  //然后应该自动加载最新的快照，如果没有则不添加
  if(ctx->itb->second->sp_list.size()!=0) {
      //则加载快照
      auto it = ctx->hashlist->find(ctx->object_name)->second->sp_list.begin();
      ctx->hashlist->find(ctx->object_name)->second=(*it) ->snap_fb;
  }
  else {
    SPDK_DEBUGLOG(object_store, "the snaplook is not exits\n");
  }
  ctx->cb_fn(ctx->arg, objerrno);
  delete ctx;
}


void object_store::readwrite(std::string object_name, 
                     uint64_t offset, char* buf, uint64_t len, 
                     object_rw_complete cb_fn, void* arg, bool is_read) 
{
    SPDK_DEBUGLOG(object_store, "object %s offset:%lu len:%lu\n", object_name.c_str(), offset, len);
    if (offset + len >= blob_size) {
      SPDK_WARNLOG("object %s offset:%lu len:%lu beyond blob size %u\n",
          object_name.c_str(), offset, len, blob_size);
      len = blob_size - offset;
    }

    auto it = table.find(object_name);
    if (it != table.end()) {
        SPDK_DEBUGLOG(object_store, "object %s found, blob id:%" PRIu64 "\n", object_name.c_str(), it->second->blobid);
        blob_readwrite(it->second->blob, channel, offset, buf, len, cb_fn, arg, is_read);
    } else {
        SPDK_DEBUGLOG(object_store, "object %s not found\n", object_name.c_str());
        // 没找到，就先创建blob对象，之后再调用write_blob
        struct blob_create_ctx* ctx = new blob_create_ctx();
        struct spdk_blob_opts opts;
        ctx->is_read = is_read;
        ctx->mgr = this;
        ctx->object_name = object_name;
        ctx->offset = offset;
        ctx->buf = buf;
        ctx->len = len;
        ctx->cb_fn = cb_fn;
        ctx->arg = arg;
        spdk_blob_opts_init(&opts, sizeof(opts));
        opts.num_clusters = object_store::blob_cluster;
        spdk_bs_create_blob_ext(bs, &opts, create_done, ctx);
    }
}

/**
 * 下面都是static函数，因为c++的成员函数隐含的第一个参数是this指针，
 * 要传递给c语言的函数指针，需要写为static成员函数，或者非成员函数
 */
void object_store::blob_readwrite(struct spdk_blob *blob, struct spdk_io_channel * channel, 
                       uint64_t offset, char* buf, uint64_t len, 
                       object_rw_complete cb_fn, void* arg, bool is_read) 
{
  struct blob_rw_ctx* ctx;
  uint64_t start_lba, num_lba, pin_buf_length;
	uint32_t lba_size;

  ctx = new blob_rw_ctx;
  ctx->pin_buf = nullptr;
  ctx->is_read = is_read;

  ctx->offset = offset;
  ctx->buf = buf;
  ctx->len = len;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  get_page_parameters(offset, len, &start_lba, &lba_size, &num_lba);
  pin_buf_length = num_lba * lba_size;

  if (is_lba_aligned(offset, len)) {
      SPDK_DEBUGLOG(object_store, "aligned offset:%lu len:%lu\n", offset, len);
      ctx->is_aligned = true;
      if (is_read) {
        spdk_blob_io_write(blob, channel, buf, start_lba, num_lba, rw_done, ctx);
      } else {
        spdk_blob_io_read(blob, channel, buf, start_lba, num_lba, rw_done, ctx);
      }
  } else {
      SPDK_DEBUGLOG(object_store, "not aligned offset:%lu len:%lu\n", offset, len);
      ctx->is_aligned = false;
      ctx->blob = blob;
      ctx->channel = channel;

      ctx->start_lba = start_lba;
      ctx->num_lba = num_lba;
      ctx->pin_buf = (char*)spdk_malloc(pin_buf_length, lba_size, NULL,
                                        SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
      ctx->blocklen = lba_size;
      
      spdk_blob_io_read(blob, channel, ctx->pin_buf, start_lba, num_lba,
          read_done, ctx);
  }
}

// 所有读写最终都会进入这个回调。
void object_store::rw_done(void *arg, int objerrno) {
  struct blob_rw_ctx* ctx = (struct blob_rw_ctx*)arg;

  if (objerrno) {
    if (ctx->is_read) {
		  SPDK_ERRLOG("read offset:%lu len:%lu failed:%s\n", ctx->offset, ctx->len, spdk_strerror(objerrno));
    } else {
      SPDK_ERRLOG("write offset:%lu len:%lu failed:%s\n", ctx->offset, ctx->len, spdk_strerror(objerrno));
    }
		return;
	}

  // object层的处理代码，可以写在这里 

  if (ctx->pin_buf) {
    SPDK_DEBUGLOG(object_store, "free pin_buf: %p\n", ctx->pin_buf);
    spdk_free(ctx->pin_buf);
  }
  //最后执行用户的回调
  ctx->cb_fn(ctx->arg, objerrno);
  delete ctx;
}

// 只有非对齐的读写，会经过这个回调。
// 预先读取对齐内容，再根据 is_read 决定，写回磁盘或者返回给用户。
void object_store::read_done(void *arg, int objerrno) {
  struct blob_rw_ctx* ctx = (struct blob_rw_ctx*)arg;
  char*  pin_buf;

  if (objerrno) {
    SPDK_ERRLOG("prior read offset:%lu len:%lu start_lba:%lu num_lba:%lu failed:%s\n", 
        ctx->offset, ctx->len, ctx->start_lba, ctx->num_lba, spdk_strerror(objerrno));
		return;
	}

  pin_buf = (char *)((uintptr_t)ctx->pin_buf + (ctx->offset & (ctx->blocklen - 1)));
  if (ctx->is_read) {
    memcpy(ctx->buf, pin_buf, ctx->len);
    rw_done(ctx, 0);
  } else {
    memcpy(pin_buf, ctx->buf, ctx->len);
    spdk_blob_io_write(ctx->blob, ctx->channel,
				   ctx->pin_buf, ctx->start_lba, ctx->num_lba,
				   rw_done, ctx);
  }
}

void object_store::create_done(void *arg, spdk_blob_id blobid, int objerrno) {
  struct blob_create_ctx* ctx = (struct blob_create_ctx*)arg;

  if (objerrno) {
    SPDK_ERRLOG("name:%s blobid:%" PRIu64 " create failed:%s\n", 
        ctx->object_name.c_str(), blobid, spdk_strerror(objerrno));
		return;
	}

  ctx->blobid = blobid;

  spdk_bs_open_blob(ctx->mgr->bs, blobid, open_done, ctx);
}

void object_store::open_done(void *arg, struct spdk_blob *blob, int objerrno) {
  struct blob_create_ctx* ctx = (struct blob_create_ctx*)arg;

  if (objerrno) {
    SPDK_ERRLOG("name:%s blobid:%" PRIu64 " open failed:%s\n", 
        ctx->object_name.c_str(), ctx->blobid, spdk_strerror(objerrno));
		return;
	}
  SPDK_DEBUGLOG(object_store, "name:%s blobid:%" PRIu64 " opened\n", ctx->object_name.c_str(), ctx->blobid);

  // 成功打开
  struct object_store::fb_blob* fblob = new fb_blob;
  fblob->blobid = ctx->blobid;
  fblob->blob = blob;
  // 此处打开 blob 后要对 object_store 中的 table 进行修改，
  // 所以 blob_create_ctx 需要保存 object_store 指针
  ctx->mgr->table.emplace(std::move(ctx->object_name), fblob);

  blob_readwrite(blob, ctx->mgr->channel, ctx->offset, ctx->buf, ctx->len, 
                 ctx->cb_fn, ctx->arg, ctx->is_read);
  delete ctx;
}

/**
 * stop()停止运行，close掉所有blob
 */
void object_store::stop(object_rw_complete cb_fn, void* arg) {
  struct blob_close_ctx* ctx = new blob_close_ctx();

  if (table.empty()) {
    cb_fn(arg, 0);
    delete ctx;
    return;
  }

  auto it = table.begin();
  ctx->it  = it;
  ctx->mgr = this;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  spdk_blob_close(it->second->blob, close_done, ctx);
}

void object_store::close_done(void *arg, int objerrno) {
  struct blob_close_ctx* ctx = (struct blob_close_ctx*)arg;

  if (objerrno) {
    SPDK_ERRLOG("blobid:%" PRIu64 " close failed:%s\n", 
        ctx->it->second->blobid, spdk_strerror(objerrno));
		return;
	}

  SPDK_DEBUGLOG(object_store, "blobid:%" PRIu64 " closed\n", ctx->it->second->blobid);
  ctx->it++;
  auto& table = ctx->mgr->table;
  if (ctx->it == table.end()) {
    SPDK_DEBUGLOG(object_store, "close %lu blobids finish\n", table.size());
    ctx->cb_fn(ctx->arg, 0);
    delete ctx;
    return;
  }

  spdk_blob_close(ctx->it->second->blob, close_done, ctx);
}