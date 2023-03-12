// SPDX-License-Identifier: GPL-2.0-only
/*
 * Jangan pelanggaran, ini source code open source GPLv2.
 *
 * gwdown - Simple multi-threaded download tool
 *
 * Author: Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org> # (copyright holder 100%)
 * License: GPLv2
 * Version: 0.1
 * Website: https://www.gnuweeb.org
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <stdbool.h>
#include <assert.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <libgen.h>
#include <curl/curl.h>

#define DEFAULT_NUM_THREADS 8
#define MIN_PARALLEL_DOWNLOAD_SIZE 65536ull
#define EXHAUST_SIZE_BEFORE_DONT_NEED (1024ull * 1024ull * 1024ull)

#ifdef __FreeBSD__
static off_t gwdown_lseek(int fd, off_t offset, int whence)
{
	return lseek(fd, offset, whence);
}
#else
static off64_t gwdown_lseek(int fd, off64_t offset, int whence)
{
	return lseek64(fd, offset, whence);
}
#endif

struct gwdown_thread {
	int			fd;
	CURL			*curl;
	struct gwdown_ctx	*ctx;
	pthread_t		thread;
	uint64_t		last_touch;
	uint64_t		offset;
	uint64_t		start;
	uint64_t		end;
	uint16_t		tid;
	volatile bool		finished;
	volatile bool		got_206;
};

struct gwdown_file_info {
	char		*filename;
	char		*content_type;
	char		*accept_ranges;
	char		*content_disposition;
	uint64_t	content_length;
};

struct gwdown_file_state {
	int	fd;
	size_t	map_size;
	char	*map;
	char	*output;
};

struct gwdown_ctx {
	volatile bool			stop;
	volatile bool			download_finished;
	bool				verbose;
	bool				resume;
	bool				support_map_download;
	bool				continue_parallel_download;
	bool				use_mmap;
	char				*url;
	char				*output;
	struct gwdown_thread		*threads;
	struct gwdown_file_info		file_info;
	struct gwdown_file_state 	file_state;
	pthread_cond_t			download_finished_cond;
	pthread_mutex_t			download_finished_mutex;
	uint64_t			per_thread_size;
	uint16_t			num_threads;
};

static struct gwdown_ctx *g_ctx;

static const struct option long_options[] = {
	{"help", no_argument, 0, 'h'},
	{"version", no_argument, 0, 'v'},
	{"output", required_argument, 0, 'o'},
	{"threads", required_argument, 0, 't'},
	{"resume", no_argument, 0, 'r'},
	{"verbose", no_argument, 0, 'V'},
	{"mmap", no_argument, 0, 'M'},
	{0, 0, 0, 0}
};

static void help(const char *prog)
{
	printf("Usage: %s [options] url\n", prog);
	printf("Options:\n");
	printf("  -h, --help\t\t\tShow this help message\n");
	printf("  -v, --version\t\t\tShow version\n");
	printf("  -o, --output\t\t\tOutput file\n");
	printf("  -t, --threads\t\t\tNumber of threads\n");
	printf("  -r, --resume\t\t\tResume download\n");
	printf("  -V, --verbose\t\t\tVerbose output\n");
	printf("  -M, --mmap\t\t\tUse mmap for writing file\n");
	printf("\n");
	printf("License: GPLv2\n");
	printf("Author: Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>  # (full copyright holder 100%%)\n");
	printf("\n");
	printf("Full open source: https://github.com/alviroiskandar/gwdown\n");
	printf("\n");
	printf("!!! Jangan pelanggaran, ini source code open source GPLv2. !!!\n\n");
}

static int parse_options(int argc, char *argv[], struct gwdown_ctx *ctx)
{
	int ret = 0;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "hvo:t:rVM", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			help(argv[0]);
			ret = 255;
			goto out;
		case 'v':
			printf("gwdown 0.1\n");
			ret = 255;
			goto out;
		case 'o':
			ctx->output = optarg;
			break;
		case 't':
			ctx->num_threads = (uint16_t)atoi(optarg);
			break;
		case 'r':
			ctx->resume = true;
			break;
		case 'V':
			ctx->verbose = true;
			break;
		case 'M':
			ctx->use_mmap = true;
			break;
		default:
			printf("Error: Unknown option '%s'\n\n", argv[optind]);
			help(argv[0]);
			ret = -EINVAL;
			goto out;
		}
	}

	if (ctx->num_threads > 512) {
		printf("Error: Too many threads, max allowed thread is 512\n\n");
		ret = -EINVAL;
		goto out;
	}

	if (optind < argc) {
		ctx->url = argv[optind];
	} else {
		printf("Error: Missing url argument\n\n");
		help(argv[0]);
		ret = -EINVAL;
	}

	if (ctx->resume) {
		printf("Error: The resume feature is currently not supproted\n");
		printf("       It's still just a draft feature\n\n");
		ret = -EOPNOTSUPP;
	}

out:
	return ret;
}

static int init_threads(struct gwdown_ctx *ctx)
{
	struct gwdown_thread *thread;
	uint16_t i;
	int ret;

	ret = pthread_mutex_init(&ctx->download_finished_mutex, NULL);
	if (ret) {
		fprintf(stderr, "Error: Failed to init mutex: %s\n",
			strerror(ret));
		return -ret;
	}

	ret = pthread_cond_init(&ctx->download_finished_cond, NULL);
	if (ret) {
		pthread_mutex_destroy(&ctx->download_finished_mutex);
		fprintf(stderr, "Error: Failed to init cond: %s\n",
			strerror(ret));
		return -ret;
	}

	if (!ctx->num_threads)
		ctx->num_threads = DEFAULT_NUM_THREADS;

	ctx->threads = calloc(ctx->num_threads, sizeof(struct gwdown_thread));
	if (!ctx->threads) {
		pthread_mutex_destroy(&ctx->download_finished_mutex);
		pthread_cond_destroy(&ctx->download_finished_cond);
		return -ENOMEM;
	}

	for (i = 0; i < ctx->num_threads; i++) {
		thread = &ctx->threads[i];
		thread->tid = i;
		thread->curl = curl_easy_init();
		if (!thread->curl) {
			/*
			 * No need to destroy the mutex and cond here
			 * because the caller will destroy it in
			 * destroy_gwdown_context().
			 */
			fprintf(stderr, "Error: Failed to init curl\n");
			return -ENOMEM;
		}
	}

	return 0;
}

