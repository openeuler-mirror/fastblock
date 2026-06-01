/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "fastblock/client/libfblock.h"
#include <spdk/bdev_module.h>
#include <thread>

#include "fastblock/bdev/global.h"
#include "fastblock/utils/utils.h"
#include "fastblock/utils/err_num.h"

namespace {

struct flatten_image_ctx {
    libblk_client* owner{nullptr};
    std::string pool_name{};
    std::string image_name{};
    std::optional<monitor::client::image_metadata> image_metadata{std::nullopt};
    std::vector<monitor::client::snapshot_metadata> fallback_chain{};
    uint64_t object_count{0};
    uint64_t current_object_seq{0};
    size_t fallback_depth{0};
};

struct rollback_image_ctx {
    libblk_client* owner{nullptr};
    std::string pool_name{};
    std::string image_name{};
    std::string snapshot_name{};
    std::optional<monitor::client::image_metadata> image_metadata{std::nullopt};
    std::optional<monitor::client::snapshot_metadata> snapshot_metadata{std::nullopt};
    uint64_t object_count{0};
    uint64_t object_size{default_object_size};
    uint64_t current_object_seq{0};
};

void flatten_issue_next(flatten_image_ctx* ctx);
void flatten_issue_parent_read(flatten_image_ctx* ctx);
void rollback_issue_next(rollback_image_ctx* ctx);

static bool is_zero_filled(const std::string& data)
{
    for (const auto ch : data) {
        if (ch != '\0') {
            return false;
        }
    }
    return true;
}

void flatten_finish(flatten_image_ctx* ctx, const int32_t state)
{
    SPDK_NOTICELOG("flatten image %s/%s finished with state %d\n", ctx->pool_name.c_str(), ctx->image_name.c_str(), state);
    delete ctx;
}

void flatten_finalize_done(const monitor::client::response_status status, monitor::client::request_context* req_ctx, flatten_image_ctx* ctx)
{
    if (status == monitor::client::response_status::ok) {
        auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
        if (metadata) {
            ctx->owner->refresh_cached_image_metadata(*metadata);
        }
        flatten_finish(ctx, err::E_SUCCESS);
        return;
    }
    flatten_finish(ctx, err::E_BUSY);
}

void flatten_write_done(void *src, int32_t state)
{
    auto* ctx = reinterpret_cast<flatten_image_ctx*>(src);
    if (state != err::E_SUCCESS) {
        flatten_finish(ctx, state);
        return;
    }
    ctx->current_object_seq++;
    flatten_issue_next(ctx);
}

void flatten_parent_read_done(void *src, uint64_t object_idx, const std::string& data, int32_t state)
{
    (void)object_idx;
    auto* ctx = reinterpret_cast<flatten_image_ctx*>(src);
    if (state == err::E_SUCCESS) {
        auto object_name = ctx->owner->calc_image_object_prefix(ctx->image_metadata->pool_id, ctx->image_metadata->image_name);
        object_name = ctx->owner->get_image_object_name(object_name, ctx->current_object_seq);
        ctx->owner->data_client()->write_object(
            object_name,
            0,
            data,
            ctx->image_metadata->pool_id,
            &flatten_write_done,
            ctx,
            0);
        return;
    }
    if (state == err::E_ENOENT) {
        flatten_issue_parent_read(ctx);
        return;
    }
    flatten_finish(ctx, state);
}

void flatten_issue_parent_read(flatten_image_ctx* ctx)
{
    if (ctx->fallback_depth >= ctx->fallback_chain.size()) {
        ctx->current_object_seq++;
        flatten_issue_next(ctx);
        return;
    }

    auto& fallback = ctx->fallback_chain[ctx->fallback_depth++];
    std::string parent_prefix = std::to_string(fallback.source_pool_id) + "__blk_data___" + fallback.source_image_name;
    auto parent_object_name = ctx->owner->get_image_object_name(parent_prefix, ctx->current_object_seq);
    ctx->owner->data_client()->read_object(
        parent_object_name,
        0,
        default_object_size,
        fallback.source_pool_id,
        &flatten_parent_read_done,
        ctx,
        0,
        fallback.snap_seq);
}

void flatten_child_probe_done(void *src, uint64_t object_idx, const std::string& data, int32_t state)
{
    (void)object_idx;
    (void)data;
    auto* ctx = reinterpret_cast<flatten_image_ctx*>(src);
    if (state == err::E_SUCCESS) {
        ctx->current_object_seq++;
        flatten_issue_next(ctx);
        return;
    }
    if (state == err::E_ENOENT) {
        ctx->fallback_depth = 0;
        flatten_issue_parent_read(ctx);
        return;
    }
    flatten_finish(ctx, state);
}

void flatten_issue_next(flatten_image_ctx* ctx)
{
    if (ctx->current_object_seq >= ctx->object_count) {
        ctx->owner->monitor_client()->emplace_finalize_flatten_image_request(
            ctx->image_metadata->image_id,
            [ctx](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
            {
                flatten_finalize_done(status, req_ctx, ctx);
            });
        return;
    }

    std::string child_prefix = std::to_string(ctx->image_metadata->pool_id) + "__blk_data___" + ctx->image_metadata->image_name;
    auto child_object_name = ctx->owner->get_image_object_name(child_prefix, ctx->current_object_seq);
    ctx->owner->data_client()->read_object(
        child_object_name,
        0,
        1,
        ctx->image_metadata->pool_id,
        &flatten_child_probe_done,
        ctx,
        0,
        0);
}

void flatten_on_root_image(const monitor::client::response_status status, monitor::client::request_context* req_ctx, flatten_image_ctx* ctx)
{
    if (status != monitor::client::response_status::ok) {
        flatten_finish(ctx, err::E_ENOENT);
        return;
    }

    auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
    if (!metadata) {
        flatten_finish(ctx, err::E_INVAL);
        return;
    }
    ctx->owner->refresh_cached_image_metadata(*metadata);
    ctx->image_metadata = *metadata;
    if (metadata->parent_snapshot_id.empty()) {
        flatten_finish(ctx, err::E_INVAL);
        return;
    }
    if (metadata->current_snap_seq > 0) {
        flatten_finish(ctx, err::E_BUSY);
        return;
    }
    ctx->fallback_chain = ctx->owner->get_fallback_chain(ctx->image_metadata);
    if (!metadata->parent_snapshot_id.empty() && ctx->fallback_chain.empty()) {
        flatten_finish(ctx, err::E_BUSY);
        return;
    }
    ctx->object_count = (metadata->size + default_object_size - 1) / default_object_size;
    ctx->current_object_seq = 0;
    flatten_issue_next(ctx);
}

void rollback_finish(rollback_image_ctx* ctx, const int32_t state)
{
    SPDK_NOTICELOG(
      "rollback image %s/%s to snapshot %s finished with state %d\n",
      ctx->pool_name.c_str(),
      ctx->image_name.c_str(),
      ctx->snapshot_name.c_str(),
      state);
    delete ctx;
}

void rollback_write_done(void *src, int32_t state)
{
    auto* ctx = reinterpret_cast<rollback_image_ctx*>(src);
    if (state != err::E_SUCCESS) {
        rollback_finish(ctx, state);
        return;
    }
    ctx->current_object_seq++;
    rollback_issue_next(ctx);
}

void rollback_snapshot_read_done(void *src, uint64_t object_idx, const std::string& data, int32_t state)
{
    (void)object_idx;
    auto* ctx = reinterpret_cast<rollback_image_ctx*>(src);
    if (state == err::E_ENOENT) {
        ctx->current_object_seq++;
        rollback_issue_next(ctx);
        return;
    }
    if (state != err::E_SUCCESS) {
        rollback_finish(ctx, state);
        return;
    }

    auto object_prefix = ctx->owner->calc_image_object_prefix(ctx->image_metadata->pool_id, ctx->image_metadata->image_name);
    auto object_name = ctx->owner->get_image_object_name(object_prefix, ctx->current_object_seq);
    if (data.empty() || is_zero_filled(data)) {
        ctx->owner->data_client()->write_object(
            object_name,
            0,
            std::string(data.size(), '\0'),
            ctx->image_metadata->pool_id,
            &rollback_write_done,
            ctx,
            ctx->image_metadata->current_snap_seq);
        return;
    }

    ctx->owner->data_client()->write_object(
        object_name,
        0,
        data,
        ctx->image_metadata->pool_id,
        &rollback_write_done,
        ctx,
        ctx->image_metadata->current_snap_seq);
}

void rollback_issue_next(rollback_image_ctx* ctx)
{
    if (ctx->current_object_seq >= ctx->object_count) {
        rollback_finish(ctx, err::E_SUCCESS);
        return;
    }

    const auto start = ctx->current_object_seq * ctx->object_size;
    auto length = ctx->object_size;
    if (start + length > static_cast<uint64_t>(ctx->image_metadata->size)) {
        length = static_cast<uint64_t>(ctx->image_metadata->size) - start;
    }

    auto object_prefix = ctx->owner->calc_image_object_prefix(ctx->image_metadata->pool_id, ctx->image_metadata->image_name);
    auto object_name = ctx->owner->get_image_object_name(object_prefix, ctx->current_object_seq);
    ctx->owner->data_client()->read_object(
        object_name,
        0,
        length,
        ctx->image_metadata->pool_id,
        &rollback_snapshot_read_done,
        ctx,
        ctx->current_object_seq,
        ctx->snapshot_metadata->snap_seq);
}

void rollback_on_snapshot_metadata(
  const monitor::client::response_status status,
  monitor::client::request_context* req_ctx,
  rollback_image_ctx* ctx)
{
    if (status != monitor::client::response_status::ok) {
        rollback_finish(ctx, err::E_ENOENT);
        return;
    }

    auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
    if (!metadata) {
        rollback_finish(ctx, err::E_INVAL);
        return;
    }
    if (metadata->status != "ready") {
        rollback_finish(ctx, err::E_BUSY);
        return;
    }
    ctx->snapshot_metadata = *metadata;
    if (metadata->source_pool_name != ctx->pool_name || metadata->source_image_name != ctx->image_name) {
        rollback_finish(ctx, err::E_INVAL);
        return;
    }
    ctx->object_size = ctx->image_metadata->object_size > 0 ? static_cast<uint64_t>(ctx->image_metadata->object_size) : default_object_size;
    ctx->object_count = (ctx->image_metadata->size + ctx->object_size - 1) / ctx->object_size;
    ctx->current_object_seq = 0;
    rollback_issue_next(ctx);
}

void rollback_on_snapshot_id(
  const monitor::client::response_status status,
  monitor::client::request_context* req_ctx,
  rollback_image_ctx* ctx)
{
    if (status != monitor::client::response_status::ok) {
        rollback_finish(ctx, err::E_ENOENT);
        return;
    }

    auto& snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
    if (!snapshot_id) {
        rollback_finish(ctx, err::E_INVAL);
        return;
    }
    ctx->owner->monitor_client()->emplace_get_snapshot_metadata_by_id_request(
        *snapshot_id,
        [ctx](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
        {
            rollback_on_snapshot_metadata(status, req_ctx, ctx);
        });
}

void rollback_on_root_image(
  const monitor::client::response_status status,
  monitor::client::request_context* req_ctx,
  rollback_image_ctx* ctx)
{
    if (status != monitor::client::response_status::ok) {
        rollback_finish(ctx, err::E_ENOENT);
        return;
    }

    auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
    if (!metadata) {
        rollback_finish(ctx, err::E_INVAL);
        return;
    }
    if (metadata->status != "ready") {
        rollback_finish(ctx, err::E_BUSY);
        return;
    }
    if (!metadata->parent_snapshot_id.empty()) {
        rollback_finish(ctx, err::E_INVAL);
        return;
    }
    ctx->owner->refresh_cached_image_metadata(*metadata);
    ctx->image_metadata = *metadata;
    ctx->owner->monitor_client()->emplace_get_snapshot_id_by_name_request(
        ctx->pool_name,
        ctx->image_name,
        ctx->snapshot_name,
        [ctx](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
        {
            rollback_on_snapshot_id(status, req_ctx, ctx);
        });
}

} // namespace

void libblk_client::warm_image_lineage_by_metadata(const monitor::client::image_metadata& metadata)
{
    cache_image_metadata(metadata);
    if (!metadata.parent_snapshot_id.empty()) {
        warm_snapshot_lineage(metadata.parent_snapshot_id);
    }
}

void libblk_client::warm_image_lineage_by_id(const std::string& image_id)
{
    _mon_cli->emplace_get_image_metadata_request(
        image_id,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "warm image lineage by id status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            warm_image_lineage_by_metadata(*metadata);
        });
}

void libblk_client::warm_snapshot_lineage(const std::string& snapshot_id)
{
    _mon_cli->emplace_get_snapshot_metadata_by_id_request(
        snapshot_id,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "warm snapshot lineage status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            cache_snapshot_metadata(*metadata);
            if (!metadata->source_image_id.empty()) {
                warm_image_lineage_by_id(metadata->source_image_id);
            }
        });
}

void libblk_client::create_image(
  const std::string pool_name,
  const std::string image_name,
  const size_t size,
  const size_t object_size)
{
    _mon_cli->emplace_create_image_request(
        pool_name,
        image_name,
        size,
        object_size,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "create_image image status %d\n", s);
        });
}

