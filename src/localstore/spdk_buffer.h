#pragma once

#include <spdk/env.h>
#include <spdk/log.h>

#include <string>
#include <cstring>
#include <list>
#include <vector>

#include <errno.h>
#include <sys/uio.h>


using iovecs = std::vector<::iovec>;

class spdk_buffer {
public:
    spdk_buffer(char* buf, size_t sz) : buf(buf), size(sz), used(0) {}

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

  // 需要用户一直持有std::vector，保证数组所在内存不会析构
  // 现在默认每段内存都是 4K 长，没有检查过，如果不是4k的可能会出现问题
  iovecs to_iovec(size_t pos, size_t len) {
    iovecs iovs;
    auto it = begin();
    size_t discard = 0, chosen = 0;
    while (discard < pos && it != end()) {
      discard += it->len();
      it++;
    }

    while (chosen < len && it != end()) {
      iovec iov;
      iov.iov_base = it->get_buf();
      iov.iov_len = it->len();     
      iovs.push_back(iov);

      chosen += it->len();
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