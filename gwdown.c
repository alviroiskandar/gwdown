// SPDX-License-Identifier: GPL-2.0-only
/*
 * Jangan pelanggaran, ini source code GPLv2.
 *
 * gwdown - Simple multi-threaded download tool
 *
 * Author: Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 * License: GPLv2
 * Version: 0.1
 * Website: https://www.gnuweeb.org
 *
 */
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
#include <ctype.h>
#include <inttypes.h>
#include <curl/curl.h>

#define DEFAULT_NR_THREADS 4

struct gwdown_file_info {
	char		*content_disposition;
	char		*content_type;
	char		*filename;
	bool		accept_ranges;
	uint64_t	size;
};

struct gwdown_thread {
	CURL			*curl;
	struct gwdown_ctx	*ctx;
	pthread_t		thread;
	uint64_t		start;
	uint64_t		end;
	uint64_t		size;
	uint64_t		pos;
};

struct gwdown_ctx {
	volatile bool		stop;
	uint16_t		nr_thread;
	const char		*url;
	const char		*output;
	bool			verbose;
	bool			resume;
	struct gwdown_thread	*threads;
	struct gwdown_file_info	file_info;
};

static struct gwdown_ctx *g_ctx;

static const struct option long_options[] = {
	{"help", no_argument, 0, 'h'},
	{"version", no_argument, 0, 'v'},
	{"output", required_argument, 0, 'o'},
	{"threads", required_argument, 0, 't'},
	{"resume", no_argument, 0, 'r'},
	{"verbose", no_argument, 0, 'V'},
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
	printf("\n");
	printf("License: GPLv2\n");
	printf("Author: Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>\n");
	printf("\n");
	printf("Full open source: https://github.com/alviroiskandar/gwdown\n");
	printf("\n");
	printf("!!! Jangan pelanggaran, ini source code GPLv2 !!!\n\n");
}

static int parse_options(int argc, char *argv[], struct gwdown_ctx *ctx)
{
	int ret = 0;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "hvo:t:rV", long_options, NULL);
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
			ctx->nr_thread = (uint16_t)atoi(optarg);
			break;
		case 'r':
			ctx->resume = true;
			break;
		case 'V':
			ctx->verbose = true;
			break;
		default:
			printf("Error: Unknown option '%s'\n\n", argv[optind]);
			help(argv[0]);
			ret = -EINVAL;
			goto out;
		}
	}

	if (ctx->nr_thread > 512) {
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

out:
	return ret;
}

static void destroy_gwdown_threads(struct gwdown_ctx *ctx)
{
	struct gwdown_thread *thread;
	int i;

	for (i = 0; i < ctx->nr_thread; i++) {
		thread = &ctx->threads[i];

		if (ctx->threads[i].curl)
			curl_easy_cleanup(ctx->threads[i].curl);

		if (!thread->ctx)
			continue;

		assert(ctx == thread->ctx);
		pthread_join(thread->thread, NULL);
	}
	free(ctx->threads);
	ctx->threads = NULL;
}

static void destroy_file_info(struct gwdown_ctx *ctx)
{
	free(ctx->file_info.content_disposition);
	free(ctx->file_info.content_type);
	free(ctx->file_info.filename);
}

static void destroy_gwdown_context(struct gwdown_ctx *ctx)
{
	if (!ctx)
		return;

	ctx->stop = true;
	destroy_gwdown_threads(ctx);
	destroy_file_info(ctx);
	memset(ctx, 0, sizeof(*ctx));
	g_ctx = NULL;
}

