#define FUSE_USE_VERSION 30

#include <fuse_lowlevel.h>

#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>


#define AULSMFS_ROOT_INODE   FUSE_ROOT_ID
#define AULSMFS_ROOT_MODE    (S_IFDIR | 0755)


struct aulsmfs_config {
	const char *image_path;
	int fd;
};

static const struct fuse_opt aulsmfs_opts[] = {
	{"--image=%s", offsetof(struct aulsmfs_config, image_path), 0},
	FUSE_OPT_END
};


static void __aulsmfs_stat(fuse_ino_t ino, struct stat *stat)
{
	assert(ino == AULSMFS_ROOT_INODE);

	memset(stat, 0, sizeof(*stat));
	stat->st_ino = ino;
	stat->st_mode = AULSMFS_ROOT_MODE;
	stat->st_nlink = 1;
	stat->st_size = 2;
}

static void aulsmfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	assert(parent == AULSMFS_ROOT_INODE);
	(void) name;
	fuse_reply_err(req, ENOENT);
}

static void aulsmfs_getattr(fuse_req_t req, fuse_ino_t ino,
			struct fuse_file_info *fi)
{
	struct stat stat;

	(void) fi;
	__aulsmfs_stat(ino, &stat);
	fuse_reply_attr(req, &stat, 0.0);
}

struct aulsmfs_dirbuf {
	char *buf;
	size_t size, capacity;
};

static void aulsmfs_dirbuf_add(fuse_req_t req, struct aulsmfs_dirbuf *buf,
			const char *name, const struct stat *stat, off_t off)
{
	const size_t oldsize = buf->size;
	const size_t esize = fuse_add_direntry(req, NULL, 0, name, NULL, 0);

	if (oldsize + esize > buf->capacity) {
		buf->capacity = buf->capacity * 2 >= oldsize + esize
					? buf->capacity * 2 : oldsize + esize;
		assert((buf->buf = realloc(buf->buf, buf->capacity)));
	}

	buf->size += esize;
	fuse_add_direntry(req, buf->buf + oldsize, esize, name, stat, off);
}

static void aulsmfs_dirbuf_setup(struct aulsmfs_dirbuf *buf)
{
	buf->buf = NULL;
	buf->size = buf->capacity = 0;
}

static void aulsmfs_dirbuf_release(struct aulsmfs_dirbuf *buf)
{
	free(buf->buf);
	aulsmfs_dirbuf_setup(buf);
}

static void aulsmfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			off_t off, struct fuse_file_info *fi)
{
	struct aulsmfs_dirbuf dirbuf;
	struct stat stat;

	assert(ino == AULSMFS_ROOT_INODE);
	(void) fi;

	if (ino != AULSMFS_ROOT_INODE) {
		fuse_reply_err(req, ENOTDIR);
		return;
	}

	aulsmfs_dirbuf_setup(&dirbuf);
	switch (off) {
	case 0:
		__aulsmfs_stat(AULSMFS_ROOT_INODE, &stat);
		aulsmfs_dirbuf_add(req, &dirbuf, ".", &stat, 1);
	case 1:
		__aulsmfs_stat(AULSMFS_ROOT_INODE, &stat);
		aulsmfs_dirbuf_add(req, &dirbuf, "..", &stat, 2);
	}
	fuse_reply_buf(req, dirbuf.buf, dirbuf.size < size
				? dirbuf.size : size);
	aulsmfs_dirbuf_release(&dirbuf);
}

static void aulsmfs_open(fuse_req_t req, fuse_ino_t ino,
			struct fuse_file_info *fi)
{
	assert(ino == AULSMFS_ROOT_INODE);
	(void) fi;
	fuse_reply_err(req, EISDIR);
}

static const struct fuse_lowlevel_ops aulsmfs_ops = {
	.lookup = &aulsmfs_lookup,
	.getattr = &aulsmfs_getattr,
	.readdir = &aulsmfs_readdir,
	.open = &aulsmfs_open,
};


#define AULSMFS_MAJOR 0
#define AULSMFS_MINOR 1

static void aulsmfs_help(void)
{
	printf("    --image=path           path to the block device image\n\n");
}

static void usage(const char *name)
{
	printf("usage: %s [options] <mountpoint>\n\n", name);
	aulsmfs_help();
	fuse_cmdline_help();
	fuse_lowlevel_help();
}

static void version(void)
{
	printf("AULSMFS version %d.%d\n", AULSMFS_MAJOR, AULSMFS_MINOR);
	printf("FUSE library version %s\n", fuse_pkgversion());
	fuse_lowlevel_version();
}

int main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_cmdline_opts opts;
	struct aulsmfs_config config;
	struct fuse_session *se;
	int ret = 1;

	memset(&opts, 0, sizeof(opts));
	memset(&config, 0, sizeof(config));
	config.fd = -1;

	if (fuse_opt_parse(&args, &config, aulsmfs_opts, NULL)) {
		puts("Failed to parse cmdline");
		goto out;
	}

	if (fuse_parse_cmdline(&args, &opts)) {
		puts("Failed to parse cmdline");
		goto out;
	}

	if (opts.show_help) {
		usage(argv[0]);
		goto out;
	}

	if (opts.show_version) {
		version();
		goto out;
	}

	if (!config.image_path) {
		puts("You didn't specified device image path (--image option)");
		usage(argv[0]);
		goto out;
	};

	/* FUSE employs kernel page cache, so no point caching it again,
	 * this is why we use O_DIRECT here. But O_DIRECT doesn't provide
	 * guarantees of SYNC or DSYNC. For filesystem metadata we will
	 * use our own caching if neccessary. */
	config.fd = open(config.image_path, O_RDWR | O_DIRECT);
	if (-1 == config.fd) {
		printf("Failed to open backing device image %s\n",
					config.image_path);
		goto out;
	}

	se = fuse_session_new(&args, &aulsmfs_ops, sizeof(aulsmfs_ops),
				&config);
	if (!se) {
		puts("Failed to create fuse session");
		goto out;
	}

	if (fuse_set_signal_handlers(se)) {
		puts("Failed to set fuse signal handlers");
		goto destroy_session;
	}

	if (fuse_session_mount(se, opts.mountpoint)) {
		puts("Failed to mount");
		goto remove_handlers;
	}

	fuse_daemonize(opts.foreground);

	if (opts.singlethread)
		ret = fuse_session_loop(se);
	else
		ret = fuse_session_loop_mt(se, opts.clone_fd);

	if (ret)
		puts("Failed to run fuse event loop");

remove_handlers:
	fuse_remove_signal_handlers(se);

destroy_session:
	fuse_session_destroy(se);

out:
	if (-1 != config.fd)
		close(config.fd);

	free((void *)config.image_path);
	free(opts.mountpoint);
	fuse_opt_free_args(&args);

	return ret ? 1 : 0;
}
