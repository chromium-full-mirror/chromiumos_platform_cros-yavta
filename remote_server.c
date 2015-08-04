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
// Assumptions:
// - Server and client are "similar" platforms, for example, same endianness
//   and function prototype.
// - Server fully trusts clients.
// - All clients share resources (fd, mmap). When a client disconnected, all
//   resources belong (opened or mmapped) by that client will be released.
// TODO verify values sent from client to prevent security attack

#include <alloca.h>
#include <asm-generic/ioctl.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/tcp.h>		// TCP_NODELAY
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "videodev2.h"
#include "remote.h"

TAILQ_HEAD(resource_fd_list, resource_fd);
struct resource_fd {
	int owner_sock;
	int fd;
	 TAILQ_ENTRY(resource_fd) entries;
};

TAILQ_HEAD(resource_mmap_list, resource_mmap);
struct resource_mmap {
	int owner_sock;
	remote_addr_t addr;
	int32_t length;
	 TAILQ_ENTRY(resource_mmap) entries;
};

struct resource {
	/* All fields in this structure are protected by mutex */
	pthread_mutex_t mutex;
	struct resource_fd_list fd_list;
	struct resource_mmap_list mmap_list;
};

struct resource g_resource;

void resource_init()
{
	struct resource *res = &g_resource;
	memset(res, 0, sizeof(struct resource));
	pthread_mutex_init(&res->mutex, NULL);
	TAILQ_INIT(&res->fd_list);
	TAILQ_INIT(&res->mmap_list);
}

void client_connected()
{
}

void client_disconnected(int sock)
{
	struct resource *res = &g_resource;
	pthread_mutex_lock(&res->mutex);

	struct resource_mmap *rmap, *rmap_next;
	for (rmap = TAILQ_FIRST(&res->mmap_list); rmap; rmap = rmap_next) {
		rmap_next = TAILQ_NEXT(rmap, entries);
		if (rmap->owner_sock != sock)
			continue;
		munmap((void *)rmap->addr, rmap->length);
		TAILQ_REMOVE(&res->mmap_list, rmap, entries);
		free(rmap);
	}

	struct resource_fd *rfd, *rfd_next;
	for (rfd = TAILQ_FIRST(&res->fd_list); rfd; rfd = rfd_next) {
		rfd_next = TAILQ_NEXT(rfd, entries);
		if (rfd->owner_sock != sock)
			continue;
		close(rfd->fd);
		TAILQ_REMOVE(&res->fd_list, rfd, entries);
		free(rfd);
	}

	pthread_mutex_unlock(&res->mutex);
}

bool is_valid_fd(struct resource *res, int fd)
{
	struct resource_fd *r;
	pthread_mutex_lock(&res->mutex);
	TAILQ_FOREACH(r, &res->fd_list, entries) {
		if (r->fd == fd) {
			pthread_mutex_unlock(&res->mutex);
			return true;
		}
	}
	pthread_mutex_unlock(&res->mutex);
	fprintf(stderr, "invalid fd(%d)\n", fd);
	return false;
}

