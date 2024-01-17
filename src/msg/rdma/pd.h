/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#pragma once

#include "device.h"

#include <spdk/log.h>

#include <optional>
#include <string>
#include <utility>

#include <infiniband/verbs.h>

namespace msg {
namespace rdma {

class protection_domain {

public:

    protection_domain(std::shared_ptr<device> dev) : _device{dev} {
        auto fn = [this] (device::device_context* ctx) -> iterate_tag {
            auto dev_name = ::ibv_get_device_name(ctx->device);

            _pd = ::ibv_alloc_pd(ctx->context);
            if (not _pd) {
                SPDK_ERRLOG(
                  "failed allocating protection domain on port %d of %s\n",
                  ctx->port, dev_name);

                return iterate_tag::keep;
            }

            SPDK_INFOLOG(
              msg,
              "allocated protection domain on port %d of %s\n",
              ctx->port, dev_name);

            _ctx = ctx;
            return iterate_tag::stop;
        };

        _device->fold_device_while(fn);

        if (not _pd) {
            throw std::runtime_error{"allocate pd failed"};
        }
    }

    protection_domain(const protection_domain&) = delete;

    protection_domain(protection_domain&& that) noexcept
      : _pd{std::exchange(that._pd, nullptr)} {}

    protection_domain& operator=(const protection_domain&) = delete;

    protection_domain& operator=(protection_domain&& that) noexcept {
        _pd = std::exchange(that._pd, nullptr);
        return *this;
    }

    ~protection_domain() noexcept {
        if (_pd) {
            ::ibv_dealloc_pd(_pd);
        }
    }

public:

    [[gnu::always_inline]] ibv_pd* value() const noexcept {
        return _pd;
    }

    [[gnu::always_inline]] ibv_pd* value() noexcept {
        return _pd;
    }

    [[gnu::always_inline]] ibv_device_attr& device_attr() noexcept {
        return _ctx->device_attr->orig_attr;
    }

    [[gnu::always_inline]] ibv_device* deivce() noexcept {
        return _ctx->device;
    }

    [[gnu::always_inline]] ibv_context* device_context() noexcept {
        return _ctx->context;
    }

private:

    std::shared_ptr<device> _device{nullptr};
    device::device_context* _ctx{nullptr};
    ::ibv_pd* _pd{nullptr};
};

} // namespace rdma
} // namespace msg

