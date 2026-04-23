#ifndef KFASTBLOCK_RAWPROTO_H
#define KFASTBLOCK_RAWPROTO_H

#include <linux/types.h>

#define KFASTBLOCK_RAW_MAGIC 0x46425257U
#define KFASTBLOCK_RAW_VERSION_MAJOR 1U
#define KFASTBLOCK_RAW_VERSION_MINOR 0U

#define KFASTBLOCK_RAW_SERVICE_MONITOR 1U
#define KFASTBLOCK_RAW_SERVICE_OSD 2U

#define KFASTBLOCK_RAW_OP_GET_IMAGE_INFO 1U
#define KFASTBLOCK_RAW_OP_GET_CLUSTER_MAP 2U

#define KFASTBLOCK_RAW_OSD_OP_GET_LEADER 1U
#define KFASTBLOCK_RAW_OSD_OP_READ_OBJECT 2U
#define KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT 3U
#define KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT 4U

#define KFASTBLOCK_RAW_FLAG_RESPONSE (1U << 0)

#define KFASTBLOCK_RAW_STATUS_OK 0U
#define KFASTBLOCK_RAW_STATUS_INVALID_REQUEST 1U
#define KFASTBLOCK_RAW_STATUS_NOT_FOUND 2U
#define KFASTBLOCK_RAW_STATUS_STALE_EPOCH 3U
#define KFASTBLOCK_RAW_STATUS_RETRY_LATER 4U
#define KFASTBLOCK_RAW_STATUS_NOT_LEADER 5U
#define KFASTBLOCK_RAW_STATUS_PG_INITIALIZING 6U
#define KFASTBLOCK_RAW_STATUS_OSD_DOWN 7U
#define KFASTBLOCK_RAW_STATUS_INTERNAL_ERROR 8U

#define KFASTBLOCK_RAW_IMAGE_FLAG_READ_ONLY (1U << 0)

struct kfastblock_raw_header {
	__le32 magic;
	u8 version_major;
	u8 version_minor;
	u8 service;
	u8 opcode;
	__le32 flags;
	__le64 seq;
	__le32 status;
	__le32 body_len;
} __packed;

struct kfastblock_raw_get_image_info_req {
	__le64 image_epoch;
	__le16 pool_name_len;
	__le16 image_name_len;
} __packed;

struct kfastblock_raw_get_image_info_rsp {
	__le64 image_epoch;
	__le32 pool_id;
	__le32 block_size;
	__le32 object_size;
	__le32 flags;
	__le64 size_bytes;
} __packed;

struct kfastblock_raw_get_cluster_map_req {
	__le64 osdmap_epoch;
	__le64 pgmap_epoch;
} __packed;

struct kfastblock_raw_cluster_map_rsp_hdr {
	__le64 osdmap_epoch;
	__le64 pgmap_epoch;
	__le32 osd_count;
	__le32 pg_count;
} __packed;

struct kfastblock_raw_osd_entry_hdr {
	__le32 osd_id;
	__le32 flags;
	__le16 address_len;
	__le16 shard_count;
} __packed;

struct kfastblock_raw_osd_shard_entry {
	__le32 shard_id;
	__le16 port;
	__le16 core_id;
} __packed;

struct kfastblock_raw_pg_entry_hdr {
	__le32 pool_id;
	__le32 pg_id;
	__le64 version;
	__le32 state;
	__le32 primary_shard;
	__le32 replica_count;
} __packed;

struct kfastblock_raw_get_leader_req {
	__le32 pool_id;
	__le32 pg_id;
} __packed;

struct kfastblock_raw_get_leader_rsp {
	__le32 leader_id;
	__le16 leader_port;
	__le16 address_len;
} __packed;

struct kfastblock_raw_read_object_req {
	__le32 pool_id;
	__le32 pg_id;
	__le64 offset;
	__le32 length;
	__le16 object_name_len;
	__le16 reserved;
} __packed;

struct kfastblock_raw_read_object_rsp {
	__le32 data_len;
	__le32 reserved;
} __packed;

struct kfastblock_raw_write_object_req {
	__le32 pool_id;
	__le32 pg_id;
	__le64 offset;
	__le32 data_len;
	__le16 object_name_len;
	__le16 reserved;
} __packed;

struct kfastblock_raw_delete_object_req {
	__le32 pool_id;
	__le32 pg_id;
	__le16 object_name_len;
	__le16 reserved;
} __packed;

#endif
