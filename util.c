/*
 * cros-yavta --  ChromiumOS Yet Another V4L2 Test Application
 *
 * Copyright (C) 2005-2015 cros-yavta authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <alloca.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "videodev2.h"

#include "yavta.h"
#include "remote.h"

int verbose;
char *log_filename = "yavta.log";
int log_verbose = -1;
__thread char thread_tag[64];

/* ------------------------------------------------------------------
 * Helper functions to manuplate timespec
 */
void timespec_add(const struct timespec *a, const struct timespec *b,
		  struct timespec *c)
{
	c->tv_sec = a->tv_sec + b->tv_sec;
	c->tv_nsec = a->tv_nsec + b->tv_nsec;
	if (c->tv_nsec >= 1000000000) {
		c->tv_sec++;
		c->tv_nsec -= 1000000000;
	}
}

void timespec_sub(const struct timespec *a, const struct timespec *b,
		  struct timespec *c)
{
	c->tv_sec = a->tv_sec - b->tv_sec;
	c->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (c->tv_nsec < 0) {
		c->tv_sec--;
		c->tv_nsec += 1000000000;
	}
}

bool timespec_less(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec < b->tv_sec;
	return a->tv_nsec < b->tv_nsec;
}

double timespec_to_double(const struct timespec *a)
{
	return a->tv_sec + a->tv_nsec / 1e9;
}

void timeval_to_timespec(const struct timeval *a, struct timespec *b)
{
	b->tv_sec = a->tv_sec;
	b->tv_nsec = a->tv_usec * 1000;
}

double timeval_to_double(const struct timeval *a)
{
	return a->tv_sec + a->tv_usec / 1e6;
}

void timecode_to_timespec(const struct v4l2_timecode *a, struct timespec *b)
{
	double fps;
	switch (a->type) {
	case V4L2_TC_TYPE_24FPS:
		fps = 24.0;
		break;
	case V4L2_TC_TYPE_25FPS:
		fps = 25.0;
		break;
	case V4L2_TC_TYPE_30FPS:
		fps = 30.0;
		break;
	case V4L2_TC_TYPE_50FPS:
		fps = 50.0;
		break;
	case V4L2_TC_TYPE_60FPS:
		fps = 60.0;
		break;
	default:
		printf("unknown timecode type: %d\n", a->type);
		fps = 30.0;
	}

	b->tv_sec = a->hours * 3600 + a->minutes * 60 + a->seconds;
	b->tv_nsec = (long)(1000000000.0 * a->frames / fps);
}

/* ------------------------------------------------------------------
 * Helper functions for v4l
 */
const char *v4l2_buf_type_name(enum v4l2_buf_type type)
{
	static struct {
		enum v4l2_buf_type type;
		const char *name;
	} names[] = {
		{ V4L2_BUF_TYPE_VIDEO_CAPTURE, "Video capture" },
		{ V4L2_BUF_TYPE_VIDEO_OUTPUT, "Video output" },
		{ V4L2_BUF_TYPE_VIDEO_OVERLAY, "Video overlay" },
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); ++i) {
		if (names[i].type == type)
			return names[i].name;
	}

	if (type & V4L2_BUF_TYPE_PRIVATE)
		return "Private";
	else
		return "Unknown";
}

