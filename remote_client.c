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
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/tcp.h>		// TCP_NODELAY
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "videodev2.h"
#include "yavta.h"
#include "remote.h"

bool use_remote;
char remote_server_name[64];

/* Socket descriptor to remote server */
/* Because all threads may call syscalls simultaneously, the
 * connection is created independently for each thread. */
static __thread int remote_sock;

int connect_to_remote(int port)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	assert(sock > 0);
	if (sock < 0) {
		perror("socket()");
		exit(1);
	}

	int on = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(remote_server_name);

	serv_addr.sin_port = htons(port);
	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		perror("connect");
		exit(1);
	}
	return sock;
}

void maybe_connect_to_remote_server()
{
	if (remote_sock != 0)
		return;

	remote_sock = connect_to_remote(REMOTE_SERVER_PORT);
}

#define READ_SIZE(DATA_PTR, SIZE) \
	do { \
		assert(pos + SIZE <= REMOTE_COMMAND_BLOCK_SIZE); \
		memcpy(DATA_PTR, block + pos, SIZE); \
		pos += SIZE; \
	} while (0)
#define READ(var) READ_SIZE(&var, sizeof(var))

#define WRITE_SIZE(DATA_PTR, SIZE) \
	do { \
		assert(pos + SIZE <= REMOTE_COMMAND_BLOCK_SIZE); \
		memcpy(block + pos, DATA_PTR, SIZE); \
		pos += SIZE; \
	} while (0)
#define WRITE(var) WRITE_SIZE(&var, sizeof(var))

int remote_open(const char *path, int mode)
{
	if (!use_remote)
		return open(path, mode);

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	maybe_connect_to_remote_server();
	int ret;
	enum remote_command cmd = REMOTE_CMD_OPEN;
	WRITE(cmd);
	int len = strlen(path);
	WRITE(len);

	WRITE(mode);

	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;
	ret = writelen(remote_sock, len, path);
	if (ret < 0)
		return ret;
	ret = readlen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	pos = 0;
	int fd;
	int err;
	READ(fd);
	READ(err);

	errno = err;
	return fd;
}

int remote_close(int fd)
{
	if (!use_remote)
		return close(fd);

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	maybe_connect_to_remote_server();
	int ret;
	enum remote_command cmd = REMOTE_CMD_CLOSE;
	WRITE(cmd);
	WRITE(fd);

	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;
	ret = readlen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	pos = 0;
	int k;
	int err;
	READ(k);
	READ(err);

	errno = err;
	return k;
}

int remote_ioctl(int fd, int request, void *data)
{
	if (!use_remote)
		return ioctl(fd, request, data);

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	maybe_connect_to_remote_server();
	int ret;
	void *keep = NULL;

	int len = _IOC_SIZE(request);
	char *packed_data = (char *)alloca(len);
	memset(packed_data, 0, len);
	int packed_len =
	    serialize_ioctl_payload(request, (char *)data, packed_data);

	enum remote_command cmd = REMOTE_CMD_IOCTL;
	WRITE(cmd);

	WRITE(fd);
	WRITE(request);
	WRITE(packed_len);
	WRITE_SIZE(packed_data, packed_len);
	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	switch (request) {
	case VIDIOC_G_EXT_CTRLS:	// v4l2_ext_controls
	case VIDIOC_S_EXT_CTRLS:	// v4l2_ext_controls
		{
			struct v4l2_ext_controls *ctrls =
			    (struct v4l2_ext_controls *)data;
			int len = sizeof(ctrls->controls[0]) * ctrls->count;
			keep = ctrls->controls;
			ret = writelen(remote_sock, len, ctrls->controls);
			if (ret < 0)
				return ret;
		}
		break;
	}

	pos = 0;
	int k;
	int err;
	ret = readlen(remote_sock, sizeof(k) + sizeof(err) + packed_len, block);
	if (ret < 0)
		return ret;
	READ(k);
	READ(err);
	READ_SIZE(packed_data, packed_len);

	(void)deserialize_ioctl_payload(request, packed_data, (char *)data,
					packed_len, NULL);

	switch (request) {
	case VIDIOC_G_EXT_CTRLS:	// v4l2_ext_controls
	case VIDIOC_S_EXT_CTRLS:	// v4l2_ext_controls
		{
			struct v4l2_ext_controls *ctrls =
			    (struct v4l2_ext_controls *)data;
			int len = sizeof(ctrls->controls[0]) * ctrls->count;
			ctrls->controls = keep;
			ret = readlen(remote_sock, len, ctrls->controls);
			if (ret < 0)
				return ret;
		}
		break;
	}

	errno = err;
	return k;
}

/* Implementation of remote_mmap. This is a separated function because READ()
 * and WRITE() return int but mmap() expects void*. */