void libblk_client::open_image(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_get_image_info_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "open image status %d\n", s);
        });
    _mon_cli->emplace_get_image_metadata_by_name_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "warm image metadata status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            warm_image_lineage_by_metadata(*metadata);
        });
}

void libblk_client::remove_image(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_remove_image_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "remove image status %d\n", s);
        });
}

void libblk_client::resize_image(const std::string pool_name, const std::string image_name, const size_t size)
{
    _mon_cli->emplace_resize_image_request(
        pool_name,
        image_name,
        size,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "resize_image status %d\n", s);
        });
}

void libblk_client::get_image_info(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_get_image_info_request(
        pool_name,
        image_name,
        [this](const monitor::client::response_status s, [[maybe_unused]] auto _)
        {
            SPDK_INFOLOG(libblk, "get image info status %d\n", s);
        });
}

void libblk_client::get_image_metadata_by_name(const std::string pool_name, const std::string image_name)
{
    _mon_cli->emplace_get_image_metadata_by_name_request(
        pool_name,
        image_name,
        [this] (const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "get image metadata by name status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            warm_image_lineage_by_metadata(*metadata);
            SPDK_INFOLOG(
              libblk,
              "image metadata image_id=%s parent_snapshot_id=%s current_snap_seq=%lu status=%s\n",
              metadata->image_id.c_str(),
              metadata->parent_snapshot_id.c_str(),
              metadata->current_snap_seq,
              metadata->status.c_str());
        });
}

