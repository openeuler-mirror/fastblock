/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */

#pragma once

#include <cstdint>
#include <memory>

struct spdk_bdev;

namespace QoS {

struct volume_qos_state;

struct volume_qos {
    std::shared_ptr<volume_qos_state> state;
};

typedef void (*volume_qos_update_complete)(void *cb_arg, int status);

void volume_qos_init(volume_qos *qos, uint64_t iops_limit = 0, uint64_t bw_limit_mib_per_sec = 0);
void volume_qos_fini(volume_qos *qos);

int volume_qos_apply(volume_qos *qos, spdk_bdev *bdev);
int volume_qos_update(volume_qos *qos, spdk_bdev *bdev,
    uint64_t iops_limit, uint64_t bw_limit_mib_per_sec,
    volume_qos_update_complete cb_fn = nullptr, void *cb_arg = nullptr);
int volume_qos_get_config(volume_qos *qos, uint64_t *iops_limit, uint64_t *bw_limit_mib_per_sec);

} // namespace QoS
