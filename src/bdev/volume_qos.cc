/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */

#include "volume_qos.h"

#include <cerrno>
#include <new>

#include <spdk/bdev.h>
#include <spdk/log.h>

namespace QoS {

struct volume_qos_state {
    uint64_t iops_limit = 0;
    uint64_t bw_limit_mib_per_sec = 0;
    bool update_in_progress = false;
};

struct volume_qos_update_ctx {
    std::shared_ptr<volume_qos_state> state;
    uint64_t old_iops_limit = 0;
    uint64_t old_bw_limit_mib_per_sec = 0;
    uint64_t new_iops_limit = 0;
    uint64_t new_bw_limit_mib_per_sec = 0;
    volume_qos_update_complete cb_fn = nullptr;
    void *cb_arg = nullptr;
};

static constexpr uint64_t min_iops_limit = 1000;

static int volume_qos_validate_limits(uint64_t iops_limit)
{
    if (iops_limit != 0 && (iops_limit < min_iops_limit || iops_limit % min_iops_limit != 0)) {
        SPDK_NOTICELOG("[QoS] invalid iops_limit=%lu minimum/granularity=%lu\n",
          iops_limit, min_iops_limit);
        return -EINVAL;
    }
    return 0;
}

static void volume_qos_update_done(void *cb_arg, int status)
{
    auto *ctx = static_cast<volume_qos_update_ctx *>(cb_arg);
    if (!ctx) {
        SPDK_NOTICELOG("[QoS] SPDK QoS update callback status=%d with null context\n", status);
        return;
    }

    if (status == 0) {
        if (ctx->state) {
            ctx->state->iops_limit = ctx->new_iops_limit;
            ctx->state->bw_limit_mib_per_sec = ctx->new_bw_limit_mib_per_sec;
            ctx->state->update_in_progress = false;
        }
        SPDK_NOTICELOG(
          "[QoS] update success iops_limit=%lu bw_limit_mib_per_sec=%lu\n",
          ctx->new_iops_limit, ctx->new_bw_limit_mib_per_sec);
        if (ctx->cb_fn) {
            ctx->cb_fn(ctx->cb_arg, status);
        }
        delete ctx;
        return;
    }

    if (ctx->state) {
        ctx->state->update_in_progress = false;
    }
    SPDK_NOTICELOG(
      "[QoS] update failed status=%d keep old_iops_limit=%lu old_bw_limit_mib_per_sec=%lu requested_iops_limit=%lu requested_bw_limit_mib_per_sec=%lu\n",
      status, ctx->old_iops_limit, ctx->old_bw_limit_mib_per_sec,
      ctx->new_iops_limit, ctx->new_bw_limit_mib_per_sec);
    if (ctx->cb_fn) {
        ctx->cb_fn(ctx->cb_arg, status);
    }
    delete ctx;
}

static int volume_qos_ensure_state(volume_qos *qos)
{
    if (!qos) {
        return -EINVAL;
    }
    if (qos->state) {
        return 0;
    }

    qos->state = std::shared_ptr<volume_qos_state>(new (std::nothrow) volume_qos_state{});
    return qos->state ? 0 : -ENOMEM;
}

void volume_qos_init(volume_qos *qos, uint64_t iops_limit, uint64_t bw_limit_mib_per_sec)
{
    if (!qos) {
        SPDK_NOTICELOG("[QoS] init skipped: null qos\n");
        return;
    }
    if (volume_qos_validate_limits(iops_limit) != 0) {
        SPDK_NOTICELOG("[QoS] init failed: invalid initial qos config iops_limit=%lu bw_limit_mib_per_sec=%lu\n",
          iops_limit, bw_limit_mib_per_sec);
        return;
    }
    if (volume_qos_ensure_state(qos) != 0) {
        SPDK_NOTICELOG("[QoS] init failed: cannot allocate qos state\n");
        return;
    }
    qos->state->iops_limit = iops_limit;
    qos->state->bw_limit_mib_per_sec = bw_limit_mib_per_sec;
    qos->state->update_in_progress = false;

    SPDK_NOTICELOG(
      "[QoS] init qos=%p iops_limit=%lu bw_limit_mib_per_sec=%lu\n",
      qos, qos->state->iops_limit, qos->state->bw_limit_mib_per_sec);
}

void volume_qos_fini(volume_qos *qos)
{
    if (!qos) {
        SPDK_NOTICELOG("[QoS] fini skipped: null qos\n");
        return;
    }

    SPDK_NOTICELOG("[QoS] fini qos=%p\n", qos);
    qos->state.reset();
}

int volume_qos_apply(volume_qos *qos, spdk_bdev *bdev)
{
    if (!qos || !bdev) {
        SPDK_NOTICELOG("[QoS] apply failed qos=%p bdev=%p\n", qos, bdev);
        return -EINVAL;
    }
    if (volume_qos_ensure_state(qos) != 0) {
        SPDK_NOTICELOG("[QoS] apply failed: cannot allocate qos state\n");
        return -ENOMEM;
    }

    uint64_t limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES] = {};
    limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = qos->state->iops_limit;
    /* This SPDK tree takes non-IOPS QoS inputs as MiB/s and converts them to bytes/s internally. */
    limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT] = qos->state->bw_limit_mib_per_sec;

    auto *bdev_name = spdk_bdev_get_name(bdev);
    if (qos->state->iops_limit == 0 && qos->state->bw_limit_mib_per_sec == 0) {
        SPDK_NOTICELOG("[QoS] disabled qos=%p bdev=%s\n", qos, bdev_name);
    }

    SPDK_NOTICELOG(
      "[QoS] create apply qos=%p bdev=%s iops_limit=%lu bw_limit_mib_per_sec=%lu\n",
      qos, bdev_name, qos->state->iops_limit, qos->state->bw_limit_mib_per_sec);
    if (qos->state->update_in_progress) {
        SPDK_NOTICELOG("[QoS] create apply failed bdev=%s: update already in progress\n", bdev_name);
        return -EBUSY;
    }

    auto *ctx = new (std::nothrow) volume_qos_update_ctx{
      qos->state,
      qos->state->iops_limit,
      qos->state->bw_limit_mib_per_sec,
      qos->state->iops_limit,
      qos->state->bw_limit_mib_per_sec,
      nullptr,
      nullptr};
    if (!ctx) {
        SPDK_NOTICELOG("[QoS] apply failed: cannot allocate callback context\n");
        return -ENOMEM;
    }

    qos->state->update_in_progress = true;
    spdk_bdev_set_qos_rate_limits(bdev, limits, volume_qos_update_done, ctx);
    return 0;
}

