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

#include "fastblock/msg/rdma/memory_pool.h"
#include "fastblock/msg/rdma/pool_quota.h"
#include "fastblock/msg/rdma/types.h"

#include <google/protobuf/message.h>

#include <spdk/log.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <memory>
#include <string_view>
#include <vector>

namespace msg {
namespace rdma {

class transport_data {

private:

    using rdma_read_tag_type = uint8_t;

    static constexpr uint8_t _inline_tag{1};
    static constexpr uint8_t _no_inline_tag{0};
    static constexpr size_t _rdma_read_tag_length{sizeof(rdma_read_tag_type)};
    static constexpr rdma_read_tag_type _un_complete_tag{0b10101010};   // 170
    static constexpr rdma_read_tag_type _complete_tag{0b01010101};      // 85

public:

    using correlation_index_type = uint32_t;

    struct remote_info {
        uint32_t rkey;
        void* raddr;
    };

    struct inline_data {
        uint8_t is_inlined{_inline_tag};
        correlation_index_type correlation_index{0};
        uint32_t io_length{0};
    };

    static constexpr size_t inline_data_header_size{sizeof(inline_data)};

    struct metadata {
        uint8_t is_inlined{_no_inline_tag};
        correlation_index_type correlation_index{0};
        uint32_t metadata_count{1};
        uint32_t serial_no{0};
        uint32_t io_length;
        uint32_t io_count;
    };

    static constexpr size_t metadata_header_size{sizeof(metadata)}; // 24

public:

    transport_data() = delete;

    transport_data(
      const correlation_index_type correlation_index,
      const size_t serialized_size,
      std::shared_ptr<memory_pool<::ibv_send_wr>> meta_pool,
      std::shared_ptr<memory_pool<::ibv_send_wr>> data_pool,
      const alloc_class cls = alloc_class::reserved)
      : _correlation_index{correlation_index}
      , _meta_pool{meta_pool}
      , _data_pool{data_pool}
      , _transport_size{serialized_size + 2 * _rdma_read_tag_length}
      , _serialized_size{serialized_size}
      , _data_chunk_size{_data_pool->element_size()}
      , _metas(0)
      , _datas(0)
      , _alloc_class{cls} {
        init();
    }

    transport_data(std::shared_ptr<memory_pool<::ibv_send_wr>> data_pool)
      : _data_pool{data_pool}
      , _data_chunk_size{_data_pool->element_size()}
      , _metas(0)
      , _datas(0) {}

    transport_data(const transport_data&) = delete;

    transport_data(transport_data&&) = delete;

    transport_data& operator=(const transport_data&) = delete;

    transport_data& operator=(transport_data&&) = delete;

    ~transport_data() noexcept {
        release_allocated();
        SPDK_DEBUGLOG(
          msg,
          "data pool size: %lu, meta pool size: %ld\n",
          _data_pool ? _data_pool->size() : 0,
          _meta_pool ? _meta_pool->size() : -1);
    }

public:

    static auto read_metacount(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<metadata*>(ctx->mr->addr);
        return data->metadata_count;
    }

    static auto read_serial_no(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<metadata*>(ctx->mr->addr);
        return data->serial_no;
    }

    static bool is_inlined(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<inline_data*>(ctx->mr->addr);
        return static_cast<bool>(data->is_inlined);
    }

    static bool is_new(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<metadata*>(ctx->mr->addr);
        return data->serial_no == 0;
    }

    static status read_reply_meta(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<inline_data*>(ctx->mr->addr);
        if (data->is_inlined == _no_inline_tag) {
            return status::success;
        }

        auto* content = read_inlined_content(ctx);
        auto* m = reinterpret_cast<reply_meta*>(content);
        return static_cast<status>(m->reply_status);
    }

    static correlation_index_type
    read_correlation_index(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<inline_data*>(ctx->mr->addr);
        return data->correlation_index;
    }

    static uint32_t read_inlined_content_length(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<inline_data*>(ctx->mr->addr);
        return data->io_length;
    }

    static char* read_inlined_content(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        return reinterpret_cast<char*>(ctx->mr->addr) + inline_data_header_size;
    }

    static void prepare_post_receive(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* h = reinterpret_cast<uint32_t*>(ctx->mr->addr);
        *h = _magic_no;
    }

