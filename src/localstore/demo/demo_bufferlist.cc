#include <iostream>
#include "localstore/spdk_buffer.h"

int main() {
    buffer_list bl;
    char* buf;
    buf = (char*)malloc(4096);
    bl.append_buffer(spdk_buffer(buf, 4096));
    std::cout << (void*)buf << std::endl;

    buf = (char*)malloc(4096 * 2);
    bl.append_buffer(spdk_buffer(buf, 4096 * 2));
    std::cout << (void*)buf << std::endl;

    buf = (char*)malloc(4096 * 3);
    bl.append_buffer(spdk_buffer(buf, 4096 * 3));
    std::cout << (void*)buf << std::endl;

    buf = (char*)malloc(4096 * 4);
    bl.append_buffer(spdk_buffer(buf, 4096 * 4));
    std::cout << (void*)buf << std::endl;

    auto iovecs = bl.to_iovec(8192, 12288);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;

    iovecs = bl.to_iovec(4096, 24576);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;

    iovecs = bl.to_iovec(24576, 24576);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;

    buffer_list bl2;
    char* buf2 = (char*)malloc(204800);
    bl2.append_buffer(spdk_buffer(buf, 204800));
    iovecs = bl2.to_iovec(4096, 40960);
    for (auto iov : iovecs) {
        std::cout << iov.iov_base << " " << iov.iov_len << std::endl;
    }
    std::cout << std::endl;
}