#include "spdk_buffer.h"

buffer_list make_buffer_list(size_t n) {
  buffer_list bl;
  for (size_t i = 0; i < n; i++) {
    char* c = (char*)spdk_malloc(4096,
                  0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
                  SPDK_MALLOC_DMA);
  // SPDK_NOTICELOG("buffer list addr:%p\n", c);
    bl.append_buffer(spdk_buffer(c, 4096));
  }
  return bl;
};

void free_buffer_list(buffer_list& bl) {
  while(!bl.empty()) {
    auto sbuf = bl.front();
  // SPDK_NOTICELOG("free buffer list addr:%p\n", sbuf.get_buf());
    spdk_free(sbuf.get_buf());
    bl.pop_front();
  }
};