int volume_qos_update(volume_qos *qos, spdk_bdev *bdev,
    uint64_t iops_limit, uint64_t bw_limit_mib_per_sec,
    volume_qos_update_complete cb_fn, void *cb_arg)
{
    if (!qos || !bdev) {
        SPDK_NOTICELOG("[QoS] update failed qos=%p bdev=%p\n", qos, bdev);
        return -EINVAL;
    }
    if (volume_qos_ensure_state(qos) != 0) {
        SPDK_NOTICELOG("[QoS] update failed: cannot allocate qos state\n");
        return -ENOMEM;
    }
    if (volume_qos_validate_limits(iops_limit) != 0) {
        return -EINVAL;
    }
    if (qos->state->update_in_progress) {
        SPDK_NOTICELOG("[QoS] update failed bdev=%s: update already in progress\n",
          spdk_bdev_get_name(bdev));
        return -EBUSY;
    }

    SPDK_NOTICELOG(
      "[QoS] update start qos=%p bdev=%s old_iops_limit=%lu old_bw_limit_mib_per_sec=%lu new_iops_limit=%lu new_bw_limit_mib_per_sec=%lu\n",
      qos, spdk_bdev_get_name(bdev), qos->state->iops_limit, qos->state->bw_limit_mib_per_sec,
      iops_limit, bw_limit_mib_per_sec);

    uint64_t limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES] = {};
    limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = iops_limit;
    /* This SPDK tree takes non-IOPS QoS inputs as MiB/s and converts them to bytes/s internally. */
    limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT] = bw_limit_mib_per_sec;

    auto *ctx = new (std::nothrow) volume_qos_update_ctx{
      qos->state,
      qos->state->iops_limit,
      qos->state->bw_limit_mib_per_sec,
      iops_limit,
      bw_limit_mib_per_sec,
      cb_fn,
      cb_arg};
    if (!ctx) {
        SPDK_NOTICELOG("[QoS] update failed: cannot allocate callback context\n");
        return -ENOMEM;
    }

    qos->state->update_in_progress = true;
    spdk_bdev_set_qos_rate_limits(bdev, limits, volume_qos_update_done, ctx);
    return 0;
}

int volume_qos_get_config(volume_qos *qos, uint64_t *iops_limit, uint64_t *bw_limit_mib_per_sec)
{
    if (!qos || !qos->state || !iops_limit || !bw_limit_mib_per_sec) {
        return -EINVAL;
    }

    *iops_limit = qos->state->iops_limit;
    *bw_limit_mib_per_sec = qos->state->bw_limit_mib_per_sec;
    return 0;
}

} // namespace QoS