static void destroy_gwdown_context(struct gwdown_ctx *ctx);

static int init_gwdown_context(struct gwdown_ctx *ctx)
{
	int ret = 0;

	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
		return -ENOMEM;

	ret = init_threads(ctx);
	if (ret)
		goto out;

	ctx->file_state.fd = -1;
	g_ctx = ctx;
	return 0;

out:
	destroy_gwdown_context(ctx);
	return ret;
}

static void destroy_threads(struct gwdown_ctx *ctx)
{
	struct gwdown_thread *thread;
	uint16_t i;

	if (!ctx->threads)
		return;

	for (i = 1; i < ctx->num_threads; i++) {
		thread = &ctx->threads[i];
		if (thread->ctx)
			pthread_join(thread->thread, NULL);
		if (thread->curl)
			curl_easy_cleanup(thread->curl);
	}

	pthread_mutex_lock(&ctx->download_finished_mutex);
	pthread_mutex_unlock(&ctx->download_finished_mutex);
	pthread_mutex_destroy(&ctx->download_finished_mutex);
	pthread_cond_destroy(&ctx->download_finished_cond);
	free(ctx->threads);
}

static void destroy_file_info(struct gwdown_ctx *ctx)
{
	struct gwdown_file_info *info = &ctx->file_info;

	free(info->filename);
	free(info->content_type);
	free(info->accept_ranges);
	free(info->content_disposition);
	memset(info, 0, sizeof(*info));
}

static void destroy_file_state(struct gwdown_ctx *ctx)
{
	struct gwdown_file_state *state = &ctx->file_state;

	if (state->fd != -1)
		close(state->fd);

	if (state->map) {
		msync(state->map, state->map_size, MS_ASYNC);
		munmap(state->map, state->map_size);
	}

	free(state->output);
	memset(state, 0, sizeof(*state));
}

static void destroy_gwdown_context(struct gwdown_ctx *ctx)
{
	ctx->stop = true;
	destroy_threads(ctx);
	destroy_file_info(ctx);
	destroy_file_state(ctx);
	curl_global_cleanup();
}

/*
 * Just like the strncmp() function but case insensitive.
 */
int strncmpi(const char *s1, const char *s2, size_t n)
{
	int c1, c2;

	while (n--) {
		c1 = tolower(*s1++);
		c2 = tolower(*s2++);
		if (c1 != c2 || !c1 || !c2)
			return c1 - c2;
	}

	return 0;
}