    static bool is_no_content(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* h = reinterpret_cast<uint32_t*>(ctx->mr->addr);
        return *h == _magic_no;
    }

public:

    bool is_inlined() noexcept {
        return _is_inlined;
    }

    bool prepared() const noexcept { return _prepared; }

    bool too_large() const noexcept { return _too_large; }

    bool is_ready() noexcept {
        if (not _is_inlined) {
            return _prepared and (_metas.size() == _meta_count) and
                   (_datas.size() == _data_count);
        }
        return _prepared and not _metas.empty();
    }

    std::vector<memory_pool<::ibv_send_wr>::net_context*> release_data_contexts() {
        std::vector<memory_pool<::ibv_send_wr>::net_context*> out;
        out.swap(_datas);
        return out;
    }

    [[gnu::always_inline]] char* serialized_buf() noexcept {
        return _serialized_buf.get();
    }

    [[gnu::always_inline]] char* data_head() noexcept {
        return _data_head;
    }

    [[gnu::always_inline]] size_t serilaized_size() noexcept {
        return _transport_size;
    }

    [[gnu::always_inline]] auto correlation_index() noexcept {
        return _correlation_index;
    }

    [[gnu::always_inline]] auto meatdata_capacity() noexcept {
        return _meta_count;
    }

    [[gnu::always_inline]] auto data_capacity() noexcept {
        return _data_count;
    }

    [[gnu::always_inline]] bool is_metadata_complete() noexcept {
        return _is_metadata_complete;
    }

    bool is_rdma_read_complete() noexcept {
        return *_head_tag == _complete_tag and *_tail_tag == _complete_tag;
    }

    request_meta read_request_meta() noexcept {
        return load_request_meta(_data_head);
    }

    void from_net_context(memory_pool<::ibv_recv_wr>::net_context* ctx) {
        auto* data = reinterpret_cast<metadata*>(ctx->mr->addr);
        auto* rm_info = reinterpret_cast<remote_info*>(
          reinterpret_cast<char*>(data) + metadata_header_size);
        _data_count += data->io_count;
        _transport_size += data->io_length;

        if (not _rm_infos) {
            _rm_infos = std::make_unique<remote_info[]>(
              data->metadata_count * data->io_count);
        }

        auto* rm_infos = _rm_infos.get();

        SPDK_DEBUGLOG(
          msg,
          "[%p] _data_count: %ld, _transport_size: %ld, data->io_count: %d, "
          "data->io_length: %d, data->is_lined: %d, data->correlation_index: %d, "
          "data->metadata_count: %d, data->serial_no: %d, _last_rm_info_index: %d, "
          "_rm_infos size: %d\n",
          this, _data_count, _transport_size, data->io_count, data->io_length,
          data->is_inlined, data->correlation_index, data->metadata_count,
          data->serial_no, _last_rm_info_index, data->metadata_count * data->io_count);

        for (uint32_t i{0}; i < data->io_count; ++i) {
            rm_infos[_last_rm_info_index].raddr = rm_info[i].raddr;
            rm_infos[_last_rm_info_index].rkey = rm_info[i].rkey;
            ++_last_rm_info_index;
        }

        if (data->serial_no < 64) {
            _seen_serials |= (1ull << data->serial_no);
        }
        _expected_serials = data->metadata_count;
        const uint64_t need =
          _expected_serials >= 64
            ? ~uint64_t{0}
            : ((_expected_serials == 0)
                 ? 0
                 : ((uint64_t{1} << _expected_serials) - 1));
        if (_expected_serials > 0 && _expected_serials <= 64 &&
            (_seen_serials & need) == need) {
            _is_metadata_complete = true;
            _serialized_size = _transport_size - 2 * _rdma_read_tag_length;
            if (exceeds_pool_capacity()) {
                _too_large = true;
                _prepared = false;
            } else if (not alloc_reply_data()) {
                _prepared = false;
                SPDK_NOTICELOG(
                  "Not enough memory for reply transport, need data element is %ld, but has %ld\n",
                  _data_count, _data_pool ? _data_pool->size() : 0);
            }
        }
    }

