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

#include "utils/varint.h"

#include <spdk/env.h>
#include <spdk/log.h>

#include <string>
#include <cstring>
#include <list>
#include <vector>
#include <iostream>
#include <errno.h>
#include <sys/uio.h>


using iovecs = std::vector<::iovec>;

class spdk_buffer {
public:
    spdk_buffer(char* buf, size_t sz) : _buf(buf), _size(sz), _used(0) {}
    spdk_buffer() noexcept = default;

    size_t append(const char* in, size_t len) {
      size_t sz = std::min(len, remain());
      std::memcpy(get_append(), in, sz);
      _used += sz;
      return sz;
    }

    size_t append(const std::string& str) {
      return append(str.c_str(), str.size());
    }


    char* get_buf() { return _buf; }

    char* get_append() { return _buf + _used; }

    size_t inc(size_t n) {
      size_t sz = std::min(n, remain());
      _used += sz;
      return sz;
    }

    void reset() { _used = 0; }

    void set_used(size_t u) { _used = u > _size ? _size : u; }

    size_t size() const { return _size; }
    size_t used() { return _used; }
    size_t remain() { return _size > _used ? _size - _used : 0; }

private:
    char*  _buf;
    size_t _size;
    size_t _used;
};


// 偷懒的做法，不想封装迭代器，直接继承list
class buffer_list : private std::list<spdk_buffer> {
public:

  void append_buffer(buffer_list& bl) noexcept {
    size_t byte = bl.bytes();
    splice(end(), bl);
    total += byte;
  }

  void append_buffer(buffer_list&& bl) noexcept {
    size_t bytes = bl.bytes();
    splice(end(), std::move(bl));
    total += bytes;
  }

  void append_buffer(const spdk_buffer& sbuf) {
    push_back(sbuf);
    total += sbuf.size();
  }

  void prepend_buffer(const spdk_buffer& sbuf) {
    push_front(sbuf);
    total += sbuf.size();
  }

  void trim_front() noexcept {
    size_t len = front().size();
    pop_front();
    total -= len;
  }

  spdk_buffer pop_front() noexcept {
    spdk_buffer ret = front();
    size_t len = ret.size();
    std::list<spdk_buffer>::pop_front();
    total -= len;
    return ret;
  }

  buffer_list pop_front_list(size_t len) {
    buffer_list bl;
    while(len--) {
      bl.append_buffer(pop_front());
    }
    return bl;
  }

  // 1. 需要用户要保证pos和len长度位置有效
  // 2. 需要用户一直持有返回的std::vector，保证数组所在内存不会析构
  // 3. 需要地址4k对齐
  iovecs to_iovec(size_t pos, size_t len) {
    // std::cout << "pos:" << pos << " len:" << len << " size:" << bytes() << std::endl;
    if (pos + len > bytes()) {
        return iovecs{};
    }

    iovecs iovs;
    auto it = begin();
    size_t discard = 0, chosen = 0;

    while (discard + it->size() <= pos && it != end()) {
      discard += it->size();
      // std::cout << "discard:" << discard << std::endl;
      it++;
    }

    if (discard < pos && it != end()) {
      iovec iov;
      iov.iov_base = it->get_buf() + (pos - discard);
      size_t left_in_buf = it->size() - (pos - discard);
      iov.iov_len = std::min(left_in_buf, len);
      iovs.push_back(iov);
      // std::cout << "discard:" << discard << " iov_len:" << iov.iov_len << std::endl;

      chosen += iov.iov_len;
      it++;
    }

    while (chosen < len && it != end()) {
      iovec iov;
      iov.iov_base = it->get_buf();
      size_t still_need = len - chosen;
      iov.iov_len = std::min(it->size(), still_need);
      iovs.push_back(iov);


      chosen += iov.iov_len;
      // std::cout << "chosen:" << chosen << " iov_len:" << iov.iov_len << std::endl;
      it++;
    }

    return iovs;
  }

  iovecs to_iovec() {
    return to_iovec(0, total);
  }

  size_t bytes() noexcept { return total; }

  using base = std::list<spdk_buffer>;
  using base::iterator;
  using base::const_iterator;
  using base::begin;
  using base::end;

private:
  size_t total{0};

private:
  friend buffer_list make_buffer_list(size_t n);
  friend void free_buffer_list(buffer_list& bl);
};

/**
 * 用来帮助序列化数据到buffer list的类。
 *
 * 缺陷是还没有考虑过buffer_list空间不足怎么办。另外返回的bool值暂时也没有用到。
 */