static bool fix_up_header(char **header_p, size_t rsize, bool *need_free)
{
	char *header = *header_p;
	char *orig = header;
	char c;

	c = header[rsize - 1];
	if (c == '\n') {
		header[rsize - 1] = '\0';
		if (header[rsize - 2] == '\r')
			header[rsize - 2] = '\0';

		*need_free = false;
		return true;
	}

	if (c != '\0') {
		header = malloc(rsize + 1);
		if (!header) {
			*need_free = false;
			return false;
		}

		memcpy(header, orig, rsize);
		header[rsize] = '\0';
		*need_free = true;
		return true;
	}

	*need_free = false;
	return true;
}

static char *str_trim(char *str)
{
	char *end;

	while (isspace(*str))
		str++;

	if (*str == 0)
		return str;

	end = str + strlen(str) - 1;
	while (end > str && isspace(*end))
		end--;

	*(end + 1) = 0;
	return str;
}

static void parse_file_name(const char *content_disposition,
			    struct gwdown_file_info *info)
{
	char *filename = NULL;
	char *tmp;
	char *ptr;

	tmp = strdup(content_disposition);
	if (!tmp)
		return;

	ptr = strstr(tmp, "filename=");
	if (!ptr)
		goto out;

	ptr += strlen("filename=");
	ptr = str_trim(ptr);
	if (*ptr == '"') {
		ptr++;
		filename = ptr;
		ptr = strchr(ptr, '"');
		if (!ptr)
			goto out;
		*ptr = '\0';
	} else {
		filename = ptr;
	}

	info->filename = strdup(filename);
out:
	free(tmp);
}

static int decide_output_file_name(struct gwdown_ctx *ctx)
{
	struct gwdown_file_state *state = &ctx->file_state;
	struct gwdown_file_info *info = &ctx->file_info;
	char *tmp;

	if (ctx->output) {
		state->output = strdup(ctx->output);
		goto out;
	}

	if (info->filename) {
		state->output = strdup(info->filename);
		goto out;
	}

	tmp = strdup(ctx->url);
	if (!tmp)
		goto out;

	state->output = strdup(basename(tmp));
	free(tmp);

	tmp = state->output;
	while (1) {
		char c = *tmp;

		if (c == '\0')
			break;
		/*
		 * Replace special characters with underscores.
		 */
		if (!isalnum(c) && c != '.' && c != '-' && c != '_')
			*tmp = '_';

		tmp++;
	}
out:
	if (!state->output)
		return -ENOMEM;

	return 0;
}

static int allocate_file(struct gwdown_ctx *ctx)
{
	struct gwdown_file_state *state = &ctx->file_state;
	struct gwdown_file_info *info = &ctx->file_info;
	int ret;
	int fd;

	ret = decide_output_file_name(ctx);
	if (ret) {
		fprintf(stderr, "decide_output_file_name() failed: %s\n",
			strerror(-ret));
		return ret;
	}

	printf("Target output file: %s\n", state->output);
	fd = open(state->output, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "Failed to open file %s: %s\n", info->filename,
			strerror(-ret));
		return ret;
	}

	if (!ctx->continue_parallel_download) {
		/*
		 * Don't do parallel download, the server doesn't support it.
		 *
		 * Another case:
		 * The file is too small for parallel download that it's not
		 * worth to do it.
		 */
		state->fd = fd;
		return 0;
	}

	ret = ftruncate(fd, info->content_length);
	if (ret) {
		ret = -errno;
		fprintf(stderr, "Failed to truncate file %s: %s\n",
			info->filename, strerror(-ret));
		goto out_err;
	}

	if (!ctx->use_mmap)
		goto out;

	state->map = mmap(NULL, info->content_length, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	if (state->map == MAP_FAILED) {
		ret = -errno;
		fprintf(stderr, "Failed to mmap file %s: %s\n", info->filename,
			strerror(-ret));
		goto out_err;
	}

	state->map_size = info->content_length;

out:
	state->fd = fd;
	return 0;

out_err:
	close(fd);
	return ret;
}

