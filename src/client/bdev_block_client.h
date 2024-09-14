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
#pragma once

#include "client/block_client_pool.h"
#include "utils/utils.h"
#include "utils/err_num.h"

#include <spdk/bdev_module.h>

namespace fastblock {
namespace client {

class bdev_block_client {

private:

    using write_callback_type = std::function<void(::spdk_bdev_io*, int32_t, std::function<void()>)>;
    using read_callback_type = std::function<void(::spdk_bdev_io*, std::unique_ptr<char[]>, uint64_t, int32_t, std::function<void()>)>;
    using callback_type = std::variant<
      std::monostate,
      write_callback_type,
      read_callback_type
    >;

    struct request_type {
        int64_t id{0};
        ::spdk_bdev_io* bdev_io{nullptr};
        size_t object_count{0};
        callback_type cb{};
        int32_t err_code{::err::E_SUCCESS};
        std::unique_ptr<char[]> buf{nullptr};
        size_t size{0};
    };

    struct write_args {
        int32_t pool_id{};
        uint64_t offset{};
        char* image_name{};
        uint64_t length{};
        ::spdk_bdev_io* bdev_io{};
        write_callback_type cb{};
        bdev_block_client* this_client{};
    };

    struct read_args {
        int32_t pool_id{};
        char* image_name{};
        uint64_t offset{};
        uint64_t length{};
        ::spdk_bdev_io* bdev_io{};
        read_callback_type cb{};
        bdev_block_client* this_client{};
    };

    struct write_done_context {
        bdev_block_client* this_client{nullptr};
        request_type* request{nullptr};
        std::function<void()> notifier;
        int32_t err_code{::err::E_SUCCESS};
    };

    struct read_done_context {
        bdev_block_client* this_client{nullptr};
        request_type* request{nullptr};
        std::function<void()> notifier;
        int32_t err_code{::err::E_SUCCESS};
    };

public:

    bdev_block_client() = delete;

    bdev_block_client(uint32_t worker_core, block_client_pool* blk_pool)
      : _worker_core{worker_core}
      , _blk_pool{blk_pool} {}

    bdev_block_client(const bdev_block_client&) = delete;

    bdev_block_client(bdev_block_client&&) = default;

    bdev_block_client& operator=(const bdev_block_client&) = delete;

    bdev_block_client& operator=(bdev_block_client&&) = delete;

    ~bdev_block_client() noexcept = default;

private:

    /************************************************************
     * private methods
     ************************************************************/

    static uint64_t get_obj_num(const uint64_t offset, const uint64_t length) {
        auto start_off = ::utils::align_down(offset, _default_object_size);
        auto end_off = ::utils::align_up(offset + length, _default_object_size);

        return (end_off - start_off) / _default_object_size;
    }

    static std::string calc_image_object_prefix(const uint64_t pool_id, char* image_name) {
        return FMT_2("%1%__blk_data___%2%", pool_id, image_name);
    }

    static std::tuple<size_t, uint64_t, uint64_t>
    calc_first_object_position(const uint64_t offset, const uint64_t length, const size_t object_size) {
        auto first_object_offset = offset % object_size;
        auto first_object_size = object_size - first_object_offset;
        if (length < first_object_size) {
            first_object_size = length;
        }
        uint64_t object_seq = offset / uint64_t(object_size);

        return std::make_tuple(first_object_size, first_object_offset, object_seq);
    }

    static std::string get_image_object_name(std::string &prefix, uint64_t seq) {
        return FMT_2("%1%%2%", prefix, seq);
    }

public:

    /************************************************************
     * public methods
     ************************************************************/

    static auto default_object_size() noexcept {
        return _default_object_size;
    }

    void write(
      const int32_t pool_id,
      char* image_name,
      const uint64_t offset,
      uint64_t length,
      ::spdk_bdev_io* bdev_io,
      write_callback_type cb) {
        if (length == 0) {
            SPDK_INFOLOG(
              blkcli,
              "bdev write to pool %d, offset %ld, image %s with length 0, return directly\n",
              pool_id, offset, image_name);
            cb(bdev_io, ::err::E_SUCCESS, [](){});
            return;
        }

        auto* args = new write_args{pool_id, offset, image_name, length, bdev_io, std::move(cb), this};
        auto* evt = ::spdk_event_allocate(_worker_core, handle_write_args, args, nullptr);
        ::spdk_event_call(evt);
    }

