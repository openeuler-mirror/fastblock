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

#include "msg/rdma/memory_pool.h"
#include "msg/rdma/types.h"

#include <google/protobuf/message.h>

#include <spdk/log.h>

#include <cmath>
#include <cstring>
#include <numeric>
#include <memory>
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

    static constexpr size_t metadata_header_size{sizeof(metadata)};

public:

    transport_data() = delete;

    transport_data(
      const correlation_index_type correlation_index,
      const size_t serialized_size,
      std::shared_ptr<memory_pool<::ibv_send_wr>> meta_pool,
      std::shared_ptr<memory_pool<::ibv_send_wr>> data_pool)
      : _correlation_index{correlation_index}
      , _meta_pool{meta_pool}
      , _data_pool{data_pool}
      , _transport_size{serialized_size + 2 * _rdma_read_tag_length}
      , _serialized_size{serialized_size}
      , _data_chunk_size{_data_pool->element_size()}
      , _metas(0)
      , _datas(0) {
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
        if (_meta_pool) {
            for (auto* mr : _metas) {
                _meta_pool->put(mr);
            }
        }

        for (auto* mr : _datas) {
            _data_pool->put(mr);
        }

        SPDK_DEBUGLOG(
          msg,
          "data pool size: %lu, meta pool size: %ld\n",
          _data_pool->size(),
          _meta_pool ? _meta_pool->size() : -1);
    }

public:

    static bool is_inlined(memory_pool<::ibv_recv_wr>::net_context* ctx) noexcept {
        auto* data = reinterpret_cast<inline_data*>(ctx->mr->addr);
        return static_cast<bool>(data->is_inlined);
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

    bool is_ready() noexcept {
        bool is_ready{false};

        if (not _is_inlined) {
            is_ready = (_metas.size() == _meta_count) and (_datas.size() == _data_count);
        } else {
            is_ready = not _metas.empty();
        }

        if (is_ready) {
            return true;
        }

        if (not has_enough_memory()) {
            SPDK_DEBUGLOG(
              msg,
              "no enough memory, _data_count: %ld, _data_pool size: %ld, _meta_count: %ld, _meta_pool size: %ld\n",
              _data_count, _data_pool->size(), _meta_count,
              _meta_pool ? _meta_pool->size() : -1);
            return false;
        }

        alloc_request_data();

        return true;
    }

    inline char* serialized_buf() noexcept {
        return _serialized_buf.get();
    }

    inline size_t serilaized_size() noexcept {
        return _transport_size;
    }

    inline auto correlation_index() noexcept {
        return _correlation_index;
    }

    inline auto meatdata_capacity() noexcept {
        return _meta_count;
    }

    inline auto data_capacity() noexcept {
        return _data_count;
    }

    inline bool is_metadata_complete() noexcept {
        return _is_metadata_complete;
    }

    bool is_rdma_read_complete() noexcept {
        return *_head_tag == _complete_tag and *_tail_tag == _complete_tag;
    }

    request_meta* read_request_meta() noexcept {
        return reinterpret_cast<request_meta*>(_data_head);
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
        for (uint32_t i{0}; i < data->io_count; ++i) {
            rm_infos[_last_rm_info_index].raddr = rm_info[i].raddr;
            rm_infos[_last_rm_info_index].rkey = rm_info[i].rkey;
            ++_last_rm_info_index;
        }

        SPDK_DEBUGLOG(
          msg,
          "_data_count: %ld, _transport_size: %ld, data->io_count: %d, "
          "data->io_length: %d, data->is_lined: %d, data->correlation_index: %d, "
          "data->metadata_count: %d, data->serial_no: %d\n",
          _data_count, _transport_size, data->io_count, data->io_length,
          data->is_inlined, data->correlation_index, data->metadata_count,
          data->serial_no);

        if (data->serial_no + 1 == data->metadata_count) {
            _is_metadata_complete = true;
            _serialized_size = _transport_size - 2 * _rdma_read_tag_length;
            if (has_enough_memory()) {
                alloc_reply_data();
            }
        }
    }

    void serialize_data(void* meta_src, const size_t meta_size, const google::protobuf::Message* request) {
        std::ptrdiff_t off{0};
        if (meta_src) {
            off = meta_size;
        }

        auto ser_size = request->ByteSizeLong();
        if (_is_inlined) {
            auto* data = reinterpret_cast<inline_data*>(_metas[0]->mr->addr);
            auto* content = reinterpret_cast<char*>(data) + inline_data_header_size;
            if (meta_src) {
                std::memcpy(content, meta_src, meta_size);
            }
            request->SerializeToArray(content + off, ser_size);
            data->io_length = static_cast<uint32_t>(ser_size);
            SPDK_DEBUGLOG(msg, "serialized size is %lu + %d\n", meta_size, data->io_length);

            return;
        }

        _serialized_buf = std::make_unique<char[]>(_serialized_size);
        if (meta_src) {
            std::memcpy(_serialized_buf.get(), meta_src, meta_size);
        }
        request->SerializeToArray(_serialized_buf.get() + off, ser_size);

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

    bool unserialize_data(google::protobuf::Message* m, const std::ptrdiff_t start_off = 0) {
        if (_data_count == 1) {
            return m->ParseFromArray(
              _data_head + start_off,
              _serialized_size - start_off);
        }

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
            cpy_len = std::min(_data_chunk_size - cpy_off, _serialized_size - start_off - copied);
        }

        // handle last chunk
        auto last_cpy_length = _serialized_size - copied;
        std::memcpy(
          _serialized_buf.get() + copied,
          _datas[_data_count - 1]->mr->addr,
          last_cpy_length);

        return m->ParseFromArray(_serialized_buf.get(), _serialized_size - start_off);
    }

    ::ibv_send_wr* make_read_request(std::function<uint64_t()> wr_id_gen, const bool should_signal = false) noexcept {
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
            _metas[_metas.size() - 1]->wr.next = nullptr;
        }

        return &(_metas[0]->wr);
    }

