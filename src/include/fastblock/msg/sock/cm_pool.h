/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct spdk_sock;
struct spdk_thread;

namespace msg {
namespace sock {

class cm_pool {
public:
    using done_fn = std::function<void(::spdk_sock* sock, uint64_t generation)>;

    static cm_pool& instance();

    void ensure_started(int workers, uint32_t queue_depth);
    void stop();

    // Enqueue a connect. Returns false if the queue is full (cb is not invoked).
    bool submit(std::string ip, uint16_t port, uint64_t generation,
                ::spdk_thread* target, done_fn cb);

    void cancel(uint64_t generation);
    void forget(uint64_t generation);

private:
    cm_pool() = default;
};

} // namespace sock
} // namespace msg