    void read(
      const int32_t pool_id,
      char* image_name,
      const uint64_t offset,
      const uint64_t length,
      ::spdk_bdev_io* bdev_io,
      read_callback_type cb) {
        if (length == 0) {
            cb(bdev_io, nullptr, 0, ::err::E_SUCCESS, [](){});
            return;
        }

        SPDK_DEBUGLOG(blkcli, "read(): cb address is %p\n", &cb);
        auto* args = new read_args{
          static_cast<int32_t>(pool_id),
          image_name,
          offset,
          length,
          bdev_io,
          std::move(cb),
          this};
        auto* evt = ::spdk_event_allocate(_worker_core, handle_read_args, args, nullptr);
        ::spdk_event_call(evt);
    }

public:

    /************************************************************
     * spdk callbacks
     ************************************************************/

    static void handle_object_write_done(void* msg, void* arg) {
        auto* raw_ctx = reinterpret_cast<write_done_context*>(msg);
        auto ctx = std::unique_ptr<write_done_context>{raw_ctx};
        if (ctx->err_code != ::err::E_SUCCESS) {
            SPDK_ERRLOG("write object error %s\n", ::err::string_status(ctx->err_code));
            ctx->request->err_code = ctx->err_code;
        }

        ctx->request->object_count -= 1;
        if (ctx->request->object_count != 0) {
            return;
        }

        SPDK_INFOLOG(blkcli, "write done for request id %ld\n", ctx->request->id);
        auto& cb = std::get<write_callback_type>(ctx->request->cb);
        cb(ctx->request->bdev_io, ctx->err_code, ctx->notifier);
        ctx->this_client->_requests.erase(ctx->request->id);
    }

    static void handle_object_read_done(void* msg, void* arg) {
        auto* raw_ctx = reinterpret_cast<read_done_context*>(msg);
        auto ctx = std::unique_ptr<read_done_context>{raw_ctx};
        if (ctx->err_code != ::err::E_SUCCESS) {
            SPDK_ERRLOG("read object error %s\n", ::err::string_status(ctx->err_code));
            ctx->request->err_code = ctx->err_code;
        }

        ctx->request->object_count -= 1;
        if (ctx->request->object_count != 0) {
            return;
        }

        SPDK_INFOLOG(blkcli, "read done for request id %ld\n", ctx->request->id);
        auto& cb = std::get<read_callback_type>(ctx->request->cb);
        SPDK_DEBUGLOG(blkcli, "handle_object_read_done(): cb address is %p\n", &cb);
        cb(ctx->request->bdev_io,
          std::move(ctx->request->buf),
          ctx->request->size,
          ctx->request->err_code,
          ctx->notifier);
        SPDK_DEBUGLOG(blkcli, "free request %ld\n", ctx->request->id);
        ctx->this_client->_requests.erase(ctx->request->id);
    }

    static void handle_write_args(void* msg, void* arg) {
        auto* raw_args = reinterpret_cast<write_args*>(msg);
        std::unique_ptr<write_args> args{raw_args};
        auto object_prefix = calc_image_object_prefix(args->pool_id, args->image_name);
        auto [expected_object_size, object_offset, object_seq]
          = calc_first_object_position(args->offset, args->length, _default_object_size);
        size_t written_bytes{0};
        auto obj_num = get_obj_num(args->offset, args->length);
        SPDK_INFOLOG(
          blkcli,
          "write pool: %d image_name: %s offset: %lu length: %lu  obj_num: %lu\n",
          args->pool_id, args->image_name, args->offset, args->length, obj_num);

        int iov_index{0};
        size_t iov_written_size{0};
        std::ptrdiff_t iov_offset{0};
        auto req = std::make_unique<request_type>(
          args->this_client->_id_gen++, args->bdev_io, 0, std::move(args->cb));
        SPDK_INFOLOG(blkcli, "start processing bdev write request of id %ld\n", req->id);

        while (written_bytes < static_cast<size_t>(args->length)) {
            auto object_name = get_image_object_name(object_prefix, object_seq);
            SPDK_INFOLOG(
              blkcli,
              "dispatch object('%s') write request, bdev request id is %ld\n",
              object_name.c_str(), req->id);

            SPDK_DEBUGLOG(
              blkcli,
              "object name %s, written_bytes %ld, args->length %ld\n",
              object_name.c_str(), written_bytes, args->length);

            args->this_client->_blk_pool->write_object(
              args->bdev_io->u.bdev.iovs,
              iov_index,
              expected_object_size,
              iov_offset,
              std::move(object_name),
              object_offset,
              args->pool_id,
              [this_cli = args->this_client, req = req.get()] (int32_t status, std::function<void()> notifier) {
                  auto* ctx = new write_done_context{this_cli, req, notifier, status};
                  auto* evt = ::spdk_event_allocate(this_cli->_worker_core, handle_object_write_done, ctx, nullptr);
                  ::spdk_event_call(evt);
              });

            iov_written_size = 0;
            for (; iov_index < args->bdev_io->u.bdev.iovcnt; ++iov_index) {
                SPDK_DEBUGLOG(
                  blkcli,
                  "iov_index: %d, iov_written_size: %ld, iov_offset: %ld, expected_object_size: %ld, iov_len: %ld\n",
                  iov_index, iov_written_size, iov_offset, expected_object_size,
                  args->bdev_io->u.bdev.iovs[iov_index].iov_len);

                iov_written_size += args->bdev_io->u.bdev.iovs[iov_index].iov_len - iov_offset;
                iov_offset = 0;
                if (iov_written_size < expected_object_size) {
                    continue;
                } else if (iov_written_size == expected_object_size) {
                    ++iov_index;
                    break;
                }

                iov_offset = expected_object_size - (iov_written_size - args->bdev_io->u.bdev.iovs[iov_index].iov_len);
                break;
            }

            written_bytes += expected_object_size;
            expected_object_size = _default_object_size;
            if (expected_object_size > (args->length - written_bytes)) {
                expected_object_size = args->length - written_bytes;
            }
            object_offset = 0;
            ++object_seq;
            ++(req->object_count);

            SPDK_DEBUGLOG(
              blkcli,
              "iov_index: %d, iov_written_size: %ld, iov_offset: %ld, expected_object_size: %ld, iov_len %ld, object_seq %ld\n",
              iov_index, iov_written_size, iov_offset, expected_object_size,
              args->bdev_io->u.bdev.iovs[iov_index].iov_len, object_seq);
        }

        args->this_client->_requests.emplace(req->id, std::move(req));
    }

