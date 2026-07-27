#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSFS_PATH_ATTACH "/sys/bus/kfastblock/attach"
#define SYSFS_PATH_DETACH "/sys/bus/kfastblock/detach"
#define SYSFS_PATH_LAST_ERROR "/sys/bus/kfastblock/last_error"
#define SYSFS_DEVICE_ROOT "/sys/bus/kfastblock/devices"
#define SYSFS_DEVICE_PREFIX "kfastblock-vol-"
#define MODULE_PARAM_ROOT "/sys/module/kfastblock/parameters"

#define MAX_LINE_LEN 1024
#define MAX_CMD_LEN 4096
#define MAX_SYSFS_PATH 4096

struct config {
	char *monitor_addr;
	char *pool_name;
	char *image_name;
	char *conf_file;
	char *read_only;
	/* OSD data-plane: tcp | rdma | auto (default tcp in kernel) */
	char *osd_transport;
	char *debug_size_bytes;
	char *debug_object_size;
	char *debug_pool_id;
	char *debug_pg_count;
	char *scope;
	char *value;
};

static int build_volume_root_path(char *buf, size_t buf_len,
				  const struct config *cfg);
static int print_volume_attr(const char *root_path, const char *attr);

static void trim(char *str)
{
	char *start;
	char *end;

	if (!str)
		return;

	for (start = str; *start && isspace((unsigned char)*start); ++start)
		;

	memmove(str, start, strlen(start) + 1);
	if (*str == '\0')
		return;

	for (end = str + strlen(str) - 1; end > str &&
	     isspace((unsigned char)*end); --end)
		*end = '\0';
}

static void maybe_set(char **dst, const char *value)
{
	free(*dst);
	*dst = strdup(value);
}

static void parse_config_file(const char *filename, struct config *cfg)
{
	FILE *fp;
	char line[MAX_LINE_LEN];

	fp = fopen(filename, "r");
	if (!fp) {
		perror("Failed to open config file");
		exit(EXIT_FAILURE);
	}

	while (fgets(line, sizeof(line), fp)) {
		char *key = strtok(line, "=");
		char *value = strtok(NULL, "\n");

		if (!key || !value)
			continue;

		trim(key);
		trim(value);

		if (strcmp(key, "monitor_addr") == 0) {
			maybe_set(&cfg->monitor_addr, value);
		} else if (strcmp(key, "pool_name") == 0) {
			maybe_set(&cfg->pool_name, value);
		} else if (strcmp(key, "image_name") == 0) {
			maybe_set(&cfg->image_name, value);
		} else if (strcmp(key, "read_only") == 0) {
			maybe_set(&cfg->read_only, value);
		} else if (strcmp(key, "osd_transport") == 0) {
			maybe_set(&cfg->osd_transport, value);
		} else if (strcmp(key, "debug_size_bytes") == 0) {
			maybe_set(&cfg->debug_size_bytes, value);
		} else if (strcmp(key, "debug_object_size") == 0) {
			maybe_set(&cfg->debug_object_size, value);
		} else if (strcmp(key, "debug_pool_id") == 0) {
			maybe_set(&cfg->debug_pool_id, value);
		} else if (strcmp(key, "debug_pg_count") == 0) {
			maybe_set(&cfg->debug_pg_count, value);
		} else if (strcmp(key, "scope") == 0) {
			maybe_set(&cfg->scope, value);
		} else if (strcmp(key, "value") == 0) {
			maybe_set(&cfg->value, value);
		}
	}

	fclose(fp);
}