static size_t try_fetch_file_headers_curl_callback(void *ptr, size_t size,
						   size_t nmemb, void *data)
{
	struct gwdown_ctx *ctx = data;
	size_t rsize = size * nmemb;
	bool free_header = false;
	char *header = ptr;
	char *value;

	/*
	 * Skip the last CRLF. It's a delimiter between headers and
	 * the body.
	 */
	if (rsize == 2 && !memcmp(ptr, "\r\n", 2))
		return 2;

	if (rsize < 4)
		return 0;

	if (!fix_up_header(&header, rsize, &free_header))
		return 0;

	/*
	 * Parallel download is supported when at least these headers
	 * are present:
	 *
	 *   - Accept-Ranges: bytes
	 *   - Content-Length: <file size>
	 *
	 */

	if (!strncmpi(header, "accept-ranges: ", 15)) {
		value = strdup(str_trim(header + 15));
		if (!value)
			return 0;
		if (!strcmp(value, "bytes"))
			ctx->file_info.accept_ranges = value;
		else
			free(value);
	} else if (!strncmpi(header, "content-length: ", 16)) {
		value = str_trim(header + 16);
		ctx->file_info.content_length = strtoull(value, NULL, 10);
	} else if (!strncmpi(header, "content-disposition: ", 21)) {
		value = strdup(str_trim(header + 21));
		if (!value)
			return 0;
		ctx->file_info.content_disposition = value;
		parse_file_name(value, &ctx->file_info);
	}

	if (free_header)
		free(header);

	if (ctx->num_threads > 1 && ctx->file_info.accept_ranges &&
	    ctx->file_info.content_length > MIN_PARALLEL_DOWNLOAD_SIZE)
		ctx->continue_parallel_download = true;

	return rsize;
}

static void print_single_thread_download_info(struct gwdown_ctx *ctx)
{
	struct gwdown_file_info *info = &ctx->file_info;
	const char *reason;

	if (ctx->num_threads == 1) {
		reason = "the `num_threads` option is set to 1";
	} else if (!info->accept_ranges) {
		reason = "we didn't find \"Accept-Ranges: bytes\" header";
	} else if (!info->content_length) {
		reason = "we didn't find \"Content-Length\" header";
	} else if (info->content_length <= MIN_PARALLEL_DOWNLOAD_SIZE) {
		reason = "the file size is too small for parallel download";
	} else {
		reason = "of an unknown reason";
	}

	printf("Performing single threaded download because %s\n", reason);
}

static size_t try_fetch_file_body_curl_callback(void *ptr, size_t size,
						size_t nmemb, void *data)
{
	struct gwdown_ctx *ctx = data;
	struct gwdown_file_info *info = &ctx->file_info;
	struct gwdown_file_state *state = &ctx->file_state;
	size_t rsize = size * nmemb;
	ssize_t wr_ret;

	if (ctx->continue_parallel_download) {
		/*
		 * The file is eligible for parallel download, let the caller
		 * handle the rest of the download.
		 */
		return 0;
	}

	/*
	 * The file is not eligible for parallel download, just do a single
	 * threaded download.
	 *
	 * __ Langsung gas pokok'e, ora gagas parallel-paralelan. Pancene
	 *    servere ra support terus arep piye? __
	 */
	if (state->fd == -1) {
		int ret;

		ret = allocate_file(ctx);
		if (ret)
			return 0;

		print_single_thread_download_info(ctx);
		printf("Downloading file %s (%llu bytes)...\n", state->output,
		       (unsigned long long)info->content_length);
	}

	wr_ret = write(state->fd, ptr, rsize);
	if (wr_ret < 0) {
		fprintf(stderr, "Failed to write to file %s: %s\n",
			info->filename, strerror(errno));
		return 0;
	}
	return (size_t)wr_ret;
}

/*
 * The first try to fetch the file, it will try to get the file size
 * and the file name. If the file is eligible for parallel download,
 * it will set @ctx->continue_parallel_download to true.
 */
static int try_fetch_file(struct gwdown_ctx *ctx)
{
	CURLcode res;
	CURL *ch;

	printf("Trying to fetch the file %s...\n", ctx->url);
	ctx->continue_parallel_download = false;
	ch = ctx->threads[0].curl;
	curl_easy_setopt(ch, CURLOPT_URL, ctx->url);
	curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch, CURLOPT_HEADERDATA, ctx);
	curl_easy_setopt(ch, CURLOPT_HEADERFUNCTION, &try_fetch_file_headers_curl_callback);
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, ctx);
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, &try_fetch_file_body_curl_callback);
	curl_easy_setopt(ch, CURLOPT_USERAGENT, "gwdown/0.1");
	curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
	res = curl_easy_perform(ch);
	if (res != CURLE_OK && !ctx->continue_parallel_download) {
		fprintf(stderr, "try_fetch_file(): %s\n", curl_easy_strerror(res));
		return -EIO;
	}

	if (ctx->continue_parallel_download) {
		printf("Parallel download is possible, the file size is %llu bytes\n",
		       (unsigned long long)ctx->file_info.content_length);
		return allocate_file(ctx);
	}

	return 0;
}

