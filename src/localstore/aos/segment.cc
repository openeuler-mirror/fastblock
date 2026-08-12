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

#include <cerrno>
#include <cstring>

namespace fastblock::aos {
namespace {

struct append_ctx {
  segment::append_record_complete cb;
  void* arg{nullptr};
};

void on_append_done(void* arg, uint64_t phys_offset, int err) {
  auto* ctx = static_cast<append_ctx*>(arg);
  auto cb = std::move(ctx->cb);
  void* user = ctx->arg;
  delete ctx;
  if (cb) {
    cb(user, phys_offset, err);
  }
}

} // namespace

void segment::append_record(const data_record& rec, void* dma_buf,
                            uint64_t dma_cap, append_record_complete cb,
                            void* arg) {
  if (backend_ == nullptr || dma_buf == nullptr || cb == nullptr) {
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

  std::memset(dma_buf, 0, static_cast<size_t>(aligned));
  std::memcpy(dma_buf, encoded.data(), encoded.size());

  auto* ctx = new (std::nothrow) append_ctx();
  if (ctx == nullptr) {
    cb(arg, 0, -ENOMEM);
    return;
  }
  ctx->cb = std::move(cb);
  ctx->arg = arg;

  backend_->append(dma_buf, aligned, on_append_done, ctx);
}

} // namespace fastblock::aos