static void print_usage(const char *prog_name)
{
	fprintf(stderr,
		"Usage: %s <attach|detach|force-refresh|reset-backoff|drop-transport|flush-rdma-cache|reset-leaders|pause-queue|resume-queue|set-dispatch-window|set-refresh-interval|set-image-refresh-interval|list|show|show-rdma-params|show-xport|show-transport|show-rdma-pool> [options]\n",
		prog_name);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -c, --conf <file>\n");
	fprintf(stderr, "  --monitor-addr <addr[,addr...]>\n");
	fprintf(stderr, "  --pool-name <name>\n");
	fprintf(stderr, "  --image-name <name>\n");
	fprintf(stderr, "  --read-only <true|false>\n");
	fprintf(stderr,
		"  --osd-transport <tcp|rdma|auto>  OSD data-plane preference\n");
	fprintf(stderr, "  --debug-size-bytes <bytes>\n");
	fprintf(stderr, "  --debug-object-size <bytes>\n");
	fprintf(stderr, "  --debug-pool-id <id>\n");
	fprintf(stderr, "  --debug-pg-count <count>\n");
	fprintf(stderr, "  --scope <all|cluster|image|osd|monitor>\n");
	fprintf(stderr, "  --value <number>\n");
	fprintf(stderr, "\nExamples:\n");
	fprintf(stderr, "  %s list\n", prog_name);
	fprintf(stderr, "  %s show --pool-name <pool> --image-name <image>\n", prog_name);
	fprintf(stderr,
		"  %s attach --monitor-addr <m> --pool-name <p> --image-name <i> --osd-transport auto\n",
		prog_name);
	fprintf(stderr, "  %s show-rdma-params\n", prog_name);
	fprintf(stderr,
		"  %s show-xport --pool-name <pool> --image-name <image>\n",
		prog_name);
	fprintf(stderr,
		"  %s show-transport --pool-name <pool> --image-name <image>\n",
		prog_name);
	fprintf(stderr,
		"  %s show-rdma-pool --pool-name <pool> --image-name <image>\n",
		prog_name);
	fprintf(stderr,
		"  %s flush-rdma-cache --pool-name <pool> --image-name <image>\n",
		prog_name);
	fprintf(stderr, "  %s pause-queue --pool-name <pool> --image-name <image>\n", prog_name);
	fprintf(stderr, "  %s set-dispatch-window --pool-name <pool> --image-name <image> --value 16\n", prog_name);
}

static void append_kv(char *buf, size_t buf_len, const char *key,
		      const char *value)
{
	if (!value || !*value)
		return;

	if (buf[0] != '\0')
		strncat(buf, " ", buf_len - strlen(buf) - 1);

	strncat(buf, key, buf_len - strlen(buf) - 1);
	strncat(buf, "=", buf_len - strlen(buf) - 1);
	strncat(buf, value, buf_len - strlen(buf) - 1);
}