static bool decide_whether_the_download_has_finished(struct gwdown_ctx *ctx)
{
	uint16_t i;

	pthread_mutex_lock(&ctx->download_finished_mutex);
	for (i = 0; i < ctx->num_threads; i++) {
		if (ctx->threads[i].finished)
			continue;

		/*
		 * There is still a thread that is not finished.
		 */
		pthread_mutex_unlock(&ctx->download_finished_mutex);
		return false;
	}

	/*
	 * Nice, all threads have finished the download.
	 */
	ctx->download_finished = true;
	pthread_cond_signal(&ctx->download_finished_cond);
	pthread_mutex_unlock(&ctx->download_finished_mutex);
	return true;
}

/*
 * The main thread must wait for other threads to finish the download
 * before it can continue. Otherwise, we will hit a use-after-free bug.
 */
static void wait_for_download_finish(struct gwdown_ctx *ctx)
{
	if (ctx->num_threads == 1)
		return;

	if (decide_whether_the_download_has_finished(ctx))
		return;

	pthread_mutex_lock(&ctx->download_finished_mutex);
	while (1) {
		if (ctx->download_finished)
			break;
		if (ctx->stop)
			break;
		pthread_cond_wait(&ctx->download_finished_cond,
				  &ctx->download_finished_mutex);
	}
	pthread_mutex_unlock(&ctx->download_finished_mutex);
}

static size_t paralell_download_headers_curl_callback(void *ptr, size_t size,
						      size_t nmemb, void *data)
{
	struct gwdown_thread *thread = data;
	struct gwdown_ctx *ctx = thread->ctx;
	size_t rsize = size * nmemb;
	bool free_header = false;
	char *header = ptr;

	/*
	 * Skip the last CRLF. It's a delimiter between headers and
	 * the body.
	 */
	if (rsize == 2 && !memcmp(ptr, "\r\n", 2)) {
		if (thread->got_206) {
			printf("Thread %u is downloading...\n", thread->tid);
			return 2;
		}

		printf("Thread %hu did not get HTTP code 206, aborting...\n",
		       thread->tid);
		ctx->stop = true;
		return 0;
	}

	if (rsize < 4)
		return 0;

	if (!fix_up_header(&header, rsize, &free_header))
		return 0;

	if (strstr(header, "HTTP/") && strstr(header, " 206"))
		thread->got_206 = true;

	if (free_header)
		free(header);

	return rsize;
}

#ifdef MADV_DONTNEED
static void give_memory_advice(struct gwdown_thread *thread,
			       struct gwdown_file_state *state)
{
	uint64_t last_touch_size;

	last_touch_size = thread->offset - thread->last_touch;
	if (last_touch_size >= EXHAUST_SIZE_BEFORE_DONT_NEED) {
		madvise(&state->map[thread->last_touch], last_touch_size,
			MADV_DONTNEED);
		thread->last_touch = thread->offset;
	}
}
#else
static void give_memory_advice(struct gwdown_ctx *ctx)
{
	(void)ctx;
}
#endif

static size_t paralell_download_body_curl_callback(void *ptr, size_t size,
						   size_t nmemb, void *data)
{
	struct gwdown_thread *thread = data;
	struct gwdown_ctx *ctx = thread->ctx;
	struct gwdown_file_state *state = &ctx->file_state;
	size_t rsize = size * nmemb;
	ssize_t wr_ret;

	if (ctx->stop)
		return 0;

	if (ctx->use_mmap) {
		memcpy(&state->map[thread->offset], ptr, rsize);
		thread->offset += rsize;
		give_memory_advice(thread, state);
		wr_ret = (ssize_t)rsize;
	} else {
		wr_ret = write(thread->fd, ptr, rsize);
		if (wr_ret < 0) {
			fprintf(stderr, "write() failed: %s\n", strerror(errno));
			ctx->stop = true;
			return 0;
		}
		thread->offset += wr_ret;
	}

	return (size_t)wr_ret;
}