void libblk_client::get_snapshot_metadata_by_id(const std::string snapshot_id)
{
    _mon_cli->emplace_get_snapshot_metadata_by_id_request(
        snapshot_id,
        [this] (const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "get snapshot metadata by id status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            cache_snapshot_metadata(*metadata);
            if (!metadata->source_image_id.empty()) {
                warm_image_lineage_by_id(metadata->source_image_id);
            }
            SPDK_INFOLOG(
              libblk,
              "snapshot metadata snapshot_id=%s source_image_id=%s snap_seq=%lu\n",
              metadata->snapshot_id.c_str(),
              metadata->source_image_id.c_str(),
              metadata->snap_seq);
        });
}

void libblk_client::get_snapshot_id_by_name(const std::string pool_name, const std::string image_name, const std::string snapshot_name)
{
    _mon_cli->emplace_get_snapshot_id_by_name_request(
        pool_name,
        image_name,
        snapshot_name,
        [snapshot_name] (const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "get snapshot id by name status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
            if (!snapshot_id) {
                return;
            }
            SPDK_INFOLOG(
              libblk,
              "snapshot name=%s snapshot_id=%s\n",
              snapshot_name.c_str(),
              snapshot_id->c_str());
        });
}

void libblk_client::create_image_snapshot(const std::string pool_name, const std::string image_name, const std::string snapshot_name)
{
    _mon_cli->emplace_create_image_snapshot_request(
        pool_name,
        image_name,
        snapshot_name,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "create image snapshot status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            cache_snapshot_metadata(*metadata);
            if (!metadata->source_image_name.empty()) {
                auto image_metadata = find_cached_image_metadata(metadata->source_pool_id, metadata->source_image_name);
                if (image_metadata.has_value()) {
                    image_metadata->current_snap_seq = metadata->snap_seq;
                    cache_image_metadata(*image_metadata);
                }
            }
            SPDK_INFOLOG(
              libblk,
              "created snapshot snapshot_id=%s source_image_id=%s snap_seq=%lu\n",
              metadata->snapshot_id.c_str(),
              metadata->source_image_id.c_str(),
              metadata->snap_seq);
        });
}

