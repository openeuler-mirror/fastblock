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

#include "bdev_fastblock.h"
#include "fastblock/bdev/global.h"
#include <spdk/rpc.h>
#include <spdk/util.h>
#include <spdk/string.h>
#include <spdk/log.h>

static std::shared_ptr<::libblk_client>
get_management_blk_client()
{
	if (global::blk_client) {
		return global::blk_client;
	}
	if (global::app_thread_shard_id < global::blk_clients.size()) {
		auto blk_cli = global::blk_clients.at(global::app_thread_shard_id);
		if (blk_cli) {
			return blk_cli;
		}
	}
	for (auto& blk_cli : global::blk_clients) {
		if (blk_cli) {
			return blk_cli;
		}
	}
	return nullptr;
}

static void
send_monitor_status_error(struct spdk_jsonrpc_request *request, monitor::client::response_status status)
{
	int rc = -EIO;
	switch (status)
	{
	case monitor::client::response_status::image_not_found:
		rc = -ENOENT;
		break;
	case monitor::client::response_status::created_image_exists:
		rc = -EEXIST;
		break;
	default:
		rc = -EIO;
		break;
	}
	spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
}

static void
write_snapshot_metadata_json(struct spdk_json_write_ctx *w, const monitor::client::snapshot_metadata &metadata)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "snapshot_id", metadata.snapshot_id.c_str());
	spdk_json_write_named_string(w, "snapshot_name", metadata.snapshot_name.c_str());
	spdk_json_write_named_string(w, "source_image_id", metadata.source_image_id.c_str());
	spdk_json_write_named_string(w, "source_pool_name", metadata.source_pool_name.c_str());
	spdk_json_write_named_string(w, "source_image_name", metadata.source_image_name.c_str());
	spdk_json_write_named_uint32(w, "source_pool_id", metadata.source_pool_id);
	spdk_json_write_named_uint64(w, "snap_seq", metadata.snap_seq);
	spdk_json_write_named_string(w, "status", metadata.status.c_str());
	spdk_json_write_named_bool(w, "protected", metadata.is_protected);
	spdk_json_write_named_string(w, "operation_id", metadata.operation_id.c_str());
	spdk_json_write_named_uint32(w, "child_count", metadata.child_count);
	spdk_json_write_named_int64(w, "created_at_unix_nano", metadata.created_at_unix_nano);
	spdk_json_write_named_int64(w, "updated_at_unix_nano", metadata.updated_at_unix_nano);
	spdk_json_write_object_end(w);
}

static void
write_image_metadata_json(struct spdk_json_write_ctx *w, const monitor::client::image_metadata &metadata)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "image_id", metadata.image_id.c_str());
	spdk_json_write_named_uint32(w, "pool_id", metadata.pool_id);
	spdk_json_write_named_string(w, "pool_name", metadata.pool_name.c_str());
	spdk_json_write_named_string(w, "image_name", metadata.image_name.c_str());
	spdk_json_write_named_int64(w, "size", metadata.size);
	spdk_json_write_named_int64(w, "object_size", metadata.object_size);
	spdk_json_write_named_uint64(w, "current_snap_seq", metadata.current_snap_seq);
	spdk_json_write_named_string(w, "status", metadata.status.c_str());
	spdk_json_write_named_string(w, "parent_snapshot_id", metadata.parent_snapshot_id.c_str());
	spdk_json_write_named_uint32(w, "depth", metadata.depth);
	spdk_json_write_named_uint64(w, "generation", metadata.generation);
	spdk_json_write_named_int64(w, "created_at_unix_nano", metadata.created_at_unix_nano);
	spdk_json_write_named_int64(w, "updated_at_unix_nano", metadata.updated_at_unix_nano);
	spdk_json_write_object_end(w);
}