bool is_valid_addr(struct resource *res, remote_addr_t addr, int length)
{
	struct resource_mmap *r;
	pthread_mutex_lock(&res->mutex);
	TAILQ_FOREACH(r, &res->mmap_list, entries) {
		if (r->addr <= addr && addr + length <= r->addr + r->length) {
			pthread_mutex_unlock(&res->mutex);
			return true;
		}
	}
	pthread_mutex_unlock(&res->mutex);
	fprintf(stderr, "invalid addr,size (%p, %u)", (void*)addr, length);
	return false;
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

int handle_open(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;

	int len;
	int err;
	char *path;
	int mode;

	READ(len);
	if (!(0 < len && len < 100)) {
		fprintf(stderr, "bad path size (%d)\n", len);
		return -1;
	}
	READ(mode);

	path = alloca(len + 1);
	ret = readlen(sock, len, path);
	if (ret < 0)
		return ret;
	path[len] = '\0';

	struct resource_fd *r =
	    (struct resource_fd *)malloc(sizeof(struct resource_fd));
	if (!r)
		return -1;

	int fd = open(path, mode);
	err = errno;
	r->owner_sock = sock;
	r->fd = fd;
	pthread_mutex_lock(&g_resource.mutex);
	TAILQ_INSERT_TAIL(&g_resource.fd_list, r, entries);
	pthread_mutex_unlock(&g_resource.mutex);

	pos = 0;
	WRITE(fd);
	WRITE(err);

	ret = writelen(sock, REMOTE_COMMAND_BLOCK_SIZE, block);
	if (ret < 0)
		return ret;

	return 0;
}

int handle_close(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;

	int fd;
	READ(fd);

	int k, err;

	struct resource *res = &g_resource;
	struct resource_fd *r;
	pthread_mutex_lock(&res->mutex);
	TAILQ_FOREACH(r, &res->fd_list, entries) {
		if (r->owner_sock == sock && r->fd == fd) {
			k = close(fd);
			err = errno;
			TAILQ_REMOVE(&res->fd_list, r, entries);
			free(r);
			break;
		}
	}
	pthread_mutex_unlock(&res->mutex);
	if (r == NULL) {
		fprintf(stderr, "Not owner or invalid fd(%d)\n", fd);
		return -1;
	}

	pos = 0;
	WRITE(k);
	WRITE(err);
	ret = writelen(sock, REMOTE_COMMAND_BLOCK_SIZE, block);
	if (ret < 0)
		return ret;
	return 0;
}

int handle_mmap(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;

	remote_addr_t addr;
	int32_t length;
	int prot;
	int flags;
	int fd;
	int32_t offset;

	READ(addr);
	READ(length);
	READ(prot);
	READ(flags);
	READ(fd);
	READ(offset);

	if (addr != 0) {
		fprintf(stderr, "unsupport mmap addr(%p) != NULL\n",
			(void*)addr);
		return -1;
	}

	struct resource_mmap *r =
	    (struct resource_mmap *)malloc(sizeof(struct resource_mmap));

	addr =
	    (remote_addr_t) mmap((void *)addr, length, prot, flags, fd, offset);
	int err = errno;

	r->owner_sock = sock;
	r->addr = addr;
	r->length = length;
	pthread_mutex_lock(&g_resource.mutex);
	TAILQ_INSERT_TAIL(&g_resource.mmap_list, r, entries);
	pthread_mutex_unlock(&g_resource.mutex);

	pos = 0;
	WRITE(addr);
	WRITE(err);
	ret = writelen(sock, REMOTE_COMMAND_BLOCK_SIZE, block);
	if (ret < 0)
		return ret;
	return 0;
}

int handle_munmap(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;
	remote_addr_t addr;
	int32_t length;

	READ(addr);
	READ(length);

	int k, err;

	struct resource *res = &g_resource;
	struct resource_mmap *r;
	pthread_mutex_lock(&res->mutex);
	TAILQ_FOREACH(r, &res->mmap_list, entries) {
		if (r->owner_sock == sock && r->addr == addr
		    && length == r->length) {
			k = munmap((void *)addr, length);
			err = errno;
			break;
		}
	}
	pthread_mutex_unlock(&res->mutex);
	if (r == NULL) {
		fprintf(stderr, "Not owner or invalid addr,size (%p, %u)",
			(void*)addr, (unsigned)length);
		return -1;
	}

	pos = 0;
	WRITE(k);
	WRITE(err);
	ret = writelen(sock, REMOTE_COMMAND_BLOCK_SIZE, block);
	if (ret < 0)
		return ret;
	return 0;
}

int handle_ioctl(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;

	int fd, request;
	void *data;
	READ(fd);
	READ(request);
	int packed_len;
	READ(packed_len);
	if (packed_len > _IOC_SIZE(request)) {
		fprintf(stderr, "ioctl payload size too large (%d)\n",
			packed_len);
		return -1;
	}
	if (packed_len + pos > REMOTE_COMMAND_BLOCK_SIZE) {
		fprintf(stderr,
			"REMOTE_COMMAND_BLOCK_SIZE(%d) is too small for ioctl payload (%d)\n",
			REMOTE_COMMAND_BLOCK_SIZE, packed_len);
		return -1;
	}
	char *packed_data = (char *)alloca(packed_len);
	data = alloca(packed_len * 2);	// *2 will be enough to insert padding
	READ_SIZE(packed_data, packed_len);

	if (!is_valid_fd(&g_resource, fd))
		return -1;

	ret =
	    deserialize_ioctl_payload(request, packed_data, data, packed_len,
				      &request);
	if (ret < 0)
		return -1;

	switch (request) {
	case VIDIOC_G_EXT_CTRLS:	// v4l2_ext_controls
	case VIDIOC_S_EXT_CTRLS:	// v4l2_ext_controls
		{
			struct v4l2_ext_controls *ctrls =
			    (struct v4l2_ext_controls *)data;
			int len = sizeof(ctrls->controls[0]) * ctrls->count;
			ctrls->controls = alloca(len);
			ret = readlen(sock, len, ctrls->controls);
			if (ret < 0)
				return ret;
		}
		break;
	}

	int k = ioctl(fd, request, data);
	int err = errno;

	int packed_len2 = serialize_ioctl_payload(request, data, packed_data);
	if (packed_len != packed_len2) {
		fprintf(stderr, "incorrect size (%d, %d)\n", packed_len,
			packed_len2);
	}

	pos = 0;
	WRITE(k);
	WRITE(err);
	WRITE_SIZE(packed_data, packed_len);

	ret = writelen(sock, pos, block);
	if (ret < 0)
		return ret;

	switch (request) {
	case VIDIOC_G_EXT_CTRLS:	// v4l2_ext_controls
	case VIDIOC_S_EXT_CTRLS:	// v4l2_ext_controls
		{
			struct v4l2_ext_controls *ctrls =
			    (struct v4l2_ext_controls *)data;
			int len = sizeof(ctrls->controls[0]) * ctrls->count;
			ret = writelen(sock, len, ctrls->controls);
			if (ret < 0)
				return ret;
		}
		break;
	}
	return 0;
}

/* Wrapping for select() to wait file descriptor ready to read. */
/* Since this is asynchronous command, this must use another connection along
 * with other synchronous command.
 * The protocol:
 * - Client sends (fd, token). If token = 0, stop select()
 * - Otherwise, server select() on fd. If the fd is ready, server sends back
 *   the token to client.
 * Client will use different token for select() each time to detect racing.
 */
int handle_select_fd(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;
	int fd, token;
	READ(fd);
	READ(token);

	if (!is_valid_fd(&g_resource, fd))
		return -1;
	if (token == 0)
		return 0;

	fd_set readset;
	FD_ZERO(&readset);
	FD_SET(fd, &readset);
	FD_SET(sock, &readset);

	int maxfd = fd;
	if (sock > maxfd)
		maxfd = sock;

	while (1) {
		int nfd = select(maxfd + 1, &readset, NULL, NULL, NULL);
		if (nfd < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			return -1;
		}
		if (FD_ISSET(sock, &readset)) {
			break;
		}
		if (FD_ISSET(fd, &readset)) {
			ret = writelen(sock, sizeof(token), &token);
			if (ret < 0)
				return ret;
			break;
		}
	}
	return 0;
}

int handle_readmem(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;
	remote_addr_t src;
	int size;

	READ(src);
	READ(size);

	if (!is_valid_addr(&g_resource, src, size))
		return -1;

	ret = writelen(sock, size, (void *)src);
	if (ret < 0)
		return ret;
	return 0;
}

int handle_bench(int sock, char block[REMOTE_COMMAND_BLOCK_SIZE], int pos)
{
	int ret;
	int i, j;
	int n, size, count;
	bool send;

	READ(n);
	READ(size);
	READ(count);
	READ(send);

	char *tmp = (char *)malloc(size);
	if (tmp == NULL)
		return -1;
	memset(tmp, 0, size);
	for (i = 0; i < n; i++) {
		if (send)
			for (j = 0; j < count; j++) {
				ret = readlen(sock, size, tmp);
				if (ret < 0) {
					free(tmp);
					return ret;
				}
			}
		for (j = 0; j < count; j++) {
			ret = writelen(sock, size, tmp);
			if (ret < 0) {
				free(tmp);
				return ret;
			}
		}
	}
	free(tmp);
	return 0;
}

int handler(int sock)
{
	int ret;
	enum remote_command cmd;
	char block[REMOTE_COMMAND_BLOCK_SIZE];
	int pos = 0;

	ret = readlen(sock, sizeof(block), block);
	if (ret < 0)
		return ret;

	READ(cmd);
	switch (cmd) {
	case REMOTE_CMD_OPEN:
		ret = handle_open(sock, block, pos);
		break;

	case REMOTE_CMD_CLOSE:
		ret = handle_close(sock, block, pos);
		break;

	case REMOTE_CMD_MMAP:
		ret = handle_mmap(sock, block, pos);
		break;

	case REMOTE_CMD_MUNMAP:
		ret = handle_munmap(sock, block, pos);
		break;

	case REMOTE_CMD_IOCTL:
		ret = handle_ioctl(sock, block, pos);
		break;

	case REMOTE_CMD_SELECT_FD:
		ret = handle_select_fd(sock, block, pos);
		break;

	case REMOTE_CMD_READMEM:
		ret = handle_readmem(sock, block, pos);
		break;

	case REMOTE_CMD_BENCH:
		ret = handle_bench(sock, block, pos);
		break;

	default:
		ret = -1;
		fprintf(stderr, "unknown cmd(%d)\n", cmd);
	}
	return ret;
}

void *serving_loop(void *arg)
{
	int clientfd = *(int *)arg;
	free(arg);
	client_connected();
	while (1) {
		int ret = handler(clientfd);
		if (ret < 0)
			break;
	}
	close(clientfd);
	client_disconnected(clientfd);
	return NULL;
}

int main(int argc, char *argv[])
{
	int opt;
	int port = 8888;
	while ((opt = getopt(argc, argv, "p:")) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	resource_init();

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket()");
		exit(1);
	}
	int on = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	// bind
	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		perror("bind");
		exit(1);
	}
	// listen
	listen(sock, 10);

	while (1) {
		struct sockaddr_in cli_addr;
		socklen_t clilen = sizeof(cli_addr);
		int clientfd =
		    accept(sock, (struct sockaddr *)&cli_addr, &clilen);
		if (clientfd < 0) {
			perror("accept");
			exit(1);
		}

		fprintf(stderr, "new client connected\n");

		pthread_t thread;
		int *p = (int *)malloc(sizeof(int));
		*p = clientfd;
		pthread_create(&thread, NULL, serving_loop, p);
	}
	return 0;
}