static struct {
	const char *name;
	unsigned int fourcc;
} pixel_formats[] = {
	{ "RGB332", V4L2_PIX_FMT_RGB332 },
	{ "RGB555", V4L2_PIX_FMT_RGB555 },
	{ "RGB565", V4L2_PIX_FMT_RGB565 },
	{ "RGB555X", V4L2_PIX_FMT_RGB555X },
	{ "RGB565X", V4L2_PIX_FMT_RGB565X },
	{ "BGR24", V4L2_PIX_FMT_BGR24 },
	{ "RGB24", V4L2_PIX_FMT_RGB24 },
	{ "BGR32", V4L2_PIX_FMT_BGR32 },
	{ "RGB32", V4L2_PIX_FMT_RGB32 },
	{ "Y8", V4L2_PIX_FMT_GREY },
	{ "Y10", V4L2_PIX_FMT_Y10 },
	{ "Y12", V4L2_PIX_FMT_Y12 },
	{ "Y16", V4L2_PIX_FMT_Y16 },
	{ "YUYV", V4L2_PIX_FMT_YUYV },
	{ "UYVY", V4L2_PIX_FMT_UYVY },
	{ "NV12", V4L2_PIX_FMT_NV12 },
	{ "NV21", V4L2_PIX_FMT_NV21 },
	{ "NV16", V4L2_PIX_FMT_NV16 },
	{ "NV61", V4L2_PIX_FMT_NV61 },
	{ "NV24", V4L2_PIX_FMT_NV24 },
	{ "NV42", V4L2_PIX_FMT_NV42 },
	{ "SBGGR8", V4L2_PIX_FMT_SBGGR8 },
	{ "SGBRG8", V4L2_PIX_FMT_SGBRG8 },
	{ "SGRBG8", V4L2_PIX_FMT_SGRBG8 },
	{ "SRGGB8", V4L2_PIX_FMT_SRGGB8 },
	{ "SGRBG10_DPCM8", V4L2_PIX_FMT_SGRBG10DPCM8 },
	{ "SBGGR10", V4L2_PIX_FMT_SBGGR10 },
	{ "SGBRG10", V4L2_PIX_FMT_SGBRG10 },
	{ "SGRBG10", V4L2_PIX_FMT_SGRBG10 },
	{ "SRGGB10", V4L2_PIX_FMT_SRGGB10 },
	{ "SBGGR12", V4L2_PIX_FMT_SBGGR12 },
	{ "SGBRG12", V4L2_PIX_FMT_SGBRG12 },
	{ "SGRBG12", V4L2_PIX_FMT_SGRBG12 },
	{ "SRGGB12", V4L2_PIX_FMT_SRGGB12 },
	{ "DV", V4L2_PIX_FMT_DV },
	{ "MJPEG", V4L2_PIX_FMT_MJPEG },
	{ "MPEG", V4L2_PIX_FMT_MPEG },
	{ "VP8", V4L2_PIX_FMT_VP8 },
	{ "VP8S", V4L2_PIX_FMT_VP8_SIMULCAST },
};

const char *v4l2_format_name(unsigned int fourcc)
{
	static char name[5];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (pixel_formats[i].fourcc == fourcc)
			return pixel_formats[i].name;
	}

	for (i = 0; i < 4; ++i) {
		name[i] = fourcc & 0xff;
		fourcc >>= 8;
	}

	name[4] = '\0';
	return name;
}

unsigned int v4l2_format_code(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (strcasecmp(pixel_formats[i].name, name) == 0)
			return pixel_formats[i].fourcc;
	}

	return 0;
}

bool is_v4l2_vp8_format(unsigned int format)
{
	return format == V4L2_PIX_FMT_VP8
	    || format == V4L2_PIX_FMT_VP8_SIMULCAST;
}

void convert_yuv420_to_yuyv(unsigned char *src_planes[3], int src_stride[3],
			    unsigned int width, unsigned int height,
			    int dst_stride, unsigned char *dst_buf)
{
	unsigned int x, y;
	unsigned char *dst;
	unsigned char *yp, *up, *vp;
	for (y = 0; y < height; y++) {
		yp = src_planes[0] + y * src_stride[0];
		up = src_planes[1] + y / 2 * src_stride[1];
		vp = src_planes[2] + y / 2 * src_stride[2];
		dst = dst_buf + y * dst_stride;
		for (x = 0; x < width; x += 2) {
			*dst++ = *yp++;
			*dst++ = *up++;
			*dst++ = *yp++;
			*dst++ = *vp++;
		}
	}
}

void convert_yuyv_to_yuv420(const unsigned char *src_buf,
			    unsigned int width, unsigned int height,
			    unsigned char *dst_buf)
{
	unsigned int x, y;
	const unsigned char *p;
	unsigned char *Y, *U, *V;

	p = src_buf;
	Y = dst_buf;
	U = dst_buf + width * height;
	V = dst_buf + width * height + width * height / 4;
	for (y = 0; y < height; y+= 2) {
		for (x = 0; x < width; x += 2) {
			*Y++ = *p++;
			*U++ = (p[0] + p[width*2]) / 2;
			p++;
			*Y++ = *p++;
			*V++ = (p[0] + p[width*2]) / 2;
			p++;
		}
		for (x = 0; x < width; x += 2) {
			*Y++ = *p++;
			p++;
			*Y++ = *p++;
			p++;
		}
	}
}

static int clamp(int x)
{
	if (x < 0)
		return 0;
	else if (x > 255)
		return 255;
	else
		return x;
}