static int write_sysfs_file(const char *path, const char *value)
{
	int fd;
	size_t len;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	len = value ? strlen(value) : 0;
	if (write(fd, value, len) < 0) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static int read_text_file(const char *path, char *buf, size_t buf_len)
{
	int fd;
	ssize_t bytes_read;

	if (!path || !buf || !buf_len)
		return -1;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	bytes_read = read(fd, buf, buf_len - 1);
	close(fd);
	if (bytes_read < 0)
		return -1;

	buf[bytes_read] = '\0';
	trim(buf);
	return 0;
}

static void print_driver_last_error(void)
{
	char error_buf[256] = {0};

	if (read_text_file(SYSFS_PATH_LAST_ERROR, error_buf, sizeof(error_buf)) == 0 &&
	    error_buf[0] != '\0') {
		fprintf(stderr, "Driver error: %s\n", error_buf);
	}
}

static int build_volume_sysfs_path(char *buf, size_t buf_len,
				  const struct config *cfg,
				  const char *attr)
{
	if (!cfg || !cfg->pool_name || !cfg->image_name || !attr)
		return -1;

	if (snprintf(buf, buf_len, "%s/%s%s-%s/%s", SYSFS_DEVICE_ROOT,
		     SYSFS_DEVICE_PREFIX, cfg->pool_name, cfg->image_name,
		     attr) >= (int)buf_len)
		return -1;

	return 0;
}

static int op_is_volume_level(const char *operation)
{
	return strcmp(operation, "force-refresh") == 0 ||
		strcmp(operation, "reset-backoff") == 0 ||
		strcmp(operation, "drop-transport") == 0 ||
		strcmp(operation, "flush-rdma-cache") == 0 ||
		strcmp(operation, "reset-leaders") == 0 ||
		strcmp(operation, "pause-queue") == 0 ||
		strcmp(operation, "resume-queue") == 0 ||
		strcmp(operation, "set-dispatch-window") == 0 ||
		strcmp(operation, "set-refresh-interval") == 0 ||
		strcmp(operation, "set-image-refresh-interval") == 0;
}

static int op_is_read_only(const char *operation)
{
	return strcmp(operation, "list") == 0 ||
		strcmp(operation, "show") == 0 ||
		strcmp(operation, "show-rdma-params") == 0 ||
		strcmp(operation, "show-xport") == 0 ||
		strcmp(operation, "show-transport") == 0 ||
		strcmp(operation, "show-rdma-pool") == 0;
}

static int build_volume_root_path(char *buf, size_t buf_len,
				  const struct config *cfg);
static int print_volume_attr(const char *root_path, const char *attr);

/*
 * Filter debugfs diagnostics for xport.* lines (transport preference / RDMA).
 * Path: /sys/kernel/debug/kfastblock/<disk>/diagnostics
 */
static int do_show_xport(const struct config *cfg)
{
	DIR *dir;
	struct dirent *de;
	char path[MAX_SYSFS_PATH];
	char line[MAX_LINE_LEN];
	int shown = 0;

	if (!cfg || !cfg->pool_name || !cfg->image_name) {
		fprintf(stderr,
			"pool-name and image-name are required for show-xport\n");
		return -1;
	}

	dir = opendir("/sys/kernel/debug/kfastblock");
	if (!dir) {
		fprintf(stderr,
			"debugfs kfastblock not available (mount debugfs? module loaded?)\n");
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		char vol_pool[MAX_LINE_LEN] = {0};
		char vol_image[MAX_LINE_LEN] = {0};
		FILE *fp;
		int match = 0;

		if (de->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path),
			     "/sys/kernel/debug/kfastblock/%s/diagnostics",
			     de->d_name) >= (int)sizeof(path))
			continue;
		fp = fopen(path, "r");
		if (!fp)
			continue;

		while (fgets(line, sizeof(line), fp)) {
			if (strncmp(line, "volume.pool=", 12) == 0) {
				snprintf(vol_pool, sizeof(vol_pool), "%s",
					 line + 12);
				trim(vol_pool);
			} else if (strncmp(line, "volume.image=", 13) == 0) {
				snprintf(vol_image, sizeof(vol_image), "%s",
					 line + 13);
				trim(vol_image);
			}
		}
		match = (strcmp(vol_pool, cfg->pool_name) == 0 &&
			 strcmp(vol_image, cfg->image_name) == 0);
		if (match) {
			printf("disk=%s\n", de->d_name);
			rewind(fp);
			while (fgets(line, sizeof(line), fp)) {
				if (strncmp(line, "xport.", 6) == 0 ||
				    strncmp(line, "diagnostics.anomaly_flags", 25) == 0) {
					fputs(line, stdout);
					shown++;
				}
			}
		}
		fclose(fp);
		if (match)
			break;
	}
	closedir(dir);

	if (!shown) {
		fprintf(stderr,
			"no xport diagnostics found for pool=%s image=%s\n",
			cfg->pool_name, cfg->image_name);
		return -1;
	}
	return 0;
}

/*
 * Dump RDMA-related module parameters from sysfs so operators can see
 * timeouts and data-plane counters without grepping dmesg.
 */