static void
advance_image_snap_seq_on_all_clients(
	const int32_t pool_id,
	const std::string& image_name,
	const uint64_t snap_seq)
{
	auto advance = [pool_id, &image_name, snap_seq](const std::shared_ptr<::libblk_client>& blk_cli) {
		if (blk_cli) {
			blk_cli->advance_cached_image_snap_seq(pool_id, image_name, snap_seq);
		}
	};

	advance(global::blk_client);
	for (auto& blk_cli : global::blk_clients) {
		if (blk_cli && blk_cli != global::blk_client) {
			advance(blk_cli);
		}
	}
}

struct rpc_bdev_fastblock_name_request
{
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_name_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_name_request, name), spdk_json_decode_string},
};

static void
free_rpc_bdev_fastblock_name_request(struct rpc_bdev_fastblock_name_request *req)
{
	free(req->name);
}

struct rpc_create_fastblock
{
	char *name;
	uint64_t pool_id;
	char *pool_name;
	char *image_name;
	uint64_t image_size;
	uint64_t object_size;
	uint32_t block_size;
	char *monitor_address;
};

struct rpc_register_fastblock_existing
{
	char *name;
	char *pool_name;
	char *image_name;
	uint32_t block_size;
	char *monitor_address;
};

struct rpc_register_fastblock_existing_ctx
{
	struct spdk_jsonrpc_request *request;
	std::string name;
	std::string pool_name;
	std::string image_name;
	uint32_t block_size;
	std::string monitor_address;
};

static void
free_rpc_create_fastblock(struct rpc_create_fastblock *req)
{
	free(req->name);
	free(req->image_name);
	free(req->monitor_address);
}

static void
free_rpc_register_fastblock_existing(struct rpc_register_fastblock_existing *req)
{
	free(req->name);
	free(req->pool_name);
	free(req->image_name);
	free(req->monitor_address);
}

static int
bdev_fastblock_decode_config(const struct spdk_json_val *values, void *out)
{
	char ***map = (char ***)out;
	char **entry;
	uint32_t i;

	if (values->type == SPDK_JSON_VAL_NULL)
	{
		*map = (char **)calloc(1, sizeof(**map));
		if (!*map)
		{
			return -1;
		}
		return 0;
	}

	if (values->type != SPDK_JSON_VAL_OBJECT_BEGIN)
	{
		return -1;
	}

	*map = (char **)calloc(values->len + 1, sizeof(**map));
	if (!*map)
	{
		return -1;
	}

	for (i = 0, entry = *map; i < values->len;)
	{
		const struct spdk_json_val *name = &values[i + 1];
		const struct spdk_json_val *v = &values[i + 2];
		if (!(entry[0] = spdk_json_strdup(name)) ||
			!(entry[1] = spdk_json_strdup(v)))
		{
			bdev_fastblock_free_config(*map);
			*map = NULL;
			return -1;
		}
		i += 1 + spdk_json_val_len(v);
		entry += 2;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_create_fastblock_decoders[] = {
	{"name", offsetof(struct rpc_create_fastblock, name), spdk_json_decode_string, true},
	{"pool_name", offsetof(struct rpc_create_fastblock, pool_name), spdk_json_decode_string},
	{"image_name", offsetof(struct rpc_create_fastblock, image_name), spdk_json_decode_string},
	{"image_size", offsetof(struct rpc_create_fastblock, image_size), spdk_json_decode_uint64},
	{"object_size", offsetof(struct rpc_create_fastblock, object_size), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_create_fastblock, block_size), spdk_json_decode_uint32},
	{"monitor_address", offsetof(struct rpc_create_fastblock, monitor_address), spdk_json_decode_string}};

static const struct spdk_json_object_decoder rpc_register_fastblock_existing_decoders[] = {
	{"name", offsetof(struct rpc_register_fastblock_existing, name), spdk_json_decode_string, true},
	{"pool_name", offsetof(struct rpc_register_fastblock_existing, pool_name), spdk_json_decode_string},
	{"image_name", offsetof(struct rpc_register_fastblock_existing, image_name), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_register_fastblock_existing, block_size), spdk_json_decode_uint32},
	{"monitor_address", offsetof(struct rpc_register_fastblock_existing, monitor_address), spdk_json_decode_string}};

