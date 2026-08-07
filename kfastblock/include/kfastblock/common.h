#ifndef KFASTBLOCK_COMMON_H
#define KFASTBLOCK_COMMON_H

#include <linux/types.h>

#define KFASTBLOCK_DRV_NAME "kfastblock"             /* kernel module / bus name */
#define KFASTBLOCK_DRV_NAME_PREFIX "kfb"               /* short alias for device names */
#define KFASTBLOCK_SYSFS_DEV_PREFIX "kfastblock-vol-"  /* sysfs device name prefix */

#define KFASTBLOCK_DEVICE_PART_SHIFT 4                 /* partition bits in minor number */
#define KFASTBLOCK_DEFAULT_BLOCK_SIZE 4096U            /* default logical block size (bytes) */
#define KFASTBLOCK_DEFAULT_OBJECT_SIZE (4U * 1024U * 1024U)  /* default object size (4 MiB) */
#define KFASTBLOCK_DEFAULT_QUEUE_DEPTH 128U            /* default I/O queue depth per volume */
#define KFASTBLOCK_MAX_IO_BYTES (4U * 1024U * 1024U)  /* maximum single I/O size (4 MiB) */
#define KFASTBLOCK_DEFAULT_MONITOR_PORT 3334U          /* default raw monitor TCP port */
#define KFASTBLOCK_DEFAULT_SOCKET_TIMEOUT_MS 3000U     /* socket connect/recv timeout (ms) */
#define KFASTBLOCK_DEFAULT_TRANSPORT_MAX_ACTIVE 32U    /* max concurrent transport requests */
#define KFASTBLOCK_DEFAULT_OBJECT_DISPATCH_WINDOW 8U   /* outstanding object dispatch slots */
#define KFASTBLOCK_DEFAULT_REFRESH_INTERVAL_MS 3000U   /* cluster map refresh interval (ms) */
#define KFASTBLOCK_DEFAULT_IMAGE_REFRESH_INTERVAL_MS 30000U  /* image info refresh interval (ms) */

#define KFASTBLOCK_MAX_NAME_LEN 128  /* max length for image/pool names */
#define KFASTBLOCK_MAX_ADDR_LEN 256  /* max length for network address strings */
#define KFASTBLOCK_LAST_ERROR_LEN 256  /* buffer size for last error message */
#define KFASTBLOCK_MAX_MONITORS 8     /* maximum number of monitor connections */

#endif
