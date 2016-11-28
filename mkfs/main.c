#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <endian.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include <aulsmfs.h>
#include <crc64.h>
#include <file_wrappers.h>

struct aulsmfs_config {
	uint64_t bytes;
	uint64_t pages;
	uint64_t page_size;
	const char *path;
	int fd;
};

static int aulsmfs_mkfs(const struct aulsmfs_config *config)
{
	struct aulsmfs_super super;

	memset(&super, 0, sizeof(super));
	super.magic = htole64(AULSMFS_MAGIC);
	super.version = htole64(AULSMFS_VERSION);
	super.page_size = htole64(config->page_size);
	super.pages = htole64(config->pages);
	super.csum = htole64(crc64(&super, sizeof(super)));

	return file_write_at(config->fd, &super, sizeof(super), 0);
}

static void aulsmfs_config_default(struct aulsmfs_config *config)
{
	config->bytes = config->pages = 0;
	config->page_size = 4096;
	config->path = NULL;
	config->fd = -1;
}

static int aulsmfs_config_check(struct aulsmfs_config *config)
{
	ssize_t bytes;

	assert(config->path);
	assert(config->page_size >= 512);
	assert((config->page_size & (config->page_size - 1)) == 0);

	config->fd = open(config->path, O_WRONLY);
	if (config->fd < 0) {
		printf("Failed to open file %s\n", config->path);
		return -1;
	}

	if ((bytes = file_size(config->fd)) < 0) {
		printf("Failed to get file %s size\n", config->path);
		return -1;
	}

	if (!config->bytes && !config->pages)
		config->bytes = bytes;

	config->bytes &= ~(config->page_size - 1);
	if (!config->bytes)
		config->bytes = config->pages * config->page_size;
	if (!config->pages)
		config->pages = config->bytes / config->page_size;

	if (config->pages * config->page_size != config->bytes) {
		printf("Specified size in bytes and in pages don't "
			"agree with each other\n");
		return -1;
	}

	if (config->bytes > (uint64_t)bytes) {
		if (ftruncate(config->fd, config->bytes) < 0) {
			printf("Truncate for file %s failed\n", config->path);
			return -1;
		}
	}

	return 0;
}

static void aulsmfs_config_release(struct aulsmfs_config *config)
{
	if (config->fd >= 0)
		close(config->fd);
}

static const char *aulsmfs_short_opts = "s:p:P:h";
static const struct option aulsmfs_long_opts[] = {
	{"size", required_argument, NULL, 's'},
	{"pages", required_argument, NULL, 'p'},
	{"page_size", required_argument, NULL, 'P'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0}
};

static void usage(const char *name)
{
	printf("Usage: %s [options] path\n\n", name);
	printf("Possible options:\n");
	printf("\t--size      -s <bytes>    number of bytes for filesystem\n");
	printf("\t--pages     -p <pages>    number of pages for filesystem\n");
	printf("\t--page_size -P <bytes>    page size in bytes\n");
	printf("\t--help      -h            show this message\n");
}

int main(int argc, char **argv)
{
	struct aulsmfs_config config;
	char *endptr = NULL;
	int opt, ret;

	aulsmfs_config_default(&config);
	while ((opt = getopt_long(argc, argv, aulsmfs_short_opts,
				aulsmfs_long_opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			config.bytes = strtoull(optarg, &endptr, 0);
			if (*endptr) {
				printf("Wrong size value: %s\n", optarg);
				exit(1);
			}
			break;
		case 'p':
			config.pages = strtoull(optarg, &endptr, 0);
			if (*endptr) {
				printf("Wrong number of pages: %s\n", optarg);
				exit(1);
			}
			break;
		case 'P':
			config.page_size = strtoull(optarg, &endptr, 0);
			if (*endptr) {
				printf("Wrong page_size value: %s\n", optarg);
				exit(1);
			}
			if (config.page_size < 512 || (config.page_size &
						(config.page_size - 1))) {
				printf("Page size must be power of 2 greater "
					"or equal to 512\n");
				exit(1);
			}
			break;
		case 'h':
			usage(argv[0]);
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (optind != argc - 1) {
		usage(argv[0]);
		exit(1);
	}

	config.path = argv[optind];
	if (!(ret = aulsmfs_config_check(&config)))
		ret = aulsmfs_mkfs(&config);
	aulsmfs_config_release(&config);

	return ret ? 1 : 0;
}