    static void handle_read_args(void* msg, void* arg) {
        auto* raw_args = reinterpret_cast<read_args*>(msg);
        auto args = std::unique_ptr<read_args>{raw_args};
        auto object_prefix = calc_image_object_prefix(args->pool_id, args->image_name);
        auto [expected_object_size, object_offset, object_seq]
          = calc_first_object_position(args->offset, args->length, _default_object_size);
        size_t read_bytes{0};
        auto obj_num = get_obj_num(args->offset, args->length);
        SPDK_INFOLOG(
          blkcli,
          "read pool: %d image_name: %s offset: %lu length: %lu  obj_num: %lu\n",
          args->pool_id, args->image_name, args->offset, args->length, obj_num);

        auto req = std::make_unique<request_type>(
          args->this_client->_id_gen++,
          args->bdev_io, 0,
          std::move(args->cb),
          ::err::E_SUCCESS,
          std::make_unique<char[]>(args->length),
          args->length);
        SPDK_INFOLOG(blkcli, "start processing bdev read request of id %ld\n", req->id);
        std::ptrdiff_t buf_offset{0};

        while (read_bytes < args->length) {
            auto object_name = get_image_object_name(object_prefix, object_seq);
            SPDK_INFOLOG(
              blkcli,
              "dispatch object('%s') read request, bdev request id is %ld, object_offset %ld, read size %ld\n",
              object_name.c_str(), req->id, object_offset, expected_object_size);
            args->this_client->_blk_pool->read_object(
              std::move(object_name),
              object_offset,
              expected_object_size,
              args->pool_id,
              [this_cli = args->this_client, req = req.get(), buf_offset]
              (const std::string& data, int32_t status, std::function<void()> notifier) {
                  if (status == err::E_SUCCESS) {
                      SPDK_DEBUGLOG(
                        blkcli,
                        "read object done, cp offset %ld, size %ld, request id is %ld, buf size is %ld\n",
                        buf_offset, data.size(), req->id, req->size);
                      std::memcpy(req->buf.get() + buf_offset, data.data(), data.size());
                  }

                  auto* ctx = new read_done_context{this_cli, req, notifier, status};
                  auto* evt = ::spdk_event_allocate(this_cli->_worker_core, handle_object_read_done, ctx, nullptr);
                  ::spdk_event_call(evt);
              });

            buf_offset += expected_object_size;
            read_bytes += expected_object_size;
            expected_object_size = _default_object_size;
            if (expected_object_size > (args->length - read_bytes)) {
                expected_object_size = args->length - read_bytes;
            }

            object_offset = 0;
            object_seq++;
            ++(req->object_count);
        }

        args->this_client->_requests.emplace(req->id, std::move(req));
    }

private:

    uint32_t _worker_core{0};
    block_client_pool* _blk_pool{nullptr};
    int64_t _id_gen{0};
    std::unordered_map<int64_t, std::unique_ptr<request_type>> _requests{};

    static constexpr size_t _default_object_size{4 * 1024 * 1024}; // 4MiB
};

} // namespace client
} // namespace fastblock