static int init_gwdown_threads(struct gwdown_ctx *ctx)
{
	struct gwdown_thread *thread;
	int i;

	if (!ctx->nr_thread)
		ctx->nr_thread = DEFAULT_NR_THREADS;

	ctx->threads = calloc(ctx->nr_thread, sizeof(*ctx->threads));
	if (!ctx->threads) {
		printf("Error: Failed to allocate memory for threads\n");
		return -ENOMEM;
	}

	for (i = 0; i < ctx->nr_thread; i++) {
		thread = &ctx->threads[i];
		thread->curl = curl_easy_init();
		if (!thread->curl)
			return -ENOMEM;

		thread->ctx = ctx;
	}

	return 0;
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

	if (!content_disposition)
		return;

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

static size_t curl_callback_get_file_info(void *ptr, size_t size, size_t nmemb,
					  void *data)
{
	struct gwdown_ctx *ctx = data;
	struct gwdown_file_info *info = &ctx->file_info;
	bool free_hdr = false;
	char *header = ptr;
	char *value;
	int len;

	if (ctx->stop)
		return 0;

	if (size != 1)
		return 0;

	if (nmemb == 2 && header[0] == '\r' && header[1] == '\n')
		return nmemb;

	if (nmemb < 2)
		return 0;

	if (header[nmemb - 1] != '\0') {
		char *tmp;

		tmp = malloc(nmemb + 1);
		if (!tmp)
			return 0;

		memcpy(tmp, header, nmemb);
		tmp[nmemb] = '\0';
		header = tmp;
		free_hdr = true;
	}

	if (header[nmemb - 1] == '\n')
		header[nmemb - 1] = '\0';

	if (header[nmemb - 2] == '\r')
		header[nmemb - 2] = '\0';

	if (strncmpi(header, "content-length:", 15) == 0) {
		value = header + 15;
		len = nmemb - 15;
		info->size = strtoull(value, NULL, 10);
	} else if (strncmpi(header, "content-type:", 13) == 0) {
		value = header + 13;
		len = nmemb - 13;
		info->content_type = strndup(value, len);
	} else if (strncmpi(header, "content-disposition:", 20) == 0) {
		value = header + 20;
		len = nmemb - 20;
		info->content_disposition = strndup(value, len);
		if (info->content_disposition)
			parse_file_name(info->content_disposition, info);
	} else if (strncmpi(header, "accept-ranges:", 14) == 0) {
		value = header + 14;
		len = nmemb - 14;
		if (strncmpi(value, "bytes", 5) == 0)
			info->accept_ranges = true;
	}

	if (free_hdr)
		free(header);

	return nmemb;
}

static void print_file_info(struct gwdown_ctx *ctx)
{
	struct gwdown_file_info *info = &ctx->file_info;

	printf("File info:\n");
	if (info->filename)
		printf("  Filename: %s\n", info->filename);
	if (info->size)
		printf("  Size: %" PRIu64 " bytes\n", info->size);
	if (info->content_type)
		printf("  Content-Type: %s\n", info->content_type);
	if (info->content_disposition)
		printf("  Content-Disposition: %s\n", info->content_disposition);
}

static int get_file_info_from_server(struct gwdown_ctx *ctx)
{
	CURL *ch = ctx->threads[0].curl;
	CURLcode res;

	printf("Getting file info from server...\n");
	curl_easy_setopt(ch, CURLOPT_URL, ctx->url);
	curl_easy_setopt(ch, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(ch, CURLOPT_HEADER, 1L);
	curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, curl_callback_get_file_info);
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, ctx);
	curl_easy_setopt(ch, CURLOPT_USERAGENT, "gwdown/0.1");
	curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
	res = curl_easy_perform(ch);
	if (res != CURLE_OK) {
		printf("Error: Failed to get file info from server: %s\n",
		       curl_easy_strerror(res));
		return -EIO;
	}

	print_file_info(ctx);
	return 0;
}

static int init_gwdown_context(struct gwdown_ctx *ctx)
{
	int ret;

	ctx->stop = false;

	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
		printf("Error: Failed to initialize curl\n");
		return -ENOMEM;
	}

	ret = init_gwdown_threads(ctx);
	if (ret)
		goto out_err;

	ret = get_file_info_from_server(ctx);
	if (ret)
		goto out_err;

	g_ctx = ctx;
	return 0;

out_err:
	destroy_gwdown_context(ctx);
	return ret;
}

int main(int argc, char *argv[])
{
	struct gwdown_ctx ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ret = parse_options(argc, argv, &ctx);
	if (ret)
		return -ret;

	ret = init_gwdown_context(&ctx);
	if (ret)
		goto out;

out:
	destroy_gwdown_context(&ctx);
	return -ret;
}
