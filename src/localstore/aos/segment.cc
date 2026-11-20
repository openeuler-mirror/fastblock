/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "segment.h"

#include <spdk/env.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace fastblock::aos {
namespace {

void* alloc_io_buf(buffer_mode mode, size_t n, size_t align) {
  if (n == 0) {
    return nullptr;
  }
  if (mode == buffer_mode::spdk_dma) {
    return spdk_dma_zmalloc(n, align, nullptr);
  }
  void* p = nullptr;
  if (posix_memalign(&p, align, n) != 0) {
    return nullptr;
  }
  std::memset(p, 0, n);
  return p;
}

void free_io_buf(buffer_mode mode, void* p) {
  if (p == nullptr) {
    return;
  }
  if (mode == buffer_mode::spdk_dma) {
    spdk_dma_free(p);
  } else {
    std::free(p);
  }
}

struct append_ctx {
  segment* self{nullptr};
  segment::append_record_complete cb;
  void* arg{nullptr};
  // If non-empty, first write is segment_seal; then write this payload.
  std::vector<uint8_t> data_encoded;
  uint64_t data_aligned{0};
  void* owned_buf{nullptr};
  uint64_t owned_cap{0};
  void* dma_buf{nullptr};
  uint64_t dma_cap{0};
  buffer_mode mode{buffer_mode::heap};
  bool seal_phase{false};
  uint64_t data_phys{0};
};

void finish_ctx(append_ctx* ctx, uint64_t phys, int err) {
  auto cb = std::move(ctx->cb);
  void* user = ctx->arg;
  free_io_buf(ctx->mode, ctx->owned_buf);
  ctx->owned_buf = nullptr;
  delete ctx;
  if (cb) {
    cb(user, phys, err);
  }
}

void write_data_phase(append_ctx* ctx);

void on_write_done(void* arg, uint64_t phys_offset, int err) {
  auto* ctx = static_cast<append_ctx*>(arg);
  if (ctx->seal_phase) {
    ctx->seal_phase = false;
    if (err != 0) {
      finish_ctx(ctx, 0, err);
      return;
    }
    // Seal record written into the tail of the old segment; reserve() for
    // the user record will seal/open the next free segment as needed.
    write_data_phase(ctx);
    return;
  }
  finish_ctx(ctx, phys_offset, err);
}

void write_data_phase(append_ctx* ctx) {
  segment* self = ctx->self;
  uint64_t phys = 0;
  const int rerr =
      self->allocator()->reserve(ctx->data_aligned, &phys, nullptr);
  if (rerr != 0) {
    finish_ctx(ctx, 0, rerr);
    return;
  }
  ctx->data_phys = phys;
  std::memset(ctx->owned_buf, 0, static_cast<size_t>(ctx->data_aligned));
  std::memcpy(ctx->owned_buf, ctx->data_encoded.data(),
              ctx->data_encoded.size());
  // Callers (write_after_data) read header_len from their scratch.
  if (ctx->dma_buf != nullptr &&
      ctx->dma_cap >= ctx->data_encoded.size()) {
    std::memcpy(ctx->dma_buf, ctx->data_encoded.data(),
                ctx->data_encoded.size());
  }
  self->backend()->write_at(phys, ctx->owned_buf, ctx->data_aligned,
                            on_write_done, ctx);
}

} // namespace

void segment::append_record(const data_record& rec, void* dma_buf,
                            uint64_t dma_cap, append_record_complete cb,
                            void* arg) {
  if (backend_ == nullptr || alloc_ == nullptr || dma_buf == nullptr ||
      cb == nullptr) {
    if (cb) {
      cb(arg, 0, -EINVAL);
    }
    return;
  }

  std::vector<uint8_t> encoded = encode_record(rec);
  const uint64_t aligned = align_up(encoded.size());
  if (aligned > dma_cap) {
    cb(arg, 0, -ENOSPC);
    return;
  }

  auto* ctx = new (std::nothrow) append_ctx();
  if (ctx == nullptr) {
    cb(arg, 0, -ENOMEM);
    return;
  }
  ctx->self = this;
  ctx->cb = std::move(cb);
  ctx->arg = arg;
  ctx->data_encoded = std::move(encoded);
  ctx->data_aligned = aligned;
  ctx->dma_buf = dma_buf;
  ctx->dma_cap = dma_cap;
  ctx->mode = mode_;

  uint64_t buf_need = aligned;
  const uint64_t seal_aligned = align_up(kRecordHeaderLenV1);
  if (alloc_->ensure_open() == 0 && alloc_->would_roll(aligned) &&
      alloc_->remaining_in_open() >= seal_aligned) {
    buf_need = std::max(buf_need, seal_aligned);
  }
  const size_t align = backend_ && backend_->block_size() > 0
                           ? std::max<size_t>(backend_->block_size(), 4096)
                           : 4096;
  ctx->owned_buf = alloc_io_buf(mode_, static_cast<size_t>(buf_need), align);
  ctx->owned_cap = buf_need;
  if (ctx->owned_buf == nullptr) {
    finish_ctx(ctx, 0, -ENOMEM);
    return;
  }

  // If the user record would roll the segment, try to write a segment_seal
  // into the remaining pad first (design record type segment_seal).
  if (alloc_->ensure_open() == 0 && alloc_->would_roll(aligned)) {
    if (alloc_->remaining_in_open() >= seal_aligned) {
      data_record seal;
      seal.type = record_type::segment_seal;
      seal.pool_id = rec.pool_id;
      seal.pg_id = rec.pg_id;
      seal.store_epoch = rec.store_epoch;
      auto seal_bytes = encode_record(seal);
      const uint64_t sa = align_up(seal_bytes.size());
      uint64_t seal_phys = 0;
      if (alloc_->reserve(sa, &seal_phys, nullptr) == 0 && sa <= dma_cap &&
          sa <= ctx->owned_cap) {
        ctx->seal_phase = true;
        std::memset(ctx->owned_buf, 0, static_cast<size_t>(sa));
        std::memcpy(ctx->owned_buf, seal_bytes.data(), seal_bytes.size());
        backend_->write_at(seal_phys, ctx->owned_buf, sa, on_write_done, ctx);
        return;
      }
    }
  }

  write_data_phase(ctx);
}

} // namespace fastblock::aos