    void serialize_data(void* meta_src, const size_t meta_size,
                        const google::protobuf::Message* request,
                        std::string_view payload = {}) {
        std::ptrdiff_t off{0};
        if (meta_src) {
            off = static_cast<std::ptrdiff_t>(meta_size);
        }

        auto ser_size = request->ByteSizeLong();
        const size_t body_size = ser_size + payload.size();
        if (_is_inlined) {
            auto* data = reinterpret_cast<inline_data*>(_metas[0]->mr->addr);
            auto* content = reinterpret_cast<char*>(data) + inline_data_header_size;
            if (meta_src) {
                std::memcpy(content, meta_src, meta_size);
            }
            request->SerializeToArray(content + off, static_cast<int>(ser_size));
            if (!payload.empty()) {
                std::memcpy(content + off + static_cast<std::ptrdiff_t>(ser_size),
                            payload.data(), payload.size());
            }
            data->io_length = static_cast<uint32_t>(ser_size);
            SPDK_DEBUGLOG(msg, "serialized size is %lu + %lu + payload %lu\n",
                          meta_size, ser_size, payload.size());
            (void)body_size;
            return;
        }

        _serialized_buf = std::make_unique<char[]>(_serialized_size);
        if (meta_src) {
            std::memcpy(_serialized_buf.get(), meta_src, meta_size);
        }
        request->SerializeToArray(_serialized_buf.get() + off,
                                  static_cast<int>(ser_size));
        if (!payload.empty()) {
            std::memcpy(_serialized_buf.get() + off +
                          static_cast<std::ptrdiff_t>(ser_size),
                        payload.data(), payload.size());
        }

        fill_rdma_read_tag(&_complete_tag);
        std::ptrdiff_t cpy_off{_rdma_read_tag_length};
        size_t cpy_len{std::min(_data_chunk_size - cpy_off, _serialized_size)};
        size_t copied{0};
        for (decltype(_data_count) i{0}; i < _data_count; ++i) {
            std::memcpy(
              reinterpret_cast<char*>(_datas[i]->mr->addr) + cpy_off,
              _serialized_buf.get() + copied,
              cpy_len);

            copied += cpy_len;
            cpy_off = 0;
            cpy_len = std::min(_data_chunk_size - cpy_off, _serialized_size - copied);
        }
    }

    void serialize_data(reply_meta* meta) {
        assert(_is_inlined);

        auto* data = reinterpret_cast<inline_data*>(_metas[0]->mr->addr);
        auto* content = reinterpret_cast<char*>(data) + inline_data_header_size;
        std::memcpy(content, meta, reply_meta_size);
    }

    bool unserialize_data(google::protobuf::Message* m,
                          const std::ptrdiff_t start_off = 0,
                          size_t envelope_size = 0,
                          std::string_view* payload = nullptr) {
        const char* body = nullptr;
        size_t body_len = 0;
        if (_data_count == 1) {
            body = _data_head + start_off;
            body_len = _serialized_size - static_cast<size_t>(start_off);
        } else {
            _serialized_buf = std::make_unique<char[]>(_serialized_size);
            auto cpy_off = start_off + _rdma_read_tag_length;

            size_t copied{0};
            size_t cpy_len{_data_chunk_size - cpy_off};
            for (size_t i{0}; i < _data_count - 1; ++i) {
                std::memcpy(
                  _serialized_buf.get() + copied,
                  reinterpret_cast<char*>(_datas[i]->mr->addr) + cpy_off,
                  cpy_len);

                copied += cpy_len;
                cpy_off = 0;
                cpy_len = std::min(_data_chunk_size - cpy_off,
                                   _serialized_size - start_off - copied);
            }

            auto last_cpy_length = _serialized_size - copied;
            std::memcpy(
              _serialized_buf.get() + copied,
              _datas[_data_count - 1]->mr->addr,
              last_cpy_length);
            body = _serialized_buf.get();
            body_len = _serialized_size - static_cast<size_t>(start_off);
        }

        if (envelope_size == 0 || envelope_size > body_len) {
            envelope_size = body_len;
        }
        if (payload != nullptr && body_len > envelope_size) {
            *payload = std::string_view(body + envelope_size, body_len - envelope_size);
        } else if (payload != nullptr) {
            *payload = {};
        }
        return m->ParseFromArray(body, static_cast<int>(envelope_size));
    }

    bool mark_bad_read(::ibv_send_wr* bad) noexcept {
        return mark_bad_post(
          bad,
          _data_start_offset,
          &(_datas[_data_start_offset]->wr),
          _datas.size());
    }