static void yuv_to_rgv(unsigned char Y1, unsigned char Cb, unsigned char Cr,
		       unsigned char *ER, unsigned char *EG, unsigned char *EB)
{
	// http://en.wikipedia.org/wiki/YUV
	int c = Y1 - 16;
	int d = Cb - 128;
	int e = Cr - 128;
	*ER = clamp((298 * c + 409 * e + 128) >> 8);
	*EG = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
	*EB = clamp((298 * c + 516 * d + 128) >> 8);
}

void convert_yuyv_to_rgb24(const unsigned char *src_buf,
			   unsigned int width, unsigned int height,
			   unsigned char *dst_buf)
{
	unsigned int x, y;
	const unsigned char *p;
	unsigned char *q;

	p = src_buf;
	q = dst_buf;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x += 2) {
			yuv_to_rgv(p[0], p[1], p[3], q, q + 1, q + 2);
			q += 3;
			yuv_to_rgv(p[2], p[1], p[3], q, q + 1, q + 2);
			q += 3;

			p += 4;
		}
	}
}

/* ------------------------------------------------------------------
 * Logging
 */
static void do_log(int level, const char *format, va_list ap)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	char *fmt = alloca(strlen(format) + 100);
	sprintf(fmt, "%d.%09ld pid=%d tid=%x v=%d %s%s%s\n",
		(int)now.tv_sec, now.tv_nsec, getpid(),
		(int)pthread_self(), level,
		thread_tag, thread_tag[0] ? " " : "", format);

	if (level <= log_verbose) {
		va_list ap2;
		va_copy(ap2, ap);
		FILE *fp = fopen(log_filename, "a");
		vfprintf(fp, fmt, ap2);
		fclose(fp);
	}

	if (level <= verbose) {
		vprintf(fmt, ap);
	}
}

void log_msg(int level, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	do_log(level, format, ap);
	va_end(ap);
}

/* Shorthand for log_msg(0). It may be helpful to set breakpoint on this
 * function when debugging. */