void libblk_client::create_clone_from_snapshot_name(
  const std::string pool_name,
  const std::string image_name,
  const std::string snapshot_name,
  const std::string clone_image_name)
{
    _mon_cli->emplace_get_snapshot_id_by_name_request(
        pool_name,
        image_name,
        snapshot_name,
        [this, clone_image_name](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "lookup snapshot id for clone status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
            if (!snapshot_id) {
                return;
            }
            create_clone_from_snapshot(*snapshot_id, clone_image_name);
        });
}

void libblk_client::create_clone_from_snapshot(const std::string snapshot_id, const std::string clone_image_name)
{
    _mon_cli->emplace_create_clone_from_snapshot_request(
        snapshot_id,
        clone_image_name,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "create clone from snapshot status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            warm_image_lineage_by_metadata(*metadata);
            SPDK_INFOLOG(
              libblk,
              "created clone image_id=%s image_name=%s parent_snapshot_id=%s\n",
              metadata->image_id.c_str(),
              metadata->image_name.c_str(),
              metadata->parent_snapshot_id.c_str());
        });
}

void libblk_client::protect_snapshot_by_name(
  const std::string pool_name,
  const std::string image_name,
  const std::string snapshot_name)
{
    _mon_cli->emplace_get_snapshot_id_by_name_request(
        pool_name,
        image_name,
        snapshot_name,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "lookup snapshot id for protect status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
            if (!snapshot_id) {
                return;
            }
            protect_snapshot(*snapshot_id);
        });
}

void libblk_client::protect_snapshot(const std::string snapshot_id)
{
    _mon_cli->emplace_protect_snapshot_request(
        snapshot_id,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "protect snapshot status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            cache_snapshot_metadata(*metadata);
        });
}

void libblk_client::unprotect_snapshot_by_name(
  const std::string pool_name,
  const std::string image_name,
  const std::string snapshot_name)
{
    _mon_cli->emplace_get_snapshot_id_by_name_request(
        pool_name,
        image_name,
        snapshot_name,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "lookup snapshot id for unprotect status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
            if (!snapshot_id) {
                return;
            }
            unprotect_snapshot(*snapshot_id);
        });
}