static int do_show_rdma_params(void)
{
	static const char *const names[] = {
		"rdma_pool_enable",
		"rdma_cm_timeout_ms",
		"rdma_io_timeout_ms",
		"rdma_recv_depth",
		"rdma_use_cq_notify",
		"rdma_qp_max_send_wr",
		"rdma_qp_max_recv_wr",
		"rdma_retry_count",
		"rdma_rnr_retry_count",
		"rdma_signal_all",
		"rdma_send_ok",
		"rdma_send_err",
		"rdma_recv_ok",
		"rdma_recv_err",
		"rdma_exchange_ok",
		"rdma_exchange_err",
		"rdma_connect_ok",
		"rdma_connect_err",
		"rdma_dma_map_err",
		"xport_probe_cache_ttl_ms",
		"xport_probe_cache_hits",
		"xport_probe_cache_misses",
	};
	char path[MAX_SYSFS_PATH];
	char value[MAX_LINE_LEN];
	size_t i;
	int shown = 0;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		if (snprintf(path, sizeof(path), "%s/%s", MODULE_PARAM_ROOT,
			     names[i]) >= (int)sizeof(path))
			continue;
		if (read_text_file(path, value, sizeof(value)) < 0)
			continue;
		printf("%s=%s\n", names[i], value);
		shown++;
	}
	if (!shown) {
		fprintf(stderr,
			"no RDMA module params found (is kfastblock loaded?)\n");
		return -1;
	}
	return 0;
}

/* Print volume osd_transport preference (tcp|rdma|auto). */
static int do_show_transport(const struct config *cfg)
{
	char root_path[MAX_SYSFS_PATH];

	if (build_volume_root_path(root_path, sizeof(root_path), cfg) != 0) {
		fprintf(stderr,
			"pool-name and image-name are required for show-transport\n");
		return -1;
	}
	if (print_volume_attr(root_path, "osd_transport") < 0) {
		fprintf(stderr,
			"failed to read osd_transport (volume attached?)\n");
		return -1;
	}
	return 0;
}

/* Print RDMA connection-cache snapshot for one volume. */
static int do_show_rdma_pool(const struct config *cfg)
{
	char root_path[MAX_SYSFS_PATH];

	if (build_volume_root_path(root_path, sizeof(root_path), cfg) != 0) {
		fprintf(stderr,
			"pool-name and image-name are required for show-rdma-pool\n");
		return -1;
	}
	if (print_volume_attr(root_path, "rdma_cache_stats") < 0) {
		fprintf(stderr,
			"failed to read rdma_cache_stats (volume attached?)\n");
		return -1;
	}
	return 0;
}

static const char *volume_attr_for_operation(const char *operation)
{
	if (strcmp(operation, "force-refresh") == 0)
		return "force_refresh";
	if (strcmp(operation, "reset-backoff") == 0)
		return "reset_backoff";
	if (strcmp(operation, "drop-transport") == 0)
		return "drop_transport";
	if (strcmp(operation, "flush-rdma-cache") == 0)
		return "flush_rdma_cache";
	if (strcmp(operation, "reset-leaders") == 0)
		return "reset_leaders";
	if (strcmp(operation, "pause-queue") == 0)
		return "pause_queue";
	if (strcmp(operation, "resume-queue") == 0)
		return "resume_queue";
	if (strcmp(operation, "set-dispatch-window") == 0)
		return "dispatch_window";
	if (strcmp(operation, "set-refresh-interval") == 0)
		return "refresh_interval_ms";
	if (strcmp(operation, "set-image-refresh-interval") == 0)
		return "image_refresh_interval_ms";
	return NULL;
}

static void free_config(struct config *cfg)
{
	if (!cfg)
		return;
	free(cfg->monitor_addr);
	free(cfg->pool_name);
	free(cfg->image_name);
	free(cfg->conf_file);
	free(cfg->read_only);
	free(cfg->osd_transport);
	free(cfg->debug_size_bytes);
	free(cfg->debug_object_size);
	free(cfg->debug_pool_id);
	free(cfg->debug_pg_count);
	free(cfg->scope);
	free(cfg->value);
}

static int build_volume_root_path(char *buf, size_t buf_len,
				  const struct config *cfg)
{
	if (!cfg || !cfg->pool_name || !cfg->image_name)
		return -1;

	if (snprintf(buf, buf_len, "%s/%s%s-%s",
		     SYSFS_DEVICE_ROOT, SYSFS_DEVICE_PREFIX,
		     cfg->pool_name, cfg->image_name) >= (int)buf_len)
		return -1;

	return 0;
}

