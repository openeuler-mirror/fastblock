#pragma once

#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <functional>

using sleep_complete = std::function<void (void *, int)>;

struct sleep_ctx {
    sleep_complete cb_fn;
    void* arg;
    struct spdk_poller** poller;
};

static int run_sleep_cb(void* arg) {
    auto ctx = (sleep_ctx*)arg;
    spdk_poller_unregister(ctx->poller);

    ctx->cb_fn(ctx->arg, 0);
    delete ctx;
    return 0;
}

inline void spdk_sleep(struct spdk_poller** poller, uint64_t us, sleep_complete cb_fn, void* arg) {
    sleep_ctx* ctx = new sleep_ctx{.cb_fn = cb_fn, .arg = arg, .poller = poller};
    *poller = SPDK_POLLER_REGISTER(run_sleep_cb, ctx, us);
}