    bool mark_bad_send(::ibv_send_wr* bad) noexcept {
        return mark_bad_post(
          bad,
          _meta_start_offset,
          &(_metas[_meta_start_offset]->wr),
          _metas.size());
    }

    ::ibv_send_wr* make_read_request(std::function<uint64_t()> wr_id_gen, const bool should_signal = false) noexcept {
        SPDK_DEBUGLOG(msg,
          "_data_start_offset: %ld, wrs_length: %ld\n",
          _data_start_offset, _data_count);

        if (_data_start_offset != 0) {
            return &(_datas[_data_start_offset]->wr);
        }

        auto* rm_infos = _rm_infos.get();
        decltype(_transport_size) tmp_sge_len{_transport_size};
        for (size_t i{0}; i < _data_count; ++i) {
            _datas[i]->wr.wr_id = wr_id_gen();
            _datas[i]->wr.opcode = IBV_WR_RDMA_READ;
            _datas[i]->wr.send_flags = 0;
            _datas[i]->wr.wr.rdma.remote_addr = uint64_t(rm_infos[i].raddr);
            _datas[i]->wr.wr.rdma.rkey = rm_infos[i].rkey;
            _datas[i]->sge.length = std::min(tmp_sge_len, _data_pool->element_size());
            tmp_sge_len -= _data_pool->element_size();
        }

        static constexpr uint32_t aling_up{512};
        if (_datas.back()->sge.length % aling_up != 0) {
            uint32_t n = _datas.back()->sge.length / aling_up;
            SPDK_DEBUGLOG(
              msg,
              "_datas.back()->sge.length %d, n %d, new _datas.back()->sge.length %d\n",
              _datas.back()->sge.length, n, aling_up * (n + 1));
            _datas.back()->sge.length = aling_up * (n + 1);
        }

        for (size_t i{0}; i < _data_count - 1; ++i) {
            _datas[i]->wr.next = &(_datas[i + 1]->wr);
        }
        _datas[_data_count - 1]->wr.next = nullptr;

        if (should_signal) {
            _datas[0]->wr.send_flags = IBV_SEND_SIGNALED;
        }

        return &(_datas[0]->wr);
    }

    ::ibv_send_wr* make_send_request(std::function<uint64_t()> wr_id_gen, const bool should_signal = false) noexcept {
        SPDK_DEBUGLOG(msg,
          "_meta_start_offset: %ld, wrs_length: %ld\n",
          _meta_start_offset, _meta_count);

        if (_meta_start_offset != 0) {
            return &(_metas[_meta_start_offset]->wr);
        }

        for (auto* m : _metas) {
            m->wr.wr_id = wr_id_gen();
            m->wr.opcode = IBV_WR_SEND;
            m->wr.send_flags = 0;
        }

        if (should_signal) {
            _metas[0]->wr.send_flags = IBV_SEND_SIGNALED;
        }

        if (_metas.size() > 1) {
            for (size_t i{1}; i < _metas.size(); ++i) {
                _metas[i - 1]->wr.next = &(_metas[i]->wr);
            }
        }
        _metas[_metas.size() - 1]->wr.next = nullptr;

        return &(_metas[0]->wr);
    }

private:

    bool mark_bad_post(::ibv_send_wr* bad, size_t& offset_marker, ::ibv_send_wr* wrs, const size_t wrs_length) noexcept {
        if (wrs_length == 1) {
            return true;
        }

        if (offset_marker == wrs_length - 1) {
            return true;
        }

        auto* curr_wr = wrs;
        while (true) {
            if (curr_wr == bad) {
                return true;
            }

            if (not curr_wr) {
                return false;
            }

            curr_wr = curr_wr->next;
            ++offset_marker;
        }

        return false;
    }

    [[gnu::always_inline]] size_t max_inline_size() noexcept {
        return _meta_pool->element_size() - inline_data_header_size;
    }

    [[gnu::always_inline]] size_t max_remote_info_size() noexcept {
        auto rm_info_size = _meta_pool->element_size() - metadata_header_size;
        auto size_f = static_cast<double>(rm_info_size) / sizeof(remote_info);

        return static_cast<size_t>(std::floor(size_f));
    }

    [[gnu::always_inline]] size_t metadata_size() noexcept {
        return sizeof(remote_info) * _data_count + metadata_header_size;
    }