static void *run_gwdown_parallel_download_worker(void *data)
{
	struct gwdown_thread *thread = data;
	struct gwdown_ctx *ctx = thread->ctx;
	struct gwdown_file_state *state = &ctx->file_state;
	CURL *ch = thread->curl;
	char range[128];
	CURLcode res;

	snprintf(range, sizeof(range), "%llu-%llu",
		 (unsigned long long)thread->start,
		 (unsigned long long)thread->end);

	if (thread->tid > 0 && !ctx->use_mmap) {
		off_t tmp;
		int fd;

		assert(state->map == NULL);
		fd = open(ctx->file_state.output, O_WRONLY);
		if (fd < 0) {
			fprintf(stderr, "open() failed: %s\n", strerror(errno));
			ctx->stop = true;
			goto out;
		}

		tmp = (off_t)gwdown_lseek(fd, thread->start, SEEK_SET);
		if (tmp < 0) {
			fprintf(stderr, "lseek64() failed: %s\n", strerror(errno));
			ctx->stop = true;
			goto out;
		}

		thread->fd = fd;
	} else if (ctx->use_mmap) {
		assert(state->map != NULL);
	}

	curl_easy_setopt(ch, CURLOPT_URL, ctx->url);
	curl_easy_setopt(ch, CURLOPT_RANGE, range);
	curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch, CURLOPT_USERAGENT, "gwdown/0.1");
	curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(ch, CURLOPT_HEADERFUNCTION, &paralell_download_headers_curl_callback);
	curl_easy_setopt(ch, CURLOPT_HEADERDATA, thread);
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, &paralell_download_body_curl_callback);
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, thread);
	res = curl_easy_perform(ch);
	if (res != CURLE_OK) {
		fprintf(stderr, "run_gwdown_parallel_download_worker() thread %u: %s\n",
			thread->tid, curl_easy_strerror(res));
	} else {
		printf("Thread %u has finished the download!\n", thread->tid);
	}

	thread->finished = true;
	if (thread->tid == 0) {
		wait_for_download_finish(ctx);
	} else {
		decide_whether_the_download_has_finished(ctx);
		if (!ctx->use_mmap)
			close(thread->fd);
	}

out:
	return data;
}

static int run_parallel_download(struct gwdown_ctx *ctx)
{
	struct gwdown_thread *thread;
	int ret;
	int i;

	ctx->per_thread_size = ctx->file_info.content_length / ctx->num_threads;

	for (i = 0; i < ctx->num_threads; i++) {
		thread = &ctx->threads[i];
		thread->ctx = ctx;

		thread->start = ctx->per_thread_size * i;
		if (i == ctx->num_threads - 1)
			thread->end = ctx->file_info.content_length - 1;
		else
			thread->end = thread->start + ctx->per_thread_size - 1;

		thread->offset = thread->start;
		thread->last_touch = thread->start;

		printf("Thread %hu range: (%llu-%llu)\n", thread->tid,
		       (unsigned long long)thread->start,
		       (unsigned long long)thread->end);

		/*
		 * The main thread does not need to create a thread.
		 */
		if (i == 0) {
			thread->fd = ctx->file_state.fd;
			continue;
		}

		ret = pthread_create(&thread->thread, NULL,
				     &run_gwdown_parallel_download_worker,
				     thread);
		if (ret) {
			fprintf(stderr, "Failed to create thread: %s\n",
				strerror(ret));
			return -ret;
		}
	}

	ctx->threads[0].ctx = ctx;
	run_gwdown_parallel_download_worker(&ctx->threads[0]);
	return 0;
}

static int run_gwdown(struct gwdown_ctx *ctx)
{
	int ret;

	ret = try_fetch_file(ctx);
	if (ret)
		return ret;

	if (!ctx->continue_parallel_download)
		return 0;

	return run_parallel_download(ctx);
}

int main(int argc, char *argv[])
{
	struct gwdown_ctx ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.num_threads = DEFAULT_NUM_THREADS;

	ret = parse_options(argc, argv, &ctx);
	if (ret)
		goto out;

	ret = init_gwdown_context(&ctx);
	if (ret)
		goto out;

	ret = run_gwdown(&ctx);
out:
	if (ret < 0)
		printf("Error: %s\n", strerror(-ret));

	destroy_gwdown_context(&ctx);
	return abs(ret);
}
