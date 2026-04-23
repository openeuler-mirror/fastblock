#define _GNU_SOURCE
#include <ctype.h>
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

#define MAX_LINE_LEN 1024
#define MAX_CMD_LEN 4096

struct config {
	char *monitor_addr;
	char *pool_name;
	char *image_name;
	char *token;
	char *snapshot_name;
	char *conf_file;
	char *read_only;
	char *debug_size_bytes;
	char *debug_object_size;
	char *debug_pool_id;
	char *debug_pg_count;
};

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
		} else if (strcmp(key, "token") == 0) {
			maybe_set(&cfg->token, value);
		} else if (strcmp(key, "snapshot_name") == 0) {
			maybe_set(&cfg->snapshot_name, value);
		} else if (strcmp(key, "read_only") == 0) {
			maybe_set(&cfg->read_only, value);
		} else if (strcmp(key, "debug_size_bytes") == 0) {
			maybe_set(&cfg->debug_size_bytes, value);
		} else if (strcmp(key, "debug_object_size") == 0) {
			maybe_set(&cfg->debug_object_size, value);
		} else if (strcmp(key, "debug_pool_id") == 0) {
			maybe_set(&cfg->debug_pool_id, value);
		} else if (strcmp(key, "debug_pg_count") == 0) {
			maybe_set(&cfg->debug_pg_count, value);
		}
	}

	fclose(fp);
}

static void print_usage(const char *prog_name)
{
	fprintf(stderr, "Usage: %s <attach|detach> [options]\n", prog_name);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -c, --conf <file>\n");
	fprintf(stderr, "  --monitor-addr <addr[,addr...]>\n");
	fprintf(stderr, "  --pool-name <name>\n");
	fprintf(stderr, "  --image-name <name>\n");
	fprintf(stderr, "  --token <token>\n");
	fprintf(stderr, "  --snapshot-name <name>\n");
	fprintf(stderr, "  --read-only <true|false>\n");
	fprintf(stderr, "  --debug-size-bytes <bytes>\n");
	fprintf(stderr, "  --debug-object-size <bytes>\n");
	fprintf(stderr, "  --debug-pool-id <id>\n");
	fprintf(stderr, "  --debug-pg-count <count>\n");
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

int main(int argc, char *argv[])
{
	const char *operation;
	const char *sysfs_path;
	struct config cfg = {0};
	char command_str[MAX_CMD_LEN] = {0};
	int fd;
	int opt;
	int option_index = 0;

	static struct option long_options[] = {
		{"conf", required_argument, 0, 'c'},
		{"monitor-addr", required_argument, 0, 0},
		{"pool-name", required_argument, 0, 0},
		{"image-name", required_argument, 0, 0},
		{"token", required_argument, 0, 0},
		{"snapshot-name", required_argument, 0, 0},
		{"read-only", required_argument, 0, 0},
		{"debug-size-bytes", required_argument, 0, 0},
		{"debug-object-size", required_argument, 0, 0},
		{"debug-pool-id", required_argument, 0, 0},
		{"debug-pg-count", required_argument, 0, 0},
		{0, 0, 0, 0},
	};

	if (argc < 2 ||
	    (strcmp(argv[1], "attach") != 0 && strcmp(argv[1], "detach") != 0)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	operation = argv[1];
	++argv;
	--argc;

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
					  "token") == 0) {
				maybe_set(&cfg.token, optarg);
			} else if (strcmp(long_options[option_index].name,
					  "snapshot-name") == 0) {
				maybe_set(&cfg.snapshot_name, optarg);
			} else if (strcmp(long_options[option_index].name,
					  "read-only") == 0) {
				maybe_set(&cfg.read_only, optarg);
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
			}
			break;
		case 'c':
			cfg.conf_file = strdup(optarg);
			break;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (cfg.conf_file)
		parse_config_file(cfg.conf_file, &cfg);

	if (!cfg.monitor_addr || !cfg.pool_name || !cfg.image_name) {
		fprintf(stderr, "monitor-addr, pool-name and image-name are required\n");
		return EXIT_FAILURE;
	}

	append_kv(command_str, sizeof(command_str), "monitor_addr",
		  cfg.monitor_addr);
	append_kv(command_str, sizeof(command_str), "pool_name",
		  cfg.pool_name);
	append_kv(command_str, sizeof(command_str), "image_name",
		  cfg.image_name);
	append_kv(command_str, sizeof(command_str), "token", cfg.token);
	append_kv(command_str, sizeof(command_str), "snapshot_name",
		  cfg.snapshot_name);
	append_kv(command_str, sizeof(command_str), "read_only",
		  cfg.read_only);
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

	fd = open(sysfs_path, O_WRONLY);
	if (fd < 0) {
		perror("Failed to open sysfs file");
		return EXIT_FAILURE;
	}

	if (write(fd, command_str, strlen(command_str)) < 0) {
		char error_buf[256] = {0};
		int error_fd;

		perror("Failed to write to sysfs file");
		error_fd = open(SYSFS_PATH_LAST_ERROR, O_RDONLY);
		if (error_fd >= 0) {
			ssize_t bytes_read;

			bytes_read = read(error_fd, error_buf,
					  sizeof(error_buf) - 1);
			if (bytes_read > 0)
				fprintf(stderr, "Driver error: %s\n", error_buf);
			close(error_fd);
		}
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	free(cfg.monitor_addr);
	free(cfg.pool_name);
	free(cfg.image_name);
	free(cfg.token);
	free(cfg.snapshot_name);
	free(cfg.conf_file);
	free(cfg.read_only);
	free(cfg.debug_size_bytes);
	free(cfg.debug_object_size);
	free(cfg.debug_pool_id);
	free(cfg.debug_pg_count);
	return EXIT_SUCCESS;
}