void log_err(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	do_log(0, format, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------
 * buffer
 */
int buffer_alloc(struct buffer *buf, unsigned int size, int offset, int padding)
{
	static int page_size = 0;
	int ret;

	memset(buf, 0, sizeof(*buf));
	if (page_size == 0)
		page_size = getpagesize();

	ret = posix_memalign(&buf->mem, page_size, size + offset + padding);
	if (ret < 0) {
		log_err("Unable to allocate buffer (%d)", ret);
		return -1;
	}
	buf->alloc_type = BUFFER_ALLOC_TYPE_MALLOC;
	buf->mem += offset;
	buf->size = size;
	buf->padding = padding;
	buf->offset = offset;
	return 0;
}

int buffer_mmap(struct buffer *buf, unsigned int size, int fd,
		unsigned int offset)
{
	memset(buf, 0, sizeof(*buf));
	buf->mem =
	    remote_mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			offset);
	if (buf->mem == MAP_FAILED) {
		log_err("Unable to map buffer: %s (%d)",
			strerror(errno), errno);
		return -1;
	}
	if (use_remote) {
		buf->remote_mem = buf->mem;
		buf->mem = malloc(size);
	}
	buf->alloc_type = BUFFER_ALLOC_TYPE_MMAP;
	buf->size = size;
	buf->padding = 0;
	return 0;
}

void buffer_foreign(struct buffer *buf, void *block, unsigned size, bool erase)
{
	if (erase)
		memset(buf, 0, sizeof(*buf));
	buf->alloc_type = BUFFER_ALLOC_TYPE_FOREIGN;
	buf->size = size;
	buf->mem = block;
}

int buffer_free(struct buffer *buf)
{
	int ret;
	switch (buf->alloc_type) {
	case BUFFER_ALLOC_TYPE_MMAP:
		if (use_remote) {
			free(buf->mem);
			buf->mem = buf->remote_mem;
		}
		ret = remote_munmap(buf->mem, buf->size);
		if (ret < 0) {
			log_err("Unable to unmap buffer: %s (%d)",
				strerror(errno), errno);
			return ret;
		}
		break;
	case BUFFER_ALLOC_TYPE_MALLOC:
		buf->mem -= buf->offset;
		free(buf->mem);
		break;
	case BUFFER_ALLOC_TYPE_FOREIGN:
	case BUFFER_ALLOC_TYPE_NONE:
		// do nothing
		break;
	}
	buf->alloc_type = BUFFER_ALLOC_TYPE_NONE;
	buf->mem = NULL;
	return 0;
}

void buffer_verify(struct buffer *buf)
{
	const uint8_t *data = buf->mem + buf->size;
	unsigned int errors = 0;
	unsigned int dirty = 0;
	unsigned int i;

	if (buf->padding == 0)
		return;

	for (i = 0; i < buf->padding; ++i) {
		if (data[i] != 0x55) {
			errors++;
			dirty = i + 1;
		}
	}

	if (errors) {
		log_msg(0,
			"Warning: %u bytes overwritten among %u first padding bytes",
			errors, dirty);

		dirty = (dirty + 15) & ~15;
		dirty = dirty > 32 ? 32 : dirty;

		for (i = 0; i < dirty; ++i) {
			printf("%02x ", data[i]);
			if (i % 16 == 15)
				printf("\n");
		}
	}
}

void buffer_fill_padding(struct buffer *buf)
{
	memset(buf->mem + buf->size, 0x55, buf->padding);
}

/* ------------------------------------------------------------------
 * command channel
 *          send_cmd       get_cmd
 *   client ----------------------> handler
 *          get_reply   send_reply
 *         <----------------------
 *
 * Command value and extra arguments of command are all "int".
 * Clients and handles must use the same protocol (correct number of arguments
 * and replys), otherwise subsequent commands will break.
 */
int chan_init(struct command_channel *chan)
{
	pthread_mutex_init(&chan->mutex, NULL);
	if (pipe(chan->pipe_cmd) < 0) {
		log_err("pipe failed");
		return -1;
	}
	if (pipe(chan->pipe_reply) < 0) {
		log_err("pipe failed");
		return -1;
	}
	return 0;
}

void chan_close(struct command_channel *chan)
{
	int i;
	for (i = 0; i < 2; i++) {
		if (chan->pipe_cmd[i] >= 0)
			close(chan->pipe_cmd[i]);
		if (chan->pipe_reply[i] >= 0)
			close(chan->pipe_reply[i]);
	}
	pthread_mutex_destroy(&chan->mutex);
}

/* Client calls chan_lock/chan_unlock to protect the exclusive communication
 * session. During lock, other clients have to wait until current client
 * finished the communication.
 */
void chan_lock(struct command_channel *chan)
{
	pthread_mutex_lock(&chan->mutex);
}

void chan_unlock(struct command_channel *chan)
{
	pthread_mutex_unlock(&chan->mutex);
}

/* Client sends command or arguments to handler.
 * Returns -1 if failed.
 */
int chan_send_cmd(struct command_channel *chan, int arg)
{
	int ret = write(chan->pipe_cmd[1], &arg, sizeof(arg));
	if (ret != sizeof(arg)) {
		log_err("write failed: %s (%d)", strerror(errno), errno);
		close(chan->pipe_cmd[1]);
		chan->pipe_cmd[1] = -1;
		pthread_mutex_unlock(&chan->mutex);
		return -1;
	}
	return 0;
}

/* Handler get command or arguments from client.
 * Returns -1 if failed.
 */
int chan_get_cmd(struct command_channel *chan, int *cmd)
{
	int ret = read(chan->pipe_cmd[0], cmd, sizeof(*cmd));
	if (ret != sizeof(*cmd)) {
		log_err("read failed: %s (%d)", strerror(errno), errno);
		close(chan->pipe_cmd[0]);
		chan->pipe_cmd[0] = -1;
		return -1;
	}
	return 0;
}

/* Handler send reply to client.
 * Returns -1 if failed.
 */
int chan_send_reply(struct command_channel *chan, int arg)
{
	int ret = write(chan->pipe_reply[1], &arg, sizeof(arg));
	if (ret != sizeof(arg)) {
		log_err("write failed: %s (%d)", strerror(errno), errno);
		close(chan->pipe_reply[1]);
		chan->pipe_reply[1] = -1;
		return -1;
	}
	return 0;
}

/* Client get reply from handler.
 * Returns -1 if failed.
 */
int chan_get_reply(struct command_channel *chan, int *reply)
{
	int ret = read(chan->pipe_reply[0], reply, sizeof(*reply));
	if (ret != sizeof(*reply)) {
		log_err("read failed: %s (%d)", strerror(errno), errno);
		close(chan->pipe_reply[0]);
		chan->pipe_reply[0] = -1;
		pthread_mutex_unlock(&chan->mutex);
		return -1;
	}
	return 0;
}