static int remote_mmap_imp(void *addr, int32_t length, int prot, int flags,
			   int fd, int32_t offset, void **result)
{
	int ret;
	remote_addr_t raddr = 0;
	if (addr) {
		raddr = *(remote_addr_t *) addr;
		free(addr);
	}

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	enum remote_command cmd = REMOTE_CMD_MMAP;
	WRITE(cmd);

	WRITE(raddr);
	WRITE(length);
	WRITE(prot);
	WRITE(flags);
	WRITE(fd);
	WRITE(offset);

	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;
	ret = readlen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	pos = 0;
	int err;
	READ(raddr);
	READ(err);

	errno = err;
	*result = malloc(sizeof(remote_addr_t));
	*(remote_addr_t *) * result = raddr;
	return 0;
}

void *remote_mmap(void *addr, size_t length, int prot, int flags,
		  int fd, off_t offset)
{
	if (!use_remote)
		return mmap(addr, length, prot, flags, fd, offset);

	maybe_connect_to_remote_server();
	void *result;
	int ret =
	    remote_mmap_imp(addr, length, prot, flags, fd, offset, &result);
	if (ret < 0)
		return MAP_FAILED;
	return result;
}

int remote_munmap(void *addr, size_t length)
{
	if (!use_remote)
		return munmap(addr, length);

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	maybe_connect_to_remote_server();
	int ret;
	remote_addr_t raddr = *(remote_addr_t *) addr;
	free(addr);

	enum remote_command cmd = REMOTE_CMD_MUNMAP;
	WRITE(cmd);

	WRITE(raddr);
	int32_t rlength = (int32_t) length;
	WRITE(rlength);

	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;
	ret = readlen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	pos = 0;
	int k;
	int err;
	READ(k);
	READ(err);

	errno = err;
	return k;
}

/* Wrapping for select() to wait file descriptor ready to read. */
/* See comments in handle_select_fd() */
int remote_select_fd(int sock, int fd, int token)
{
	int ret;

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	enum remote_command cmd = REMOTE_CMD_SELECT_FD;
	WRITE(cmd);
	WRITE(fd);
	WRITE(token);

	ret = writelen(sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	return 0;
}

int remote_readmem(void *src, void *dst, int size)
{
	int ret;
	remote_addr_t raddr = *(remote_addr_t *) src;
	enum remote_command cmd = REMOTE_CMD_READMEM;
	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	WRITE(cmd);
	WRITE(raddr);
	WRITE(size);

	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	ret = readlen(remote_sock, size, dst);
	if (ret < 0)
		return ret;

	return 0;
}

static int remote_bench(int n, int size, int count, bool send, double *t)
{
	enum remote_command cmd = REMOTE_CMD_BENCH;
	int i, j;
	int ret;

	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	WRITE(cmd);
	WRITE(n);
	WRITE(size);
	WRITE(count);
	WRITE(send);

	ret = writelen(remote_sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	char *tmp = (char *)malloc(size);
	if (tmp == NULL)
		return -1;
	memset(tmp, 0, size);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < n; i++) {
		if (send)
			for (j = 0; j < count; j++) {
				ret = writelen(remote_sock, size, tmp);
				if (ret < 0) {
					free(tmp);
					return ret;
				}
			}
		for (j = 0; j < count; j++) {
			ret = readlen(remote_sock, size, tmp);
			if (ret < 0) {
				free(tmp);
				return ret;
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	*t = (timespec_to_double(&t1) - timespec_to_double(&t0)) / n;
	free(tmp);

	return 0;
}

int benchmark_remote_overhead(void)
{
	int ret;
	double t1, t2, t3;

	log_msg(0, "Benchmarking");

	/* read/write single block */
	ret = remote_bench(1000, 64, 1, true, &t1);
	if (ret < 0)
		return ret;
	log_msg(0, "Average round-trip time: %.2fms (single block)", t1 * 1000);

	/* multiple read/write */
	ret = remote_bench(1000, 4, 16, true, &t2);
	if (ret < 0)
		return ret;
	log_msg(0, "Average round-trip time: %.2fms (multiple read/write)",
		t2 * 1000);

	int bps;
	int fps = 30;
	const int kb = 1024, mb = 1024 * 1024;
	for (bps = 128 * kb;; bps *= 2) {
		int size = bps / 8 / fps;
		ret = remote_bench(10, size, 1, false, &t3);
		if (ret < 0)
			return ret;

		/* current protocol: 5 round-trip per frame */
		double overhead = t2 * 5 + t3;
		if (overhead > 1.0 / fps) {
			bps /= 2;
			log_msg(0, "at most %.2fMbps for %dfps", 1.0 * bps / mb,
				fps);
			break;
		} else if (bps >= 1024 * mb) {
			log_msg(0, "at least %.2fMbps for %dfps",
				1.0 * bps / mb, fps);
			break;
		}
	}
	return 0;
}
