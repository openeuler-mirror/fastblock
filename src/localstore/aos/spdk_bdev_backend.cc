/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "spdk_bdev_backend.h"

#include <spdk/env.h>
#include <spdk/log.h>

#include <cerrno>
#include <cstdlib>
#include <new>
#include <utility>

namespace fastblock::aos {
namespace {

struct io_ctx_append {
  spdk_bdev_backend* self{nullptr};
  device_backend::append_complete cb;
  void* arg{nullptr};
  uint64_t phys_offset{0};
  uint64_t nbytes{0};
};

struct io_ctx_op {
  spdk_bdev_backend* self{nullptr};
  device_backend::op_complete cb;
  void* arg{nullptr};
};

} // namespace

spdk_bdev_backend::~spdk_bdev_backend() { close(); }

void spdk_bdev_backend::bdev_event_cb(enum spdk_bdev_event_type type,
                                      struct spdk_bdev* bdev, void* event_ctx) {
  (void)event_ctx;
  SPDK_NOTICELOG("aos bdev event type=%d bdev=%s\n", static_cast<int>(type),
                 bdev ? spdk_bdev_get_name(bdev) : "null");
}

int spdk_bdev_backend::open(const char* bdev_name, uint64_t region_offset,
                            uint64_t region_length) {
  close();
  if (bdev_name == nullptr || region_length == 0) {
    return -EINVAL;
  }

  int rc = spdk_bdev_open_ext(bdev_name, true, bdev_event_cb, this, &desc_);
  if (rc != 0) {
    return rc;
  }
  bdev_ = spdk_bdev_desc_get_bdev(desc_);
  block_size_ = spdk_bdev_get_block_size(bdev_);
  if (block_size_ == 0) {
    close();
    return -EINVAL;
  }
  if ((region_offset % block_size_) != 0 ||
      (region_length % block_size_) != 0) {
    close();
    return -EINVAL;
  }

  const uint64_t bdev_bytes =
      spdk_bdev_get_num_blocks(bdev_) * static_cast<uint64_t>(block_size_);
  if (region_offset + region_length > bdev_bytes) {
    close();
    return -ERANGE;
  }

  channel_ = spdk_bdev_get_io_channel(desc_);
  if (channel_ == nullptr) {
    close();
    return -ENOMEM;
  }

  region_offset_ = region_offset;
  region_length_ = region_length;
  write_pointer_ = 0;
  write_inflight_ = 0;
  read_inflight_ = 0;
  flush_inflight_ = 0;
  return 0;
}

void spdk_bdev_backend::set_max_write_inflight(uint32_t n) {
  max_write_inflight_ = n == 0 ? 1 : n;
}

void spdk_bdev_backend::fail_queued(int err) {
  auto q = std::move(write_q_);
  write_q_.clear();
  for (auto& p : q) {
    if (p.cb) {
      p.cb(p.arg, 0, err);
    }
  }
}

void spdk_bdev_backend::close() {
  fail_queued(-ESHUTDOWN);
  if (channel_ != nullptr) {
    spdk_put_io_channel(channel_);
    channel_ = nullptr;
  }
  if (desc_ != nullptr) {
    spdk_bdev_close(desc_);
    desc_ = nullptr;
  }
  bdev_ = nullptr;
  block_size_ = 0;
  region_offset_ = 0;
  region_length_ = 0;
  write_pointer_ = 0;
  write_inflight_ = 0;
  read_inflight_ = 0;
  flush_inflight_ = 0;
}

void spdk_bdev_backend::set_write_pointer(uint64_t bytes_from_region_start) {
  if (bytes_from_region_start > region_length_) {
    return;
  }
  if (block_size_ && (bytes_from_region_start % block_size_) != 0) {
    return;
  }
  write_pointer_ = bytes_from_region_start;
}

void spdk_bdev_backend::write_done(struct spdk_bdev_io* bdev_io, bool success,
                                   void* cb_arg) {
  auto* ctx = static_cast<io_ctx_append*>(cb_arg);
  spdk_bdev_backend* self = ctx->self;
  const int err = success ? 0 : -EIO;
  if (success) {
    const uint64_t end = ctx->phys_offset + ctx->nbytes;
    if (end > self->write_pointer_) {
      self->write_pointer_ = end;
    }
  }
  if (self->write_inflight_ > 0) {
    --self->write_inflight_;
  }
  auto cb = std::move(ctx->cb);
  void* arg = ctx->arg;
  const uint64_t phys = ctx->phys_offset;
  delete ctx;
  spdk_bdev_free_io(bdev_io);
  if (cb) {
    cb(arg, phys, err);
  }
  self->pump_writes();
}

void spdk_bdev_backend::read_done(struct spdk_bdev_io* bdev_io, bool success,
                                  void* cb_arg) {
  auto* ctx = static_cast<io_ctx_op*>(cb_arg);
  spdk_bdev_backend* self = ctx->self;
  const int err = success ? 0 : -EIO;
  if (self != nullptr && self->read_inflight_ > 0) {
    --self->read_inflight_;
  }
  auto cb = std::move(ctx->cb);
  void* arg = ctx->arg;
  delete ctx;
  spdk_bdev_free_io(bdev_io);
  if (cb) {
    cb(arg, err);
  }
}

void spdk_bdev_backend::flush_done(struct spdk_bdev_io* bdev_io, bool success,
                                   void* cb_arg) {
  auto* ctx = static_cast<io_ctx_op*>(cb_arg);
  spdk_bdev_backend* self = ctx->self;
  const int err = success ? 0 : -EIO;
  if (self != nullptr && self->flush_inflight_ > 0) {
    --self->flush_inflight_;
  }
  auto cb = std::move(ctx->cb);
  void* arg = ctx->arg;
  delete ctx;
  spdk_bdev_free_io(bdev_io);
  if (cb) {
    cb(arg, err);
  }
}

void spdk_bdev_backend::append(void* data, uint64_t nbytes, append_complete cb,
                               void* arg) {
  write_at(write_pointer_, data, nbytes, std::move(cb), arg);
}

void spdk_bdev_backend::submit_write(uint64_t phys_offset, void* data,
                                     uint64_t nbytes, append_complete cb,
                                     void* arg) {
  auto* ctx = new (std::nothrow) io_ctx_append();
  if (ctx == nullptr) {
    if (cb) {
      cb(arg, 0, -ENOMEM);
    }
    return;
  }
  ctx->self = this;
  ctx->cb = std::move(cb);
  ctx->arg = arg;
  ctx->phys_offset = phys_offset;
  ctx->nbytes = nbytes;

  const uint64_t abs_off = region_offset_ + phys_offset;
  const uint64_t offset_blocks = abs_off / block_size_;
  const uint64_t num_blocks = nbytes / block_size_;

  ++write_inflight_;
  const int rc = spdk_bdev_write_blocks(desc_, channel_, data, offset_blocks,
                                        num_blocks, write_done, ctx);
  if (rc != 0) {
    if (write_inflight_ > 0) {
      --write_inflight_;
    }
    auto done = std::move(ctx->cb);
    void* a = ctx->arg;
    delete ctx;
    done(a, 0, rc);
    pump_writes();
  }
}

void spdk_bdev_backend::pump_writes() {
  while (write_inflight_ < max_write_inflight_ && !write_q_.empty()) {
    pending_write p = std::move(write_q_.front());
    write_q_.pop_front();
    submit_write(p.phys_offset, p.data, p.nbytes, std::move(p.cb), p.arg);
  }
}

void spdk_bdev_backend::write_at(uint64_t phys_offset, void* data,
                                  uint64_t nbytes, append_complete cb,
                                  void* arg) {
  if (!ready() || data == nullptr || cb == nullptr) {
    if (cb) {
      cb(arg, 0, -EINVAL);
    }
    return;
  }
  if (nbytes == 0 || (nbytes % block_size_) != 0 ||
      (phys_offset % block_size_) != 0) {
    cb(arg, 0, -EINVAL);
    return;
  }
  if (phys_offset + nbytes > region_length_) {
    cb(arg, 0, -ENOSPC);
    return;
  }
  if (write_inflight_ >= max_write_inflight_) {
    write_q_.push_back(
        pending_write{phys_offset, data, nbytes, std::move(cb), arg});
    return;
  }
  submit_write(phys_offset, data, nbytes, std::move(cb), arg);
}

void spdk_bdev_backend::read(uint64_t phys_offset, void* data, uint64_t nbytes,
                             op_complete cb, void* arg) {
  if (!ready() || data == nullptr || cb == nullptr) {
    if (cb) {
      cb(arg, -EINVAL);
    }
    return;
  }
  if (nbytes == 0 || (nbytes % block_size_) != 0 ||
      (phys_offset % block_size_) != 0) {
    cb(arg, -EINVAL);
    return;
  }
  if (phys_offset + nbytes > region_length_) {
    cb(arg, -ERANGE);
    return;
  }

  auto* ctx = new (std::nothrow) io_ctx_op();
  if (ctx == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  ctx->self = this;
  ctx->cb = std::move(cb);
  ctx->arg = arg;

  const uint64_t abs_off = region_offset_ + phys_offset;
  const uint64_t offset_blocks = abs_off / block_size_;
  const uint64_t num_blocks = nbytes / block_size_;

  ++read_inflight_;
  const int rc = spdk_bdev_read_blocks(desc_, channel_, data, offset_blocks,
                                       num_blocks, read_done, ctx);
  if (rc != 0) {
    if (read_inflight_ > 0) {
      --read_inflight_;
    }
    auto done = std::move(ctx->cb);
    void* a = ctx->arg;
    delete ctx;
    done(a, rc);
  }
}

void spdk_bdev_backend::flush(op_complete cb, void* arg) {
  if (!ready() || cb == nullptr) {
    if (cb) {
      cb(arg, -EINVAL);
    }
    return;
  }

  auto* ctx = new (std::nothrow) io_ctx_op();
  if (ctx == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  ctx->self = this;
  ctx->cb = std::move(cb);
  ctx->arg = arg;

  // Flush the whole region that may contain dirty writes.
  const uint64_t offset_blocks = region_offset_ / block_size_;
  const uint64_t num_blocks = region_length_ / block_size_;

  ++flush_inflight_;
  const int rc = spdk_bdev_flush_blocks(desc_, channel_, offset_blocks,
                                        num_blocks, flush_done, ctx);
  if (rc != 0) {
    if (flush_inflight_ > 0) {
      --flush_inflight_;
    }
    // malloc bdev and some devices do not support flush; durability is
    // already implied when the write callback completed for those devices.
    auto done = std::move(ctx->cb);
    void* a = ctx->arg;
    delete ctx;
    if (rc == -ENOTSUP || rc == -ENODEV || rc == -EINVAL) {
      done(a, 0);
    } else {
      done(a, rc);
    }
  }
}

} // namespace fastblock::aos