private:

    size_t max_inline_size() noexcept {
        return _meta_pool->element_size() - inline_data_header_size;
    }

    size_t max_remote_info_size() noexcept {
        auto rm_info_size = _meta_pool->element_size() - metadata_header_size;
        auto size_f = static_cast<double>(rm_info_size) / sizeof(remote_info);

        return static_cast<size_t>(std::floor(size_f));
    }

    size_t metadata_size() noexcept {
        return sizeof(remote_info) * _data_count + metadata_header_size;
    }

    bool has_enough_memory() noexcept {
        if (not _is_inlined) {
            auto ret = _data_count <= _data_pool->size();
            if (_meta_pool) {
                ret &= _meta_count <= _meta_pool->size();
            }

            return ret;
        }

        return _meta_count <= _meta_pool->size();
    }

    void fill_rdma_read_tag(const rdma_read_tag_type* tag_val) noexcept {
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
        _metas[0] = _meta_pool->get();
        auto* m = reinterpret_cast<inline_data*>(_metas[0]->mr->addr);
        m->is_inlined = _inline_tag;
        m->io_length = static_cast<uint32_t>(_transport_size);
        m->correlation_index = _correlation_index;
    }

    inline void init_metadata(
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

    inline void init_metadata(metadata* m) noexcept {
        init_metadata(
          m,
          static_cast<uint32_t>(_data_count),
          static_cast<uint32_t>(_transport_size),
          _correlation_index, 0);
    }

    void alloc_reply_data() {
        _datas.resize(_data_count);
        for (size_t i{0}; i < _data_count; ++i) {
            _datas[i] = _data_pool->get();
        }
        set_rdma_read_tag();
        fill_rdma_read_tag(&_un_complete_tag);
        SPDK_DEBUGLOG(msg, "_head_tag is %p, _tail_tag is %p\n", _head_tag, _tail_tag);
    }

    void alloc_request_data() {
        if (_is_inlined) {
            _metas.resize(1);
            init_inlined_metadata();

            return;
        }

        _metas.resize(_meta_count);
        _datas.resize(_data_count);

        for (size_t i{0}; i < _meta_count; ++i) {
            _metas[i] = _meta_pool->get();
        }

        for (size_t i{0}; i < _data_count; ++i) {
            _datas[i] = _data_pool->get();
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
                double num_meta_f = static_cast<double>(metadata_size()) / _meta_pool->element_size();

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

        if (not has_enough_memory()) {
            SPDK_NOTICELOG(
              "Not enough memory for transport, need meta element is %ld, but has %ld; need data element is %ld, but has %ld\n",
              _meta_count, _meta_pool->size(), _data_count, _data_pool->size());
            return;
        }

        alloc_request_data();
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

    rdma_read_tag_type* _head_tag{nullptr};
    rdma_read_tag_type* _tail_tag{nullptr};
    char* _data_head{nullptr};

    static constexpr uint32_t _magic_no{0xffff};
};

} // namespace rdma
} // namespace msg
