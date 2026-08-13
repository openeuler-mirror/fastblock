/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/**
 * SPDK reactor integration test for AOS.
 *
 * Requires: JSON conf creating a malloc bdev (see aos_malloc.json).
 *
 *   ./aos_spdk_test -c aos_malloc.json
 */

#include "pg_store.h"
#include "spdk_bdev_backend.h"

#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/thread.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using fastblock::aos::buffer_mode;
using fastblock::aos::pg_id;
using fastblock::aos::pg_store;
using fastblock::aos::spdk_bdev_backend;

const char* g_bdev_name = "AosMalloc0";

struct test_ctx {
  spdk_bdev_backend backend;
  std::unique_ptr<pg_store> store;
  int phase{0};
  int failures{0};
  std::vector<char> pattern;
  std::vector<char> readback;
};

void shutdown_app(test_ctx* ctx, int rc) {
  // Release bdev channel before subsystem teardown or stop can hang.
  if (ctx) {
    ctx->store.reset();
    ctx->backend.close();
  }
  spdk_app_stop(rc);
}

void fail(test_ctx* ctx, const char* msg) {
  SPDK_ERRLOG("aos_spdk_test FAIL: %s\n", msg);
  ++ctx->failures;
  shutdown_app(ctx, 1);
}

// Deferred after read completion: must not destroy pg_store inside finish_op's
// user callback (finish_op still calls pump_queue after cb returns).
void start_recover_phase(void* arg) {
  auto* ctx = static_cast<test_ctx*>(arg);
  ctx->store.reset();
  ctx->store = std::make_unique<pg_store>(&ctx->backend, pg_id{1, 1, 1},
                                          buffer_mode::spdk_dma);
  ctx->store->recover(
      [](void* a, int rerr) {
        auto* c = static_cast<test_ctx*>(a);
        if (rerr != 0) {
          fail(c, "recover failed");
          return;
        }
        c->readback.assign(4096, 0);
        c->store->read(
            "spdk_obj", 0, c->readback.data(), 4096,
            [](void* a2, int e2) {
              auto* c2 = static_cast<test_ctx*>(a2);
              if (e2 != 0) {
                fail(c2, "read after recover failed");
                return;
              }
              if (c2->readback != c2->pattern) {
                fail(c2, "data mismatch after recover");
                return;
              }
              SPDK_NOTICELOG("aos_spdk_test: recover verify ok\n");
              // Defer shutdown past finish_op as well.
              spdk_thread_send_msg(
                  spdk_get_thread(),
                  [](void* a3) {
                    auto* c3 = static_cast<test_ctx*>(a3);
                    shutdown_app(c3, c3->failures != 0 ? 1 : 0);
                  },
                  c2);
            },
            c);
      },
      ctx);
}

void on_read(void* arg, int err) {
  auto* ctx = static_cast<test_ctx*>(arg);
  if (err != 0) {
    fail(ctx, "read failed");
    return;
  }
  if (ctx->readback != ctx->pattern) {
    fail(ctx, "read data mismatch");
    return;
  }
  SPDK_NOTICELOG("aos_spdk_test: write/read verify ok\n");
  spdk_thread_send_msg(spdk_get_thread(), start_recover_phase, ctx);
}

void on_write(void* arg, int err) {
  auto* ctx = static_cast<test_ctx*>(arg);
  if (err != 0) {
    SPDK_ERRLOG("write err=%d\n", err);
    fail(ctx, "write failed");
    return;
  }
  ctx->readback.assign(4096, 0);
  ctx->store->read("spdk_obj", 0, ctx->readback.data(), 4096, on_read, ctx);
}

void app_start(void* arg) {
  auto* ctx = static_cast<test_ctx*>(arg);

  struct spdk_bdev* bdev = spdk_bdev_get_by_name(g_bdev_name);
  if (bdev == nullptr) {
    fail(ctx, "bdev not found (check -c aos_malloc.json)");
    return;
  }

  const uint64_t block_size = spdk_bdev_get_block_size(bdev);
  const uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);
  const uint64_t bytes = block_size * num_blocks;

  int rc = ctx->backend.open(g_bdev_name, 0, bytes);
  if (rc != 0) {
    SPDK_ERRLOG("backend open rc=%d\n", rc);
    fail(ctx, "spdk_bdev_backend open failed");
    return;
  }

  ctx->store = std::make_unique<pg_store>(&ctx->backend, pg_id{1, 1, 1},
                                          buffer_mode::spdk_dma);
  ctx->pattern.assign(4096, 'S');
  ctx->store->write("spdk_obj", 0, ctx->pattern.data(), ctx->pattern.size(),
                    /*raft*/ 1, /*image*/ 0, on_write, ctx);
}

} // namespace

int main(int argc, char** argv) {
  struct spdk_app_opts opts;
  spdk_app_opts_init(&opts, sizeof(opts));
  opts.name = "aos_spdk_test";

  // Use built-in SPDK args only (-c json config). No app-specific flags.
  enum spdk_app_parse_args_rvals parse_rv =
      spdk_app_parse_args(argc, argv, &opts, nullptr, nullptr, nullptr, nullptr);
  if (parse_rv != SPDK_APP_PARSE_ARGS_SUCCESS) {
    return parse_rv != 0 ? static_cast<int>(parse_rv) : 1;
  }

  test_ctx ctx;
  const int rc = spdk_app_start(&opts, app_start, &ctx);
  spdk_app_fini();

  if (rc != 0 || ctx.failures != 0) {
    std::fprintf(stderr, "aos_spdk_test failed rc=%d failures=%d\n", rc,
                 ctx.failures);
    return 1;
  }
  std::printf("aos_spdk_test: ok\n");
  return 0;
}