static int print_volume_attr(const char *root_path, const char *attr)
{
	char path[MAX_SYSFS_PATH];
	char value[MAX_LINE_LEN];

	if (!root_path || !attr)
		return -1;
	if (snprintf(path, sizeof(path), "%s/%s", root_path, attr) >= (int)sizeof(path))
		return -1;
	if (read_text_file(path, value, sizeof(value)) < 0)
		return -1;

	printf("%s=%s\n", attr, value);
	return 0;
}

static int do_list_volumes(void)
{
		static const char *attrs[] = {
			"health_state",
			"sync_state",
			"queue_paused",
			"manual_queue_pause",
			"open_count",
			"osd_transport",
		};
	DIR *dir;
	struct dirent *de;
	char path[MAX_SYSFS_PATH];
	char pool[MAX_LINE_LEN];
	char image[MAX_LINE_LEN];
	char value[MAX_LINE_LEN];
	size_t i;

	dir = opendir(SYSFS_DEVICE_ROOT);
	if (!dir) {
		perror("Failed to open kfastblock device root");
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (strncmp(de->d_name, SYSFS_DEVICE_PREFIX,
			    strlen(SYSFS_DEVICE_PREFIX)) != 0)
			continue;

		if (snprintf(path, sizeof(path), "%s/%s", SYSFS_DEVICE_ROOT,
			     de->d_name) >= (int)sizeof(path))
			continue;
		if (snprintf(value, sizeof(value), "%s/pool_name", path) >= (int)sizeof(value))
			continue;
		if (read_text_file(value, pool, sizeof(pool)) < 0)
			strcpy(pool, "?");
		if (snprintf(value, sizeof(value), "%s/image_name", path) >= (int)sizeof(value))
			continue;
		if (read_text_file(value, image, sizeof(image)) < 0)
			strcpy(image, "?");

		printf("%s pool=%s image=%s", de->d_name, pool, image);
		for (i = 0; i < sizeof(attrs) / sizeof(attrs[0]); ++i) {
			if (snprintf(value, sizeof(value), "%s/%s", path, attrs[i]) >= (int)sizeof(value))
				continue;
			if (read_text_file(value, pool, sizeof(pool)) == 0)
				printf(" %s=%s", attrs[i], pool);
		}
		putchar('\n');
	}

	closedir(dir);
	return 0;
}

