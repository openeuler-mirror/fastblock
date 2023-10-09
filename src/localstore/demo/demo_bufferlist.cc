#include <iostream>
#include <string>
#include "localstore/spdk_buffer.h"

inline std::string rand_str(int len) {
  std::string str(len, '0');
  static char t[63] = {"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
  for(int i = 0; i < len; i++) {
    str[i] = t[rand() % 62];
  }
  return str;
}

int main() {
    srand(time(0));


    /**
     * test case 1: 测试buffer list的to_iovec()，跨多个4k buffer，切分成多个slice
     */
    std::cout << std::endl << "test case 1:" << std::endl;
    buffer_list bl;
    char* buf;
    size_t len;

    len = 4096;
    buf = (char*)malloc(len);
    bl.append_buffer(spdk_buffer(buf, len));
    std::cout << "buf1:" << (void*)buf << " len:" << len << std::endl;

    len = 4096 * 2;
    buf = (char*)malloc(len);
    bl.append_buffer(spdk_buffer(buf, len));
    std::cout << "buf2:" << (void*)buf << " len:" << len << std::endl;

    len = 4096 * 3;
    buf = (char*)malloc(len);
    bl.append_buffer(spdk_buffer(buf, len));
    std::cout << "buf3:" << (void*)buf << " len:" << len << std::endl;

    len = 4096 * 4;
    buf = (char*)malloc(len);
    bl.append_buffer(spdk_buffer(buf, len));
    std::cout << "buf4:" << (void*)buf << " len:" << len << std::endl;

    std::cout << "bl.to_iovec(8192, 12288):" << std::endl;
    auto iovecs = bl.to_iovec(8192, 12288);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;

    std::cout << "bl.to_iovec(4096, 24576):" << std::endl;
    iovecs = bl.to_iovec(4096, 24576);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;

    std::cout << "bl.to_iovec(24576, 24576):" << std::endl;
    iovecs = bl.to_iovec(24576, 24576);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;


    /**
     * test case 2: 测试buffer list的to_iovec()，怎么从一个大块的buffer切出一小块slice
     */
    std::cout << std::endl << "test case 2: large buffer list" << std::endl;
    buffer_list bl2;
    len = 4096 * 50;
    char* buf2 = (char*)malloc(len);
    bl2.append_buffer(spdk_buffer(buf, len));
    iovecs = bl2.to_iovec(4096, 40960);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;


    /**
     * test case 3: 测试buffer list中，同一个buffer中的encode
     */
    std::cout << std::endl << "test case 3: buffer list encoder" << std::endl;
    buffer_list bl3;
    len = 4096;
    buf = (char*)malloc(len);
    bl3.append_buffer(spdk_buffer(buf, len));
    buf = (char*)malloc(len);
    bl3.append_buffer(spdk_buffer(buf, len));
    auto bl_encoder = buffer_list_encoder(bl3);

    uint64_t used = bl_encoder.used();
    std::cout << "bl_encoder.used:" << used << std::endl;
    bl_encoder.put(10);
    std::cout << "bl_encoder.used add:" << bl_encoder.used() - used << std::endl;
    used = bl_encoder.used();
    for (int i = 0; i < 10; ++i) {
        bl_encoder.put(std::string("key") + rand_str(6)); // 8 + 9 = 17
        std::cout << "bl_encoder.used add:" << bl_encoder.used() - used << std::endl;
        used = bl_encoder.used();

        bl_encoder.put(std::string("value") + rand_str(6)); // 8 + 11 = 19
        std::cout << "bl_encoder.used add:" << bl_encoder.used() - used << std::endl;
        used = bl_encoder.used();
    }

    // 清空used，准备读取
    for (auto& buf : bl3) { buf.reset(); }

    buffer_list_encoder bl_encoder2(bl3);
    uint64_t table_size = 0;
    used = bl_encoder2.used();
    std::cout << "bl_encoder2.used:" << used << std::endl;
    bl_encoder2.get(table_size);
    std::cout << "bl_encoder2.used add:" << bl_encoder2.used() - used << " table_size:" << table_size << std::endl;
    used = bl_encoder2.used();

    for (uint64_t i = 0; i < table_size; i++) {
        std::string key, value;
        bl_encoder2.get(key);
        std::cout << "bl_encoder2.used add:" << bl_encoder2.used() - used << " get:" << key << std::endl;
        used = bl_encoder2.used();

        bl_encoder2.get(value);
        std::cout << "bl_encoder2.used add:" << bl_encoder2.used() - used << " get:" << value << std::endl;
        used = bl_encoder2.used();
    }

    /**
     * test case 4: 测试buffer list中，跨buffer的encode
     */
    std::cout << std::endl << "test case 4: buffer list discontinuous encoder" << std::endl;
    for (auto& buf : bl3) { buf.reset(); }
    bl3.begin()->set_used(4093);

    buffer_list_encoder bl_encoder3(bl3);
    std::string put_str = std::string("key") + rand_str(6);
    std::cout << "put:" << put_str << std::endl;
    bl_encoder3.put(put_str);

    for (auto& buf : bl3) { buf.reset(); }
    bl3.begin()->set_used(4093);

    buffer_list_encoder bl_encoder4(bl3);
    std::string get_str;
    bl_encoder4.get(get_str);
    std::cout << "get:" << get_str << std::endl;

}