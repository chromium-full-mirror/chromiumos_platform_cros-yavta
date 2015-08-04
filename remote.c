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
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "videodev2.h"
#include "remote.h"

int writelen(int fd, int size, const void *buf)
{
	while (size > 0) {
		int ret = write(fd, buf, size);
		if (ret == 0) {
			fprintf(stderr, "socket disconnected\n");
			return -1;
		} else if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("write");
			return ret;
		}
		buf += ret;
		size -= ret;
	}
	return 0;
}

int readlen(int fd, int size, void *buf)
{
	while (size > 0) {
		int ret = read(fd, buf, size);
		if (ret == 0) {
			fprintf(stderr, "socket disconnected\n");
			return -1;
		} else if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("read");
			return ret;
		}
		buf += ret;
		size -= ret;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * Padding
 * The padding schemes are different across different platforms. 64 bits
 * platforms may have more paddings or larger size for some types, ex. time_t
 * and pointer. In remote yavta, both server and client call
 * serialize_ioctl_payload() to remove paddings of ioctl payload before
 * transmission, and call deserialize_ioctl_payload() to insert proper paddings
 * after receiving data.
 */
#define IGNORE_SIZE(R) ((R) & ~IOCSIZE_MASK)

// Describe where the padding and the size are.
// The offset is relative to data before padding.
struct padding {
	int offset;
	int size;
};

// Padding type
// Note, the padding of x86-32 and arm-32 are different.
enum padding_type {
	PADDING_TYPE_UNKNOWN = -1,
	PADDING_TYPE_X86_64 = 0,
	PADDING_TYPE_ARM_32,
	PADDING_TYPE_X86_32,
	PADDING_TYPE_NUM
};

#define PADDING_RECORD_NUM 6
struct padding_scheme {
	// Payload size of ioctl excluding padding
	int payload_size;
	// Payload size of ioctl including padding
	int payload_size_padded;
	// Padding records. {0,0} means the end of records.
	struct padding padding[PADDING_TYPE_NUM][PADDING_RECORD_NUM];
};

struct padding_scheme padding_v4l2_buffer = {
	// x86-64, 88 bytes
	// x86-32, arm-32, 68 bytes
	68,
	sizeof(struct v4l2_buffer),
	{
	 [PADDING_TYPE_X86_64] = {
				  {20, 4},	// before timestamp
				  {24, 4},	// timestamp is 64bit
				  {28, 4},	// timestamp is 64bit
				  {56, 4},	// sizeof(m.planes) and sizeof(m.userptr) are 4 bytes more
				  {68, 4},	// padding for total size
				  {0, 0},
				  },
	 },
};

struct padding_scheme padding_v4l2_ext_controls = {
	// x86-64, 32 bytes
	// x86-32, arm-32, 24 bytes
	24,
	sizeof(struct v4l2_ext_controls),
	{
	 [PADDING_TYPE_X86_64] = {
				  {20, 4},	// before controls
				  {24, 4},	// sizeof(controls) is 8 bytes more
				  {0, 0},
				  },
	 },
};

struct padding_scheme padding_v4l2_format = {
	// x86-64, 208 bytes
	// x86-32, arm-32, 204 bytes
	204,
	sizeof(struct v4l2_format),
	{
	 [PADDING_TYPE_X86_64] = {
				  {4, 4},	// before fmt
				  {0, 0},
				  },
	 },
};

struct padding_scheme padding_v4l2_input = {
	// x86-64, arm-32, 80 bytes
	// x86-32, 76 bytes
	76,
	sizeof(struct v4l2_input),
	{
	 [PADDING_TYPE_X86_64] = {
				  {76, 4},	// padding for total size
				  {0, 0},
				  },
	 [PADDING_TYPE_ARM_32] = {
				  {76, 4},	// padding for total size
				  {0, 0},
				  },
	 },
};

/* Mapping from ioctl request number to padding scheme */
struct padding_scheme *padding_schemes[1 << _IOC_NRBITS] = {
	[_IOC_NR(VIDIOC_QUERYBUF)] = &padding_v4l2_buffer,
	[_IOC_NR(VIDIOC_QBUF)] = &padding_v4l2_buffer,
	[_IOC_NR(VIDIOC_DQBUF)] = &padding_v4l2_buffer,
	[_IOC_NR(VIDIOC_PREPARE_BUF)] = &padding_v4l2_buffer,
	[_IOC_NR(VIDIOC_G_EXT_CTRLS)] = &padding_v4l2_ext_controls,
	[_IOC_NR(VIDIOC_S_EXT_CTRLS)] = &padding_v4l2_ext_controls,
	[_IOC_NR(VIDIOC_TRY_EXT_CTRLS)] = &padding_v4l2_ext_controls,
	[_IOC_NR(VIDIOC_G_FMT)] = &padding_v4l2_format,
	[_IOC_NR(VIDIOC_S_FMT)] = &padding_v4l2_format,
	[_IOC_NR(VIDIOC_TRY_FMT)] = &padding_v4l2_format,
	[_IOC_NR(VIDIOC_ENUMINPUT)] = &padding_v4l2_input,
	// NOTE, following requests have padding issues, too. Since we don't
	// use them, just ignore.
	// VIDIOC_G_FBUF, VIDIOC_S_FBUF (v4l2_framebuffer)
	// VIDIOC_DQEVENT (v4l2_event)
	// VIDIOC_CREATE_BUFS (v4l2_create_buffers)
	// VIDIOC_ENUMSTD (v4l2_standard)
};

static enum padding_type determine_padding_type()
{
	if (_IOC_SIZE(VIDIOC_QUERYBUF) == 88)
		return PADDING_TYPE_X86_64;
	if (_IOC_SIZE(VIDIOC_QUERYBUF) == 68) {
		if (_IOC_SIZE(VIDIOC_ENUMINPUT) == 76)
			return PADDING_TYPE_X86_32;
		if (_IOC_SIZE(VIDIOC_ENUMINPUT) == 80)
			return PADDING_TYPE_ARM_32;
	}
	return PADDING_TYPE_UNKNOWN;
}

static const struct padding *query_padding(const struct padding_scheme *scheme)
{
	enum padding_type type = determine_padding_type();
	assert(type != PADDING_TYPE_UNKNOWN);
	if (scheme == NULL)
		return NULL;
	return scheme->padding[type];
}

int serialize_ioctl_payload(int request, const char *inbuf, char *outbuf)
{
	const char *src = inbuf;
	char *dst = outbuf;
	const struct padding_scheme *scheme = padding_schemes[_IOC_NR(request)];
	const struct padding *padding = query_padding(scheme);

	int size;
	if (padding) {
		int s;
		while (padding->offset) {
			s = padding->offset - (dst - outbuf);
			assert(s >= 0);
			memcpy(dst, src, s);
			src += s;
			dst += s;
			src += padding->size;
			padding++;
		}
		s = scheme->payload_size - (dst - outbuf);
		assert(s >= 0);
		memcpy(dst, src, s);
		size = scheme->payload_size;
	} else {
		size = _IOC_SIZE(request);
		memcpy(outbuf, inbuf, size);
	}
	assert(size <= _IOC_SIZE(request));
	return size;
}

int deserialize_ioctl_payload(int request, const char *inbuf, char *outbuf,
			      int len, int *req)
{
	const char *src = inbuf;
	char *dst = outbuf;
	const struct padding_scheme *scheme = padding_schemes[_IOC_NR(request)];
	const struct padding *padding = query_padding(scheme);

	if (scheme && scheme->payload_size != len) {
		fprintf(stderr,
			"packed payload size (%d) is not expected (%d)\n", len,
			scheme->payload_size);
		return -1;
	}

	int size;
	if (padding) {
		int s;
		while (padding->offset) {
			s = padding->offset - (src - inbuf);
			assert(s >= 0);
			memcpy(dst, src, s);
			src += s;
			dst += s;
			dst += padding->size;
			padding++;
		}
		s = scheme->payload_size - (src - inbuf);
		assert(s >= 0);
		memcpy(dst, src, s);
		size = scheme->payload_size_padded;
	} else {
		size = _IOC_SIZE(request);
		memcpy(outbuf, inbuf, size);
	}
	if (req)
		*req = IGNORE_SIZE(request) | size << IOCSIZE_SHIFT;
	return 0;
}
