#include "spdk_buffer.h"
#include "buffer_pool.h"

buffer_list make_buffer_list(size_t n) {
  buffer_list bl;
  for (size_t i = 0; i < n; i++) {
    bl.append_buffer(buffer_pool_get());
  }
  return bl;
};

void free_buffer_list(buffer_list& bl) {
  while(!bl.empty()) {
    auto sbuf = bl.front();
    buffer_pool_put(sbuf);
    bl.pop_front();
  }
};