static void
rpc_bdev_fastblock_create(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_create_fastblock req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	SPDK_DEBUGLOG(bdev_fastblock, "rpc_bdev_fastblock_create\n");
	if (spdk_json_decode_object(params, rpc_create_fastblock_decoders,
								SPDK_COUNTOF(rpc_create_fastblock_decoders),
								&req))
	{
		SPDK_DEBUGLOG(bdev_fastblock, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_fastblock_create(&bdev, req.name, req.pool_name,
							   req.image_name,
							   req.image_size,
							   req.block_size,
							   req.object_size,
							   req.monitor_address);
	if (rc)
	{
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_create_fastblock(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_create", rpc_bdev_fastblock_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_fastblock_create, construct_fastblock_bdev)

static void
rpc_bdev_fastblock_register_existing_on_image(
	const monitor::client::response_status status,
	monitor::client::request_context *req_ctx,
	rpc_register_fastblock_existing_ctx *ctx)
{
	if (status != monitor::client::response_status::ok)
	{
		send_monitor_status_error(ctx->request, status);
		delete ctx;
		return;
	}

	auto &metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
	if (!metadata)
	{
		spdk_jsonrpc_send_error_response(ctx->request, -EIO, spdk_strerror(EIO));
		delete ctx;
		return;
	}

	struct spdk_bdev *bdev = nullptr;
	auto rc = bdev_fastblock_register_existing(
		&bdev,
		ctx->name.c_str(),
		ctx->pool_name.c_str(),
		ctx->image_name.c_str(),
		metadata->size,
		ctx->block_size,
		metadata->object_size,
		ctx->monitor_address.c_str());
	if (rc != 0)
	{
		spdk_jsonrpc_send_error_response(ctx->request, rc, spdk_strerror(-rc));
		delete ctx;
		return;
	}

	auto *w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(ctx->request, w);
	delete ctx;
}

static void
rpc_bdev_fastblock_register_existing(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_register_fastblock_existing req = {};
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_register_fastblock_existing_decoders,
								SPDK_COUNTOF(rpc_register_fastblock_existing_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (spdk_bdev_get_by_name(req.name) != NULL)
	{
		spdk_jsonrpc_send_error_response(request, -EEXIST, spdk_strerror(EEXIST));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	{
		auto *ctx = new rpc_register_fastblock_existing_ctx{
			.request = request,
			.name = req.name,
			.pool_name = req.pool_name,
			.image_name = req.image_name,
			.block_size = req.block_size,
			.monitor_address = req.monitor_address,
		};
		blk_cli->monitor_client()->emplace_get_image_metadata_by_name_request(
			req.pool_name,
			req.image_name,
			[ctx](const monitor::client::response_status status, monitor::client::request_context *req_ctx)
			{
				rpc_bdev_fastblock_register_existing_on_image(status, req_ctx, ctx);
			});
	}

cleanup:
	free_rpc_register_fastblock_existing(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_register_existing", rpc_bdev_fastblock_register_existing, SPDK_RPC_RUNTIME)

struct rpc_bdev_fastblock_delete
{
	char *name;
};

static void
free_rpc_bdev_fastblock_delete(struct rpc_bdev_fastblock_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_fastblock_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_delete, name), spdk_json_decode_string},
};

static void
_rpc_bdev_fastblock_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = (struct spdk_jsonrpc_request *)cb_arg;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_fastblock_delete(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_delete req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_delete_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_delete_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	bdev_fastblock_delete(bdev, _rpc_bdev_fastblock_delete_cb, request);

cleanup:
	free_rpc_bdev_fastblock_delete(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_delete", rpc_bdev_fastblock_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_fastblock_delete, delete_fastblock_bdev)

struct rpc_bdev_fastblock_image_name_request
{
	char *pool_name;
	char *image_name;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_image_name_request_decoders[] = {
	{"pool_name", offsetof(struct rpc_bdev_fastblock_image_name_request, pool_name), spdk_json_decode_string},
	{"image_name", offsetof(struct rpc_bdev_fastblock_image_name_request, image_name), spdk_json_decode_string},
};

static void
free_rpc_bdev_fastblock_image_name_request(struct rpc_bdev_fastblock_image_name_request *req)
{
	free(req->pool_name);
	free(req->image_name);
}

static void
rpc_bdev_fastblock_remove_image(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_image_name_request req = {};
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_image_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_image_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->remove_image(req.pool_name, req.image_name);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_image_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_remove_image", rpc_bdev_fastblock_remove_image, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_get_image_metadata(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_image_name_request req = {};
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_image_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_image_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->monitor_client()->emplace_get_image_metadata_by_name_request(
		req.pool_name,
		req.image_name,
		[request](const monitor::client::response_status status, monitor::client::request_context *req_ctx)
		{
			if (status != monitor::client::response_status::ok)
			{
				send_monitor_status_error(request, status);
				return;
			}
			auto &metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
			if (!metadata)
			{
				spdk_jsonrpc_send_error_response(request, -EIO, spdk_strerror(EIO));
				return;
			}
			auto *w = spdk_jsonrpc_begin_result(request);
			write_image_metadata_json(w, *metadata);
			spdk_jsonrpc_end_result(request, w);
		});

cleanup:
	free_rpc_bdev_fastblock_image_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_get_image_metadata", rpc_bdev_fastblock_get_image_metadata, SPDK_RPC_RUNTIME)

struct rpc_bdev_fastblock_resize
{
	char *name;
	uint64_t new_size;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_resize, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_fastblock_resize, new_size), spdk_json_decode_uint64}};

static void
free_rpc_bdev_fastblock_resize(struct rpc_bdev_fastblock_resize *req)
{
	free(req->name);
}

static void
rpc_bdev_fastblock_resize(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_resize req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_resize_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_resize_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = bdev_fastblock_resize(bdev, req.new_size);
	if (rc)
	{
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_rpc_bdev_fastblock_resize(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_resize", rpc_bdev_fastblock_resize, SPDK_RPC_RUNTIME)

struct rpc_bdev_fastblock_flatten
{
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_flatten_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_flatten, name), spdk_json_decode_string},
};

static void
free_rpc_bdev_fastblock_flatten(struct rpc_bdev_fastblock_flatten *req)
{
	free(req->name);
}

static void
rpc_bdev_fastblock_flatten(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_flatten req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_flatten_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_flatten_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = bdev_fastblock_flatten(bdev);
	if (rc)
	{
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_flatten(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_flatten", rpc_bdev_fastblock_flatten, SPDK_RPC_RUNTIME)

struct rpc_bdev_fastblock_create_snapshot
{
	char *name;
	char *snapshot_name;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_create_snapshot_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_create_snapshot, name), spdk_json_decode_string},
	{"snapshot_name", offsetof(struct rpc_bdev_fastblock_create_snapshot, snapshot_name), spdk_json_decode_string},
};

static void
free_rpc_bdev_fastblock_create_snapshot(struct rpc_bdev_fastblock_create_snapshot *req)
{
	free(req->name);
	free(req->snapshot_name);
}

static void
rpc_bdev_fastblock_create_snapshot(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_create_snapshot req = {};
	struct spdk_bdev *bdev;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_create_snapshot_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_create_snapshot_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->monitor_client()->emplace_create_image_snapshot_request(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		req.snapshot_name,
		[request, blk_cli](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
		{
			if (status != monitor::client::response_status::ok)
			{
				send_monitor_status_error(request, status);
				return;
			}
			auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
			if (!metadata)
			{
				spdk_jsonrpc_send_error_response(request, -EIO, spdk_strerror(EIO));
				return;
			}
			advance_image_snap_seq_on_all_clients(metadata->source_pool_id, metadata->source_image_name, metadata->snap_seq);
			auto *w = spdk_jsonrpc_begin_result(request);
			spdk_json_write_bool(w, true);
			spdk_jsonrpc_end_result(request, w);
		});

cleanup:
	free_rpc_bdev_fastblock_create_snapshot(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_create_snapshot", rpc_bdev_fastblock_create_snapshot, SPDK_RPC_RUNTIME)

struct rpc_bdev_fastblock_snapshot_name_request
{
	char *name;
	char *snapshot_name;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_snapshot_name_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_snapshot_name_request, name), spdk_json_decode_string},
	{"snapshot_name", offsetof(struct rpc_bdev_fastblock_snapshot_name_request, snapshot_name), spdk_json_decode_string},
};

static void
free_rpc_bdev_fastblock_snapshot_name_request(struct rpc_bdev_fastblock_snapshot_name_request *req)
{
	free(req->name);
	free(req->snapshot_name);
}

static void
rpc_bdev_fastblock_rollback_to_snapshot(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_snapshot_name_request req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_snapshot_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_snapshot_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = bdev_fastblock_rollback_to_snapshot(bdev, req.snapshot_name);
	if (rc)
	{
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_snapshot_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_rollback_to_snapshot", rpc_bdev_fastblock_rollback_to_snapshot, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_protect_snapshot_by_name(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_snapshot_name_request req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_snapshot_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_snapshot_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->protect_snapshot_by_name(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		req.snapshot_name);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_snapshot_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_protect_snapshot_by_name", rpc_bdev_fastblock_protect_snapshot_by_name, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_unprotect_snapshot_by_name(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_snapshot_name_request req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_snapshot_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_snapshot_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->unprotect_snapshot_by_name(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		req.snapshot_name);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_snapshot_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_unprotect_snapshot_by_name", rpc_bdev_fastblock_unprotect_snapshot_by_name, SPDK_RPC_RUNTIME)

struct rpc_bdev_fastblock_clone_snapshot_request
{
	char *name;
	char *snapshot_name;
	char *clone_image_name;
};

static const struct spdk_json_object_decoder rpc_bdev_fastblock_clone_snapshot_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_clone_snapshot_request, name), spdk_json_decode_string},
	{"snapshot_name", offsetof(struct rpc_bdev_fastblock_clone_snapshot_request, snapshot_name), spdk_json_decode_string},
	{"clone_image_name", offsetof(struct rpc_bdev_fastblock_clone_snapshot_request, clone_image_name), spdk_json_decode_string},
};

static void
free_rpc_bdev_fastblock_clone_snapshot_request(struct rpc_bdev_fastblock_clone_snapshot_request *req)
{
	free(req->name);
	free(req->snapshot_name);
	free(req->clone_image_name);
}

static void
rpc_bdev_fastblock_create_clone_from_snapshot(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_clone_snapshot_request req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_clone_snapshot_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_clone_snapshot_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->create_clone_from_snapshot_name(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		req.snapshot_name,
		req.clone_image_name);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_clone_snapshot_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_create_clone_from_snapshot", rpc_bdev_fastblock_create_clone_from_snapshot, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_delete_snapshot_by_name(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_snapshot_name_request req = {};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_snapshot_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_snapshot_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->delete_image_snapshot_by_name(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		req.snapshot_name);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_snapshot_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_delete_snapshot_by_name", rpc_bdev_fastblock_delete_snapshot_by_name, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_get_snapshot_by_name(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_snapshot_name_request req = {};
	struct spdk_bdev *bdev;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_snapshot_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_snapshot_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->monitor_client()->emplace_get_snapshot_id_by_name_request(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		req.snapshot_name,
		[request, blk_cli](const monitor::client::response_status status, monitor::client::request_context *req_ctx)
		{
			if (status != monitor::client::response_status::ok)
			{
				send_monitor_status_error(request, status);
				return;
			}
			auto &snapshot_id = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
			if (!snapshot_id)
			{
				spdk_jsonrpc_send_error_response(request, -EIO, spdk_strerror(EIO));
				return;
			}
			blk_cli->monitor_client()->emplace_get_snapshot_metadata_by_id_request(
				*snapshot_id,
				[request](const monitor::client::response_status status, monitor::client::request_context *req_ctx)
				{
					if (status != monitor::client::response_status::ok)
					{
						send_monitor_status_error(request, status);
						return;
					}
					auto &metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
					if (!metadata)
					{
						spdk_jsonrpc_send_error_response(request, -EIO, spdk_strerror(EIO));
						return;
					}
					auto *w = spdk_jsonrpc_begin_result(request);
					write_snapshot_metadata_json(w, *metadata);
					spdk_jsonrpc_end_result(request, w);
				});
		});

cleanup:
	free_rpc_bdev_fastblock_snapshot_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_get_snapshot_by_name", rpc_bdev_fastblock_get_snapshot_by_name, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_list_snapshots(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_name_request req = {};
	struct spdk_bdev *bdev;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_name_request_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_name_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL)
	{
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->monitor_client()->emplace_get_image_metadata_by_name_request(
		bdev_fastblock_get_pool_name(bdev),
		bdev_fastblock_get_image_name(bdev),
		[request, blk_cli](const monitor::client::response_status status, monitor::client::request_context *req_ctx)
		{
			if (status != monitor::client::response_status::ok)
			{
				send_monitor_status_error(request, status);
				return;
			}
			auto &metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
			if (!metadata)
			{
				spdk_jsonrpc_send_error_response(request, -EIO, spdk_strerror(EIO));
				return;
			}
			blk_cli->monitor_client()->emplace_list_snapshot_metadata_request(
				metadata->image_id,
				[request](const monitor::client::response_status status, monitor::client::request_context *req_ctx)
				{
					if (status != monitor::client::response_status::ok)
					{
						send_monitor_status_error(request, status);
						return;
					}
					auto &items = std::get<std::unique_ptr<monitor::client::snapshot_metadata_list>>(req_ctx->response_data);
					auto *w = spdk_jsonrpc_begin_result(request);
					spdk_json_write_array_begin(w);
					if (items)
					{
						for (const auto &item : items->data)
						{
							write_snapshot_metadata_json(w, item);
						}
					}
					spdk_json_write_array_end(w);
					spdk_jsonrpc_end_result(request, w);
				});
		});

cleanup:
	free_rpc_bdev_fastblock_name_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_list_snapshots", rpc_bdev_fastblock_list_snapshots, SPDK_RPC_RUNTIME)

struct rpc_snapshot_id_request
{
	char *snapshot_id;
};

static const struct spdk_json_object_decoder rpc_snapshot_id_request_decoders[] = {
	{"snapshot_id", offsetof(struct rpc_snapshot_id_request, snapshot_id), spdk_json_decode_string},
};

static void
free_rpc_snapshot_id_request(struct rpc_snapshot_id_request *req)
{
	free(req->snapshot_id);
}

static void
rpc_bdev_fastblock_protect_snapshot(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_snapshot_id_request req = {};
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_snapshot_id_request_decoders,
								SPDK_COUNTOF(rpc_snapshot_id_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->protect_snapshot(req.snapshot_id);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_snapshot_id_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_protect_snapshot", rpc_bdev_fastblock_protect_snapshot, SPDK_RPC_RUNTIME)

static void
rpc_bdev_fastblock_unprotect_snapshot(struct spdk_jsonrpc_request *request,
						  const struct spdk_json_val *params)
{
	struct rpc_snapshot_id_request req = {};
	struct spdk_json_write_ctx *w;
	auto blk_cli = get_management_blk_client();

	if (spdk_json_decode_object(params, rpc_snapshot_id_request_decoders,
								SPDK_COUNTOF(rpc_snapshot_id_request_decoders),
								&req))
	{
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
										 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!blk_cli)
	{
		spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(EBUSY));
		goto cleanup;
	}

	blk_cli->unprotect_snapshot(req.snapshot_id);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_snapshot_id_request(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_unprotect_snapshot", rpc_bdev_fastblock_unprotect_snapshot, SPDK_RPC_RUNTIME)