class buffer_list_encoder {
public:
  bool put(const std::string& value) {
      return put(value.size()) && put(value.c_str(), value.size());
  }
  bool put(uint64_t value) {
      if (this->remain() < sizeof(uint64_t)) { return false; }

      if (_itor->remain() < sizeof(uint64_t)) {
          auto prev = _itor++;
          // ++_itor;
          size_t prev_bytes = prev->remain();
          _itor->reset();

          encode_fixed64(prev->get_append(), prev_bytes, _itor->get_append(), value);

          _itor->inc(sizeof(uint64_t) - prev_bytes);
          prev->inc(prev_bytes);
          _used += sizeof(uint64_t);
          return true;
      }

      encode_fixed64(_itor->get_append(), value);
      _itor->inc(sizeof(uint64_t));
      _used += sizeof(uint64_t);
      return true;
  }

  bool put(const char* ptr, size_t len) {
      if (this->remain() < len) { return false; }

      if (_itor->remain() < len) {
          auto prev = _itor++;
          size_t prev_bytes = prev->remain();
          _itor->reset();

          prev->append(ptr, prev_bytes);
          _itor->append(ptr + prev_bytes, len - prev_bytes);
          _used += len;
          return true;
      }
      _itor->append(ptr, len);
      _used += len;
      return true;
  }

  bool get(std::string& str) {
      uint64_t size;
      if (!get(size)) {
          return false;
      };

      str.resize(size);
      // std::cout << "get str_size:" << size << std::endl;
      if (!get(str.data(), size)) {
          return false;
      }
      return true;
  }

  bool get(uint64_t& value) {
      if (this->remain() < sizeof(uint64_t)) { return false; }

      if (_itor->remain() < sizeof(uint64_t)) {
          auto prev = _itor++;
          size_t prev_bytes = prev->remain();
          _itor->reset();

          value = decode_fixed64(prev->get_append(), prev_bytes, _itor->get_append());
          // std::cout << "1 prev_size:" << prev->size()
          //         << " used:" << prev->used()
          //         << " remain:" << prev->remain()
          //         << " value:" << value
          //         << std::endl;
          // SPDK_NOTICELOG("1 size:%lu used:%lu remain:%lu value:%lu \n",
          //     prev->size(), prev->used(), prev->remain(), value);
          _itor->inc(sizeof(uint64_t) - prev_bytes);
          prev->inc(prev_bytes);

          _used += sizeof(uint64_t);
          return true;
      }

      value = decode_fixed64(_itor->get_append());
      // std::cout << "2 value:" << value
      //             << std::endl;
      // SPDK_NOTICELOG("2 value:%lu used:%lu\n", value, _used);
      // std::string str(_itor->get_append(), 8);
      // printf("\n"); for (auto& c : str) { printf("%x ", c); } printf("\n");
      _itor->inc(sizeof(uint64_t));
      _used += sizeof(uint64_t);
      return true;
  }

  bool get(char* ptr, size_t len) {
      if (this->remain() < len) { return false; }

      if (_itor->remain() < len) {
          auto prev = _itor++;
          size_t prev_bytes = prev->remain();
          _itor->reset();

          // SPDK_NOTICELOG("1 str size:%lu used:%lu remain:%lu len:%lu \n",
          //     prev->size(), prev->used(), prev->remain(), len);
          memcpy(ptr, prev->get_append(), prev_bytes);
          memcpy(ptr + prev_bytes, _itor->get_append(), len - prev_bytes);
          prev->inc(prev_bytes);
          _itor->inc(len - prev_bytes);

          _used += len;
          return true;
      }


      memcpy(ptr, _itor->get_append(), len);
      // std::string str(_itor->get_append(), len);
      // std::cout << "get str:" << str.c_str() << " used:" << _used << " len:" << len << std::endl;
      // SPDK_NOTICELOG("2 str:%s used:%lu\n", str.c_str(), _used);
      _itor->inc(len);
      _used += len;
      return true;
  }

  size_t bytes() noexcept { return _buffer_list.bytes(); }
  size_t used() { return _used; }
  size_t remain() { return _buffer_list.bytes() - _used; }

public:
  buffer_list_encoder(buffer_list& bl) : _buffer_list(bl) , _itor(bl.begin()) {}
private:
  buffer_list& _buffer_list;
  buffer_list::iterator _itor;
  uint64_t _used = 0;
};

buffer_list make_buffer_list(size_t n);

void free_buffer_list(buffer_list& bl);