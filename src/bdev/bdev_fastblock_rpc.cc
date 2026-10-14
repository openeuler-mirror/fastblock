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
#include <spdk/rpc.h>
#include <spdk/util.h>
#include <spdk/string.h>
#include <spdk/log.h>

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
	uint64_t iops_limit;
	uint64_t bw_limit_mib_per_sec;
};

static void
free_rpc_create_fastblock(struct rpc_create_fastblock *req)
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
	{"monitor_address", offsetof(struct rpc_create_fastblock, monitor_address), spdk_json_decode_string},
	{"iops_limit", offsetof(struct rpc_create_fastblock, iops_limit), spdk_json_decode_uint64, true},
	{"bw_limit_mib_per_sec", offsetof(struct rpc_create_fastblock, bw_limit_mib_per_sec), spdk_json_decode_uint64, true}};

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
							   req.monitor_address,
							   req.iops_limit,
							   req.bw_limit_mib_per_sec);
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

struct rpc_bdev_fastblock_qos
{
	char *name;
	uint64_t iops_limit;
	uint64_t bw_limit_mib_per_sec;
};

static void
free_rpc_bdev_fastblock_qos(struct rpc_bdev_fastblock_qos *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_fastblock_update_qos_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_qos, name), spdk_json_decode_string},
	{"iops_limit", offsetof(struct rpc_bdev_fastblock_qos, iops_limit), spdk_json_decode_uint64},
	{"bw_limit_mib_per_sec", offsetof(struct rpc_bdev_fastblock_qos, bw_limit_mib_per_sec), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_fastblock_update_qos_cb(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = (struct spdk_jsonrpc_request *)cb_arg;

	if (status != 0)
	{
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_fastblock_update_qos(struct spdk_jsonrpc_request *request,
							  const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_qos req = {};
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_update_qos_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_update_qos_decoders),
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

	rc = bdev_fastblock_update_qos(bdev, req.iops_limit, req.bw_limit_mib_per_sec,
								   rpc_bdev_fastblock_update_qos_cb, request);
	if (rc)
	{
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	free_rpc_bdev_fastblock_qos(&req);
	return;

cleanup:
	free_rpc_bdev_fastblock_qos(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_update_qos", rpc_bdev_fastblock_update_qos, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_fastblock_get_qos_decoders[] = {
	{"name", offsetof(struct rpc_bdev_fastblock_qos, name), spdk_json_decode_string},
};

static void
rpc_bdev_fastblock_get_qos(struct spdk_jsonrpc_request *request,
						   const struct spdk_json_val *params)
{
	struct rpc_bdev_fastblock_qos req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	uint64_t iops_limit = 0;
	uint64_t bw_limit_mib_per_sec = 0;
	uint64_t spdk_iops_limit = 0;
	uint64_t spdk_bw_limit_mib_per_sec = 0;
	uint64_t spdk_read_bw_limit_mib_per_sec = 0;
	uint64_t spdk_write_bw_limit_mib_per_sec = 0;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_fastblock_get_qos_decoders,
								SPDK_COUNTOF(rpc_bdev_fastblock_get_qos_decoders),
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

	rc = bdev_fastblock_get_qos(bdev,
								&iops_limit,
								&bw_limit_mib_per_sec,
								&spdk_iops_limit,
								&spdk_bw_limit_mib_per_sec,
								&spdk_read_bw_limit_mib_per_sec,
								&spdk_write_bw_limit_mib_per_sec);
	if (rc)
	{
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_object_begin(w, "configured");
	spdk_json_write_named_uint64(w, "iops_limit", iops_limit);
	spdk_json_write_named_uint64(w, "bw_limit_mib_per_sec", bw_limit_mib_per_sec);
	spdk_json_write_object_end(w);
	spdk_json_write_named_object_begin(w, "spdk_assigned");
	spdk_json_write_named_uint64(w, "rw_ios_per_sec", spdk_iops_limit);
	spdk_json_write_named_uint64(w, "rw_mbytes_per_sec", spdk_bw_limit_mib_per_sec);
	spdk_json_write_named_uint64(w, "r_mbytes_per_sec", spdk_read_bw_limit_mib_per_sec);
	spdk_json_write_named_uint64(w, "w_mbytes_per_sec", spdk_write_bw_limit_mib_per_sec);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_fastblock_qos(&req);
}

SPDK_RPC_REGISTER("bdev_fastblock_get_qos", rpc_bdev_fastblock_get_qos, SPDK_RPC_RUNTIME)