    bool has_enough_memory() noexcept {
        if (not _is_inlined) {
            auto ret = _data_pool and _data_count <= _data_pool->size();
            if (_meta_pool) {
                ret &= _meta_count <= _meta_pool->size();
            }

            return ret;
        }

        return _meta_pool and _meta_count <= _meta_pool->size();
    }

    bool exceeds_pool_capacity() const noexcept {
        if (_data_pool and _data_count > _data_pool->capacity()) {
            return true;
        }
        if (_meta_pool and _meta_count > _meta_pool->capacity()) {
            return true;
        }
        return false;
    }

    void release_allocated() noexcept {
        if (_meta_pool) {
            for (auto* mr : _metas) {
                if (mr) {
                    _meta_pool->put(mr);
                }
            }
        }
        if (_data_pool) {
            for (auto* mr : _datas) {
                if (mr) {
                    _data_pool->put(mr);
                }
            }
        }
        _metas.clear();
        _datas.clear();
        _head_tag = nullptr;
        _tail_tag = nullptr;
        _data_head = nullptr;
        _prepared = false;
    }

    [[gnu::always_inline]] void fill_rdma_read_tag(const rdma_read_tag_type* tag_val) noexcept {
        *_head_tag = *tag_val;
        *_tail_tag = *tag_val;
    }

    void set_rdma_read_tag() noexcept {
        _head_tag = reinterpret_cast<rdma_read_tag_type*>(_datas[0]->mr->addr);
        _data_head = reinterpret_cast<char*>(_head_tag + 1);

        if (_data_count == 1) {
            _tail_tag = _head_tag + _serialized_size + _rdma_read_tag_length;

            SPDK_DEBUGLOG(
              msg,
              "_head_tag: %p, _data_head: %p, _tail_tag: %p, _serialized_size: %lu\n",
              _head_tag, _data_head, _tail_tag, _serialized_size);
        } else {
            auto* last_chunk_addr = reinterpret_cast<char*>(_datas[_data_count - 1]->mr->addr);
            auto last_chunk_need_size =
              _serialized_size + _rdma_read_tag_length - (_data_count - 1) * _data_pool->element_size(); // _rdma_read_tag_length for head_tag
            _tail_tag = reinterpret_cast<rdma_read_tag_type*>(last_chunk_addr + last_chunk_need_size);

            SPDK_DEBUGLOG(
              msg,
              "_head_tag: %p, _data_head: %p, _tail_tag: %p, last_chunk_need_size: %lu, _serialized_size: %lu\n",
              _head_tag, _data_head, _tail_tag, last_chunk_need_size, _serialized_size);
        }
    }

    void init_inlined_metadata() noexcept {
        auto* m = reinterpret_cast<inline_data*>(_metas[0]->mr->addr);
        m->is_inlined = _inline_tag;
        m->io_length = static_cast<uint32_t>(_transport_size);
        m->correlation_index = _correlation_index;
    }

    [[gnu::always_inline]] void init_metadata(
      metadata* m,
      const uint32_t io_count,
      const uint32_t io_length,
      const uint32_t corr_idx,
      const uint32_t serial_no) noexcept {
        m->is_inlined = _no_inline_tag;
        m->io_count = io_count;
        m->io_length = io_length;
        m->correlation_index = corr_idx;
        m->serial_no = serial_no;
        m->metadata_count = _meta_count;
        SPDK_DEBUGLOG(
          msg,
          "io_count: %d, io_length: %d, corr_idx: %d, serial_no: %d\n",
          io_count, io_length, corr_idx, serial_no);
    }

    [[gnu::always_inline]] void init_metadata(metadata* m) noexcept {
        init_metadata(
          m,
          static_cast<uint32_t>(_data_count),
          static_cast<uint32_t>(_transport_size),
          _correlation_index, 0);
    }

    alloc_class effective_class() const noexcept {
        if (_alloc_class == alloc_class::priority) {
            return alloc_class::priority;
        }
        return _is_inlined ? alloc_class::reserved : alloc_class::bulk;
    }

