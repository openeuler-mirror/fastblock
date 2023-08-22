#pragma once

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
    spdk_buffer(char* buf, size_t sz) : buf(buf), size(sz), used(0) {}
    spdk_buffer() noexcept = default;

    size_t append(const char* in, size_t len) {
      if (len > remain()) {
        SPDK_NOTICELOG("spdk_buffer no memory, size: %lu, len:%lu \n", size, len);
        return -ENOMEM;
      }
      const size_t sz = std::min(len, remain());
      std::memcpy(get_append(), in, sz);
      used += sz;
      return sz;
    }

    size_t append(const std::string& str) {
      return append(str.c_str(), str.size());
    }


    char* get_buf() { return buf; }

    char* get_append() { return buf + used; }

    size_t inc(size_t n) { 
      if (n > remain()) { 
        SPDK_NOTICELOG("spdk_buffer no memory. need %lu while have %lu\n", n, size);
        return -ENOMEM;
      }
      used += n;
      return n; 
    } 

    void reset() { used = 0; }

    size_t len() const { return size; }
    size_t used_size() { return used; }
    size_t remain() { return size > used ? size - used : 0; }

private:
    char* buf;
    size_t size;
    size_t used;
};


// 偷懒的做法，不想封装迭代器，直接继承list
class buffer_list : public std::list<spdk_buffer> {
public:

  void append_buffer(buffer_list& bl) {
    size_t byte = bl.bytes();
    splice(end(), bl, bl.begin(), bl.end());
    total += byte;
  }

  void append_buffer(buffer_list&& bl) {
    size_t bytes = bl.bytes();
    splice(end(), std::move(bl));
    total += bytes;
  }

  void append_buffer(const spdk_buffer& sbuf) {
    push_back(sbuf);
    total += sbuf.len();
  }

  void prepend_buffer(const spdk_buffer& sbuf) {
    push_front(sbuf);
    total += sbuf.len();
  }

  void trim_front() {
    size_t len = front().len();
    pop_front();
    total -= len;
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

    while (discard + it->len() <= pos && it != end()) {
      discard += it->len();
      // std::cout << "discard:" << discard << std::endl;
      it++;
    }

    if (discard < pos && it != end()) {
      iovec iov;
      iov.iov_base = it->get_buf() + (pos - discard);
      size_t left_in_buf = it->len() - (pos - discard);
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
      iov.iov_len = std::min(it->len(), still_need);     
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

  size_t bytes() { return total; }

private:
  size_t total{0};
};

buffer_list make_buffer_list(size_t n);

void free_buffer_list(buffer_list& bl);