void libblk_client::unprotect_snapshot(const std::string snapshot_id)
{
    _mon_cli->emplace_unprotect_snapshot_request(
        snapshot_id,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "unprotect snapshot status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            cache_snapshot_metadata(*metadata);
        });
}

void libblk_client::delete_image_snapshot_by_name(
  const std::string pool_name,
  const std::string image_name,
  const std::string snapshot_name)
{
    _mon_cli->emplace_get_snapshot_id_by_name_request(
        pool_name,
        image_name,
        snapshot_name,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "lookup snapshot id for delete status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
            if (!snapshot_id) {
                return;
            }
            delete_image_snapshot(*snapshot_id);
        });
}

void libblk_client::delete_image_snapshot(const std::string snapshot_id)
{
    _mon_cli->emplace_delete_image_snapshot_request(
        snapshot_id,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "delete image snapshot status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            cache_snapshot_metadata(*metadata);
        });
}

void libblk_client::rollback_image_to_snapshot(
  const std::string pool_name,
  const std::string image_name,
  const std::string snapshot_name)
{
    auto* ctx = new rollback_image_ctx{
        .owner = this,
        .pool_name = pool_name,
        .image_name = image_name,
        .snapshot_name = snapshot_name,
    };
    _mon_cli->emplace_get_image_metadata_by_name_request(
        pool_name,
        image_name,
        [ctx](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
        {
            rollback_on_root_image(status, req_ctx, ctx);
        });
}

void libblk_client::finalize_flatten_image(const std::string image_id)
{
    _mon_cli->emplace_finalize_flatten_image_request(
        image_id,
        [this](const monitor::client::response_status s, monitor::client::request_context* req_ctx)
        {
            SPDK_INFOLOG(libblk, "finalize flatten image status %d\n", s);
            if (s != monitor::client::response_status::ok) {
                return;
            }
            auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
            if (!metadata) {
                return;
            }
            warm_image_lineage_by_metadata(*metadata);
        });
}

void libblk_client::flatten_image(const std::string pool_name, const std::string image_name)
{
    auto* ctx = new flatten_image_ctx{
        .owner = this,
        .pool_name = pool_name,
        .image_name = image_name,
    };
    _mon_cli->emplace_get_image_metadata_by_name_request(
        pool_name,
        image_name,
        [ctx](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
        {
            flatten_on_root_image(status, req_ctx, ctx);
        });
}

std::vector<monitor::client::snapshot_metadata> libblk_client::build_fallback_chain(
    const std::optional<monitor::client::image_metadata>& image_metadata) const
{
    std::vector<monitor::client::snapshot_metadata> chain{};
    auto current = image_metadata;
    while (current.has_value() && !current->parent_snapshot_id.empty()) {
        auto parent_snapshot = find_cached_snapshot_metadata(current->parent_snapshot_id);
        if (!parent_snapshot.has_value()) {
            break;
        }
        chain.emplace_back(*parent_snapshot);
        current = find_cached_image_metadata(parent_snapshot->source_pool_id, parent_snapshot->source_image_name);
    }
    return chain;
}

bool libblk_client::is_image_lineage_ready(const std::optional<monitor::client::image_metadata>& image_metadata) const
{
    if (!image_metadata.has_value()) {
        return false;
    }

    auto current = image_metadata;
    while (current.has_value() && !current->parent_snapshot_id.empty()) {
        auto parent_snapshot = find_cached_snapshot_metadata(current->parent_snapshot_id);
        if (!parent_snapshot.has_value()) {
            return false;
        }
        current = find_cached_image_metadata(parent_snapshot->source_pool_id, parent_snapshot->source_image_name);
        if (!current.has_value()) {
            return false;
        }
    }
    return true;
}

// bdev的IO，转化为char* buf 的io
int libblk_client::write(
  const uint64_t pool_id,
  const std::string image_name,
  const uint64_t offset,
  uint64_t length,
  struct spdk_bdev_io *bdev_io,
  write_callback cb)
{
    if (length == 0)
    {
        cb(bdev_io, errc::success);
        return 0;
    }

    std::string data;

    for (int i = 0; i < bdev_io->u.bdev.iovcnt; i++)
    {
        data.append((char *)bdev_io->u.bdev.iovs[i].iov_base, bdev_io->u.bdev.iovs[i].iov_len);
    }

    SPDK_INFOLOG(libblk, "write off: %lu len: %lu size: %lu\n", offset, length, data.size());
    if (data.size() > length)
    {
        data = data.substr(0, length);
    }

    write(pool_id, image_name, offset, bdev_io, data, cb);
    return 0;
}

struct write_source
{
    struct object_write_plan {
        std::string object_name;
        uint64_t object_seq;
        uint64_t object_offset;
        uint64_t length;
        uint64_t buffer_offset;
        bool full_overwrite{false};
        size_t fallback_depth{0};
    };

    write_callback cb;
    uint32_t obj_num;
    struct spdk_bdev_io *bdev_io;
    int32_t result;
    struct spdk_thread *thread;
    fblock_client *client;
    uint64_t pool_id;
    uint64_t current_snap_seq;
    std::string image_name;
    std::string data;
    bool is_clone;
    std::vector<monitor::client::snapshot_metadata> fallback_chain;
    std::vector<object_write_plan> plans;

    write_source(write_callback _cb, uint32_t _obj_num, struct spdk_bdev_io *_bdev_io,
                 fblock_client* _client, uint64_t _pool_id, uint64_t _current_snap_seq,
                 std::string _image_name, std::string _data, bool _is_clone,
                 std::vector<monitor::client::snapshot_metadata> _fallback_chain)
    : cb(_cb)
    , obj_num(_obj_num)
    , bdev_io(_bdev_io)
    , result(err::E_SUCCESS)
    , thread(spdk_get_thread())
    , client(_client)
    , pool_id(_pool_id)
    , current_snap_seq(_current_snap_seq)
    , image_name(std::move(_image_name))
    , data(std::move(_data))
    , is_clone(_is_clone)
    , fallback_chain(std::move(_fallback_chain)) {
        plans.reserve(_obj_num);
    }

    ~write_source() {}

    static void thread_run(void *arg)
    {
        write_source *source = (write_source *)arg;
        source->cb(source->bdev_io, source->result);
        delete source;
    }

    void invoke()
    {
#ifdef DEBUG
        /*
           debug模式下，回调cb里调用spdk_bdev_io_complete时，会开启assert检查当前spdk thread与_bdev_io的spdk thread是否相同
        */
        spdk_thread_send_msg(thread, &thread_run, (void *)this);
#else
        cb(bdev_io, result);
        delete this;
#endif
    }

    static void finish_one(write_source* source, int32_t state) {
        if(state != err::E_SUCCESS){
            source->result = state;
        }
        source->obj_num--;
        SPDK_INFOLOG(libblk, "write_done, state: %d source->obj_num: %u\n", state, source->obj_num);
        if (source->obj_num == 0)
        {
            SPDK_INFOLOG(libblk, "write_done\n");
            source->invoke();
        }
    }

    static void submit_direct_write(write_source* source, uint64_t object_idx) {
        auto& plan = source->plans.at(object_idx);
        auto str = std::string(source->data.data() + plan.buffer_offset, plan.length);
        source->client->write_object(
            plan.object_name,
            plan.object_offset,
            str,
            source->pool_id,
            &write_source::write_done,
            source,
            source->current_snap_seq);
    }

    static void submit_copyup_write(write_source* source, uint64_t object_idx, std::string full_object) {
        auto& plan = source->plans.at(object_idx);
        memcpy(full_object.data() + plan.object_offset, source->data.data() + plan.buffer_offset, plan.length);
        source->client->write_object(
            plan.object_name,
            0,
            full_object,
            source->pool_id,
            &write_source::write_done,
            source,
            source->current_snap_seq);
    }

    static void issue_parent_read(write_source* source, uint64_t object_idx) {
        auto& plan = source->plans.at(object_idx);
        auto& fallback = source->fallback_chain[plan.fallback_depth++];
        auto parent_prefix = std::to_string(fallback.source_pool_id) + "__blk_data___" + fallback.source_image_name;
        auto parent_object_name = parent_prefix + std::to_string(plan.object_seq);
        source->client->read_object(
            parent_object_name,
            0,
            default_object_size,
            fallback.source_pool_id,
            &write_source::copyup_read_done,
            source,
            object_idx,
            fallback.snap_seq);
    }

    static void copyup_read_done(void *src, uint64_t object_idx, const std::string& data, int32_t state) {
        write_source* source = (write_source*)src;
        auto& plan = source->plans.at(object_idx);
        if(state == err::E_SUCCESS){
            submit_copyup_write(source, object_idx, data);
            return;
        }
        if(state == err::E_ENOENT){
            if(plan.fallback_depth < source->fallback_chain.size()){
                issue_parent_read(source, object_idx);
                return;
            }
            if(source->is_clone){
                finish_one(source, err::E_BUSY);
                return;
            }
            submit_copyup_write(source, object_idx, std::string(default_object_size, '\0'));
            return;
        }
        finish_one(source, state);
    }

    static void write_done(void *src, int32_t state) {
        write_source* source = (write_source*)src;
        finish_one(source, state);
    }
};
// get obj number
static uint64_t get_obj_num(const uint64_t offset, const uint64_t length)
{
    auto start_off = utils::align_down(offset, default_object_size);
    auto end_off = utils::align_up(offset + length, default_object_size);

    return (end_off - start_off) / default_object_size;
}

int libblk_client::write(const uint64_t pool_id, const std::string image_name, const uint64_t offset,
                        struct spdk_bdev_io *bdev_io, std::string& buf, write_callback cb)
{
    if (buf.size() == 0)
    {
        cb(bdev_io, errc::success);
        return 0;
    }
    auto length = buf.size();

    auto object_prefix = calc_image_object_prefix(pool_id, image_name);
    auto [expected_object_size, object_offset, object_seq] = calc_first_object_position(offset, length, default_object_size);
    size_t write_bytes = 0;

    auto obj_num = get_obj_num(offset, length);
    uint64_t current_snap_seq = 0;
    auto image_metadata = find_cached_image_metadata(static_cast<int32_t>(pool_id), image_name);
    if (!is_image_lineage_ready(image_metadata)) {
        cb(bdev_io, err::E_BUSY);
        return 0;
    }
    if (image_metadata.has_value()) {
        current_snap_seq = image_metadata->current_snap_seq;
    }
    auto fallback_chain = build_fallback_chain(image_metadata);
    bool is_clone = image_metadata.has_value() && !image_metadata->parent_snapshot_id.empty();
    // 建立回调函数
    write_source *source = new write_source(cb, obj_num, bdev_io, _client.get(), pool_id, current_snap_seq, image_name, std::move(buf), is_clone, std::move(fallback_chain));

    SPDK_INFOLOG(libblk, "write pool: %lu image_name: %s offset: %lu length: %lu  obj_num: %lu \n",
            pool_id, image_name.c_str(), offset, length, obj_num);
    uint64_t object_idx = 0;
    while (write_bytes < length)
    {

        auto object_name = get_image_object_name(object_prefix, object_seq);
        source->plans.push_back(write_source::object_write_plan{
            .object_name = object_name,
            .object_seq = object_seq,
            .object_offset = object_offset,
            .length = expected_object_size,
            .buffer_offset = write_bytes,
            .full_overwrite = (object_offset == 0 && expected_object_size == default_object_size),
        });
        auto& plan = source->plans.back();
        if (!source->is_clone || plan.full_overwrite) {
            write_source::submit_direct_write(source, object_idx);
        } else {
            _client->read_object(
                object_name,
                0,
                default_object_size,
                pool_id,
                &write_source::copyup_read_done,
                source,
                object_idx);
        }
        write_bytes += expected_object_size;
        expected_object_size = default_object_size; // 默认的对象大小
        if (expected_object_size > (length - write_bytes))
        {
            expected_object_size = length - write_bytes;
        }
        object_offset = uint64_t(0);
        object_seq++;
        object_idx++;
    }

    return 0;
}

struct read_source
{
    struct object_read_plan {
        uint64_t object_seq;
        uint64_t object_offset;
        uint64_t length;
        uint64_t buffer_offset;
        size_t fallback_depth{0};
    };

    read_callback cb;
    uint32_t obj_num;
    char *buf;
    uint64_t len;
    struct spdk_bdev_io *bdev_io;
    int32_t result;
    uint64_t offset;
    struct spdk_thread *thread;
    fblock_client *client;
    uint64_t pool_id;
    std::string image_name;
    std::optional<monitor::client::image_metadata> image_metadata;
    bool is_clone;
    std::vector<monitor::client::snapshot_metadata> fallback_chain;
    std::vector<object_read_plan> plans;


    read_source(read_callback _cb, uint32_t _obj_num, uint64_t _len, struct spdk_bdev_io *_bdev_io,
            uint64_t _offset, fblock_client* _client, uint64_t _pool_id, std::string _image_name,
            std::optional<monitor::client::image_metadata> _image_metadata, bool _is_clone,
            std::vector<monitor::client::snapshot_metadata> _fallback_chain)
    : cb(_cb)
    , obj_num(_obj_num)
    , len(_len)
    , bdev_io(_bdev_io)
    , result(err::E_SUCCESS)
    , offset(_offset)
    , thread(spdk_get_thread())
    , client(_client)
    , pool_id(_pool_id)
    , image_name(std::move(_image_name))
    , image_metadata(std::move(_image_metadata))
    , is_clone(_is_clone)
    , fallback_chain(std::move(_fallback_chain)) {
        buf = new char[len];
        memset(buf, 0, len);
        plans.reserve(_obj_num);
    }

    ~read_source()
    {
        delete[] buf;
    }

    static void thread_run(void *arg){
        read_source* source = (read_source*)arg;
        source->cb(source->bdev_io, source->buf, source->len, source->result);
        delete source;
    }

    void invoke()
    {
#ifdef DEBUG
        /*
           debug模式下，回调cb里调用spdk_bdev_io_complete时，会开启assert检查当前spdk thread与_bdev_io的spdk thread是否相同
        */
        spdk_thread_send_msg(thread, &thread_run, (void *)this);
#else
        cb(bdev_io, buf, len, result);
        delete this;
#endif
    }

    static void finish_one(read_source* source, int32_t state) {
        if(state != err::E_SUCCESS){
            source->result = state;
        }
        source->obj_num--;
        if(source->obj_num == 0){
            SPDK_INFOLOG(libblk, "---- read data off: %lu length: %lu\n", source->offset, source->len);
            source->invoke();
        }
    }

    static void read_done(void *src, uint64_t object_idx, const std::string& data, int32_t state) {
        read_source* source = (read_source*)src;
        auto& plan = source->plans.at(object_idx);
        if(state == err::E_SUCCESS){
            memcpy(source->buf + plan.buffer_offset, data.data(), data.size());
            finish_one(source, err::E_SUCCESS);
            return;
        }

        if(state == err::E_ENOENT){
            if(plan.fallback_depth < source->fallback_chain.size()){
                auto& fallback = source->fallback_chain[plan.fallback_depth++];
                auto parent_prefix = std::to_string(fallback.source_pool_id) + "__blk_data___" + fallback.source_image_name;
                auto parent_object_name = parent_prefix + std::to_string(plan.object_seq);
                source->client->read_object(
                    parent_object_name,
                    plan.object_offset,
                    plan.length,
                    fallback.source_pool_id,
                    &read_source::read_done,
                    source,
                    object_idx,
                    fallback.snap_seq);
                return;
            }
            if(source->is_clone){
                finish_one(source, err::E_BUSY);
                return;
            }
            finish_one(source, err::E_SUCCESS);
            return;
        }

        finish_one(source, state);
    }
};

int libblk_client::read(const uint64_t pool_id, const std::string image_name, const uint64_t offset,
                        const uint64_t length, struct spdk_bdev_io *bdev_io, read_callback cb)
{
    if (length == 0)
    {
        cb(bdev_io, nullptr, 0, err::E_SUCCESS);
        return 0;
    }
    auto object_prefix = calc_image_object_prefix(pool_id, image_name);
    auto [expected_object_size, object_offset, object_seq] = calc_first_object_position(offset, length, default_object_size);
    size_t read_bytes = 0;

    auto obj_num = get_obj_num(offset, length);
    auto image_metadata = find_cached_image_metadata(static_cast<int32_t>(pool_id), image_name);
    if (!is_image_lineage_ready(image_metadata)) {
        cb(bdev_io, nullptr, 0, err::E_BUSY);
        return 0;
    }
    auto fallback_chain = build_fallback_chain(image_metadata);
    bool is_clone = image_metadata.has_value() && !image_metadata->parent_snapshot_id.empty();
    read_source* source = new read_source(
        cb,
        obj_num,
        length,
        bdev_io,
        offset,
        _client.get(),
        pool_id,
        image_name,
        image_metadata,
        is_clone,
        std::move(fallback_chain));
    uint64_t object_idx = 0;
    SPDK_INFOLOG(libblk, "read pool: %lu image_name: %s offset: %lu length: %lu  obj_num: %lu\n",
                 pool_id, image_name.c_str(), offset, length, obj_num);

    while (read_bytes < length)
    {
        auto object_name = get_image_object_name(object_prefix, object_seq);
        source->plans.push_back(read_source::object_read_plan{
            .object_seq = object_seq,
            .object_offset = object_offset,
            .length = expected_object_size,
            .buffer_offset = read_bytes,
        });
        _client->read_object(object_name, object_offset, expected_object_size, pool_id, &read_source::read_done, source, object_idx);
        read_bytes += expected_object_size;
        expected_object_size = default_object_size; // 默认的对象大小
        if (expected_object_size > (length - read_bytes))
        {
            expected_object_size = length - read_bytes;
        }
        object_offset = uint64_t(0);
        object_seq++;
        object_idx++;
    }

    return 0;
}

std::string libblk_client::calc_image_object_prefix(const uint64_t pool_id, const std::string &image_name)
{
    return std::to_string(pool_id) + "__blk_data___" + image_name;
}

std::tuple<size_t, uint64_t, uint64_t>
libblk_client::calc_first_object_position(const uint64_t offset, const uint64_t length, const size_t object_size)
{
    uint64_t first_object_offset = offset % object_size;
    size_t first_object_size = object_size - first_object_offset;

    if (length < first_object_size)
    {
        first_object_size = length;
    }
    uint64_t object_seq = offset / uint64_t(object_size);

    return std::make_tuple(first_object_size, first_object_offset, object_seq);
}

std::string libblk_client::get_image_object_name(std::string &prefix, uint64_t seq)
{
    char ch[17];
    snprintf(ch, 17, "%lu", seq);
    std::string str(ch);
    return prefix + str;
}