    bool take_chunks(
      std::shared_ptr<memory_pool<::ibv_send_wr>> pool,
      const size_t n,
      const alloc_class cls,
      std::vector<memory_pool<::ibv_send_wr>::net_context*>& out) {
        out.assign(n, nullptr);
        if (n == 0) {
            return true;
        }
        if (not pool) {
            return false;
        }
        auto got = pool->try_get(n, cls);
        if (not got) {
            return false;
        }
        for (size_t i{0}; i < n; ++i) {
            out[i] = got[i];
        }
        return true;
    }

    bool alloc_reply_data() {
        release_allocated();
        const auto cls =
          _data_count <= 1 ? alloc_class::reserved : alloc_class::bulk;
        if (not take_chunks(_data_pool, _data_count, cls, _datas)) {
            release_allocated();
            return false;
        }
        set_rdma_read_tag();
        fill_rdma_read_tag(&_un_complete_tag);
        _prepared = true;
        SPDK_DEBUGLOG(msg, "_head_tag is %p, _tail_tag is %p\n", _head_tag, _tail_tag);
        return true;
    }

    bool alloc_request_data() {
        release_allocated();
        const auto cls = effective_class();
        if (_is_inlined) {
            if (not take_chunks(_meta_pool, 1, cls, _metas)) {
                release_allocated();
                return false;
            }
            init_inlined_metadata();
            _prepared = true;
            return true;
        }

        if (not take_chunks(_meta_pool, _meta_count, cls, _metas)) {
            release_allocated();
            return false;
        }
        if (not take_chunks(_data_pool, _data_count, cls, _datas)) {
            release_allocated();
            return false;
        }

        set_rdma_read_tag();
        fill_rdma_read_tag(&_un_complete_tag);
        SPDK_DEBUGLOG(msg, "_head_tag is %p, _tail_tag is %p\n", _head_tag, _tail_tag);

        if (_meta_count == 1) {
            auto* meta_ptr = reinterpret_cast<metadata*>(_metas[0]->mr->addr);
            auto* rm_info = reinterpret_cast<remote_info*>(
              reinterpret_cast<char*>(meta_ptr) + metadata_header_size);
            for (size_t i{0}; i < _data_count; ++i) {
                rm_info[i].raddr = _datas[i]->mr->addr;
                rm_info[i].rkey = _datas[i]->mr->rkey;
            }

            init_metadata(meta_ptr);
        } else {
            size_t data_counter{0};
            size_t rm_info_counter{0};
            metadata* meta_ptr{nullptr};
            remote_info* rm_info{nullptr};
            auto max_rm_info = max_remote_info_size();

            for (size_t meta_counter{0}; meta_counter < _meta_count - 1; ++meta_counter) {
                meta_ptr = reinterpret_cast<metadata*>(_metas[meta_counter]->mr->addr);
                rm_info = reinterpret_cast<remote_info*>(
                  reinterpret_cast<char*>(meta_ptr) + metadata_header_size);
                for (rm_info_counter = 0; rm_info_counter < max_rm_info; ++rm_info_counter) {
                    rm_info[rm_info_counter].raddr = _datas[data_counter]->mr->addr;
                    rm_info[rm_info_counter].rkey = _datas[data_counter]->mr->rkey;
                    ++data_counter;
                }
                init_metadata(
                  meta_ptr,
                  max_rm_info,
                  max_rm_info * _data_chunk_size,
                  _correlation_index, meta_counter);

                SPDK_DEBUGLOG(
                  msg,
                  "correlation index: %d, metadata{io_count: %d, io_length: %d, rm_info_counter: %ld, serial_no: %d}, "
                  "max_rm_info: %ld, _metas.size(): %ld, meta_counter: %ld\n",
                  _correlation_index, meta_ptr->io_count, meta_ptr->io_length, rm_info_counter + 1, meta_ptr->serial_no,
                  max_rm_info, _metas.size(), meta_counter);
            }

            // the last meta chunk
            meta_ptr = reinterpret_cast<metadata*>(_metas[_meta_count - 1]->mr->addr);
            rm_info = reinterpret_cast<remote_info*>(
              reinterpret_cast<char*>(meta_ptr) + metadata_header_size);
            rm_info_counter = 0;
            for (size_t i{data_counter}; i < _data_count; ++i) {
                rm_info[rm_info_counter].raddr = _datas[i]->mr->addr;
                rm_info[rm_info_counter].rkey = _datas[i]->mr->rkey;
                ++rm_info_counter;
            }
            init_metadata(
              meta_ptr,
              _data_count - data_counter,
              _transport_size - (_meta_count - 1) * _data_chunk_size * max_rm_info,
              _correlation_index,
              _meta_count - 1);

            SPDK_DEBUGLOG(
              msg,
              "_transport_size: %ld, _meta_count: %ld, _data_count: %ld, data_counter: %ld\n",
              _transport_size, _meta_count, _data_count, data_counter);

            SPDK_DEBUGLOG(
              msg,
              "correlation index: %d, metadata{io_count: %d, io_length: %d, rm_info_counter: %ld, serial_no: %d}, "
              "max_rm_info: %ld, _metas.size(): %ld, meta_counter: %ld\n",
              _correlation_index, meta_ptr->io_count, meta_ptr->io_length, rm_info_counter + 1, meta_ptr->serial_no,
              max_rm_info, _metas.size(), _meta_count - 1);
        }
        _prepared = true;
        return true;
    }