static int do_show_volume(const struct config *cfg)
{
	static const char *attrs[] = {
		"pool_name",
		"image_name",
		"size_bytes",
		"object_size",
		"dispatch_window",
		"refresh_interval_ms",
		"image_refresh_interval_ms",
		"pool_id",
		"pg_count",
		"osd_count",
		"route_count",
		"osdmap_epoch",
		"pgmap_epoch",
		"leader_epoch",
		"last_refresh",
		"last_image_refresh",
		"read_only",
		"open_count",
		"sync_state",
		"queue_paused",
		"manual_queue_pause",
		"flush_in_progress",
		"inflight_ios",
		"health_state",
		"health_since",
		"last_failure_errno",
		"last_failure_source",
		"last_failure_jiffies",
		"last_success_jiffies",
		/* OSD data-plane transport preference and RDMA cache */
		"osd_transport",
		"rdma_cache_stats",
		"io_submitted",
		"io_completed",
		"io_failed",
		"object_io_completed",
		"object_io_retries",
		"object_io_errors",
		"cluster_refresh_ok",
		"cluster_refresh_fail",
		"image_refresh_ok",
		"image_refresh_fail",
		"leader_query_ok",
		"leader_query_fail",
		"refresh_kicks",
		"leader_invalidations",
		"osd_socket_drops",
		"monitor_socket_drops",
		"manual_queue_pauses",
		"manual_queue_resumes",
	};
	char root_path[MAX_SYSFS_PATH];
	size_t i;

	if (build_volume_root_path(root_path, sizeof(root_path), cfg) != 0) {
		fprintf(stderr, "pool-name and image-name are required for show\n");
		return -1;
	}

	for (i = 0; i < sizeof(attrs) / sizeof(attrs[0]); ++i)
		print_volume_attr(root_path, attrs[i]);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *operation;
	const char *sysfs_path;
	struct config cfg = {0};
	char command_str[MAX_CMD_LEN] = {0};
	char sysfs_path_buf[MAX_SYSFS_PATH] = {0};
	int fd;
	int opt;
	int option_index = 0;
	int volume_level;

	static struct option long_options[] = {
		{"conf", required_argument, 0, 'c'},
		{"monitor-addr", required_argument, 0, 0},
		{"pool-name", required_argument, 0, 0},
		{"image-name", required_argument, 0, 0},
		{"read-only", required_argument, 0, 0},
		{"osd-transport", required_argument, 0, 0},
		{"debug-size-bytes", required_argument, 0, 0},
		{"debug-object-size", required_argument, 0, 0},
		{"debug-pool-id", required_argument, 0, 0},
		{"debug-pg-count", required_argument, 0, 0},
		{"scope", required_argument, 0, 0},
		{"value", required_argument, 0, 0},
		{0, 0, 0, 0},
	};

	if (argc < 2) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	operation = argv[1];
	if (strcmp(operation, "attach") != 0 &&
	    strcmp(operation, "detach") != 0 &&
	    strcmp(operation, "force-refresh") != 0 &&
	    strcmp(operation, "reset-backoff") != 0 &&
	    strcmp(operation, "drop-transport") != 0 &&
	    strcmp(operation, "flush-rdma-cache") != 0 &&
	    strcmp(operation, "reset-leaders") != 0 &&
	    strcmp(operation, "pause-queue") != 0 &&
	    strcmp(operation, "resume-queue") != 0 &&
	    strcmp(operation, "set-dispatch-window") != 0 &&
	    strcmp(operation, "set-refresh-interval") != 0 &&
	    strcmp(operation, "set-image-refresh-interval") != 0 &&
	    strcmp(operation, "list") != 0 &&
	    strcmp(operation, "show") != 0 &&
	    strcmp(operation, "show-rdma-params") != 0 &&
	    strcmp(operation, "show-xport") != 0 &&
	    strcmp(operation, "show-transport") != 0 &&
	    strcmp(operation, "show-rdma-pool") != 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	++argv;
	--argc;
	volume_level = op_is_volume_level(operation);

	while ((opt = getopt_long(argc, argv, "c:", long_options,
				  &option_index)) != -1) {
		switch (opt) {
		case 0:
			if (strcmp(long_options[option_index].name,
				   "monitor-addr") == 0) {
				maybe_set(&cfg.monitor_addr, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "pool-name") == 0) {
				maybe_set(&cfg.pool_name, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "image-name") == 0) {
				maybe_set(&cfg.image_name, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "read-only") == 0) {
				maybe_set(&cfg.read_only, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "osd-transport") == 0) {
				maybe_set(&cfg.osd_transport, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "debug-size-bytes") == 0) {
				maybe_set(&cfg.debug_size_bytes, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "debug-object-size") == 0) {
				maybe_set(&cfg.debug_object_size, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "debug-pool-id") == 0) {
				maybe_set(&cfg.debug_pool_id, optarg);
			} else if (strcmp(long_options[option_index].name,
				  "debug-pg-count") == 0) {
				maybe_set(&cfg.debug_pg_count, optarg);
				} else if (strcmp(long_options[option_index].name,
					  "scope") == 0) {
					maybe_set(&cfg.scope, optarg);
				} else if (strcmp(long_options[option_index].name,
					  "value") == 0) {
					maybe_set(&cfg.value, optarg);
				}
				break;
		case 'c':
			cfg.conf_file = strdup(optarg);
			break;
		default:
			print_usage(argv[0]);
			free_config(&cfg);
			return EXIT_FAILURE;
		}
	}

	if (cfg.conf_file)
		parse_config_file(cfg.conf_file, &cfg);

	if (op_is_read_only(operation)) {
		int ret;

		if (strcmp(operation, "list") == 0)
			ret = do_list_volumes();
		else if (strcmp(operation, "show-rdma-params") == 0)
			ret = do_show_rdma_params();
		else if (strcmp(operation, "show-xport") == 0)
			ret = do_show_xport(&cfg);
		else if (strcmp(operation, "show-transport") == 0)
			ret = do_show_transport(&cfg);
		else if (strcmp(operation, "show-rdma-pool") == 0)
			ret = do_show_rdma_pool(&cfg);
		else
			ret = do_show_volume(&cfg);
		free_config(&cfg);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (!cfg.pool_name || !cfg.image_name) {
		fprintf(stderr, "pool-name and image-name are required\n");
		free_config(&cfg);
		return EXIT_FAILURE;
	}

	if (strcmp(operation, "attach") == 0 && !cfg.monitor_addr) {
		fprintf(stderr, "monitor-addr is required for attach\n");
		free_config(&cfg);
		return EXIT_FAILURE;
	}
	if (cfg.osd_transport &&
	    strcmp(cfg.osd_transport, "tcp") != 0 &&
	    strcmp(cfg.osd_transport, "rdma") != 0 &&
	    strcmp(cfg.osd_transport, "auto") != 0) {
		fprintf(stderr,
			"osd-transport must be tcp, rdma, or auto (got %s)\n",
			cfg.osd_transport);
		free_config(&cfg);
		return EXIT_FAILURE;
	}
	if ((strcmp(operation, "set-dispatch-window") == 0 ||
	     strcmp(operation, "set-refresh-interval") == 0 ||
	     strcmp(operation, "set-image-refresh-interval") == 0) &&
	    (!cfg.value || !*cfg.value)) {
		fprintf(stderr, "--value is required for %s\n", operation);
		free_config(&cfg);
		return EXIT_FAILURE;
	}

	if (!volume_level) {
		append_kv(command_str, sizeof(command_str), "monitor_addr",
			  cfg.monitor_addr);
		append_kv(command_str, sizeof(command_str), "pool_name",
			  cfg.pool_name);
		append_kv(command_str, sizeof(command_str), "image_name",
			  cfg.image_name);
		append_kv(command_str, sizeof(command_str), "read_only",
			  cfg.read_only);
		append_kv(command_str, sizeof(command_str), "osd_transport",
			  cfg.osd_transport);
		append_kv(command_str, sizeof(command_str), "debug_size_bytes",
			  cfg.debug_size_bytes);
		append_kv(command_str, sizeof(command_str), "debug_object_size",
			  cfg.debug_object_size);
		append_kv(command_str, sizeof(command_str), "debug_pool_id",
			  cfg.debug_pool_id);
		append_kv(command_str, sizeof(command_str), "debug_pg_count",
			  cfg.debug_pg_count);
		sysfs_path = strcmp(operation, "attach") == 0 ?
			SYSFS_PATH_ATTACH : SYSFS_PATH_DETACH;
	} else {
		const char *attr = volume_attr_for_operation(operation);

		if (!attr || build_volume_sysfs_path(sysfs_path_buf,
					    sizeof(sysfs_path_buf), &cfg, attr) != 0) {
			fprintf(stderr, "failed to build volume sysfs path\n");
			free_config(&cfg);
			return EXIT_FAILURE;
		}
		sysfs_path = sysfs_path_buf;
		if (strcmp(operation, "set-dispatch-window") == 0 ||
		    strcmp(operation, "set-refresh-interval") == 0 ||
		    strcmp(operation, "set-image-refresh-interval") == 0)
			strncpy(command_str, cfg.value, sizeof(command_str) - 1);
		else
			strncpy(command_str, cfg.scope ? cfg.scope : "all",
				sizeof(command_str) - 1);
	}

	fd = open(sysfs_path, O_WRONLY);
	if (fd < 0) {
		perror("Failed to open sysfs file");
		free_config(&cfg);
		return EXIT_FAILURE;
	}
	close(fd);

	if (write_sysfs_file(sysfs_path, command_str) < 0) {
		perror("Failed to write to sysfs file");
		print_driver_last_error();
		free_config(&cfg);
		return EXIT_FAILURE;
	}

	free_config(&cfg);
	return EXIT_SUCCESS;
}