    void init() noexcept {
        if (_transport_size <= max_inline_size()) {
            _meta_count = 1;
            _is_inlined = true;
            SPDK_DEBUGLOG(msg, "created inlined transport\n");
        } else {
            if (_transport_size <= _data_chunk_size) {
                _data_count = 1;
                _meta_count = 1;
            } else {
                auto trans_size_f = static_cast<double>(_transport_size);
                auto io_count_f = std::ceil(trans_size_f / _data_chunk_size);
                _data_count = static_cast<size_t>(io_count_f);
                auto max_rm_info = static_cast<double>(max_remote_info_size());
                auto num_meta_f = static_cast<double>(_data_count) / max_rm_info;

                SPDK_DEBUGLOG(
                  msg,
                  "metadata_size(): %ld, _data_count: %ld, num_meta_f: %f\n",
                  metadata_size(), _data_count, num_meta_f);

                if (fabs(num_meta_f - double{0.0}) <= std::numeric_limits<double>::epsilon()) {
                    _meta_count = 1;
                } else {
                    _meta_count = static_cast<size_t>(std::ceil(num_meta_f));
                }
            }
        }

        SPDK_DEBUGLOG(
          msg,
          "transport size: %ld, _is_inlined: %d, _meta_count: %ld, _data_count: %ld\n",
          _transport_size, _is_inlined, _meta_count, _data_count);

        if (exceeds_pool_capacity()) {
            _too_large = true;
            _prepared = false;
            SPDK_NOTICELOG(
              "RPC too large for transport pool, need meta element is %ld, capacity %ld; need data element is %ld, capacity %ld\n",
              _meta_count, _meta_pool ? _meta_pool->capacity() : 0,
              _data_count, _data_pool ? _data_pool->capacity() : 0);
            return;
        }

        if (not alloc_request_data()) {
            _prepared = false;
            SPDK_NOTICELOG(
              "Not enough memory for transport, need meta element is %ld, but has %ld; need data element is %ld, but has %ld\n",
              _meta_count, _meta_pool ? _meta_pool->size() : 0,
              _data_count, _data_pool ? _data_pool->size() : 0);
        }
    }

private:

    bool _is_inlined{false};
    correlation_index_type _correlation_index{0};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};
    size_t _transport_size{0};
    size_t _serialized_size{0};
    size_t _meta_count{0};
    size_t _data_count{0};
    size_t _data_chunk_size{0};

    std::vector<memory_pool<::ibv_send_wr>::net_context*> _metas;
    std::vector<memory_pool<::ibv_send_wr>::net_context*> _datas;

    std::unique_ptr<char[]> _serialized_buf{nullptr};
    std::unique_ptr<remote_info[]> _rm_infos{nullptr};
    uint32_t _last_rm_info_index{0};
    bool _is_metadata_complete{false};
    bool _prepared{false};
    bool _too_large{false};
    alloc_class _alloc_class{alloc_class::reserved};
    uint64_t _seen_serials{0};
    uint32_t _expected_serials{0};

    rdma_read_tag_type* _head_tag{nullptr};
    rdma_read_tag_type* _tail_tag{nullptr};
    char* _data_head{nullptr};
    size_t _meta_start_offset{0};
    size_t _data_start_offset{0};

    static constexpr uint32_t _magic_no{0xffff};
};

} // namespace rdma
} // namespace msg
