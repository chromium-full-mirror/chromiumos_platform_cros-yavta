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
/* Functions to access v4l2 device and device filter */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "videodev2.h"

#include "yavta.h"
#include "remote.h"

#define ioctl remote_ioctl

bool is_safe_devname(const char *name)
{
	int id;
	unsigned int readn;
	return sscanf(name, "/dev/video%d%n", &id, &readn) == 1
	    && strlen(name) == readn;
}

void video_init(struct device *dev)
{
	memset(dev, 0, sizeof *dev);
	dev->fd = -1;
	dev->fd_select = -1;
	dev->memtype = V4L2_MEMORY_MMAP;
	dev->buffers = NULL;
	dev->type = (enum v4l2_buf_type)-1;
}

int video_open(struct device *dev, const char *devname, int no_query)
{
	struct v4l2_capability cap;
	int ret;

	dev->fd = remote_open(devname, O_RDWR);
	if (dev->fd < 0) {
		log_err("Error opening device %s: %s (%d).", devname,
			strerror(errno), errno);
		return dev->fd;
	}
	dev->devname = strdup(devname);
	if (use_remote) {
		dev->fd_select = connect_to_remote(REMOTE_SERVER_PORT);
		if (dev->fd_select < 0)
			return dev->fd_select;
	} else {
		dev->fd_select = dev->fd;
	}

	log_msg(0, "Device %s opened.", devname);

	if (no_query) {
		/* Assume capture device. */
		dev->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		return 0;
	}

	memset(&cap, 0, sizeof cap);
	log_msg(3, "ioctl(%d, VIDIOC_QUERYCAP)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0)
		return 0;

	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
		dev->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	else if (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)
		dev->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	else {
		log_err("Error opening device %s: neither video capture "
			"nor video output supported.\n", devname);
		return -EINVAL;
	}

	log_msg(0, "Device `%s' on `%s' is a video %s device.",
		cap.card, cap.bus_info,
		dev->type ==
		V4L2_BUF_TYPE_VIDEO_CAPTURE ? "capture" : "output");
	return 0;
}

void video_close(struct device *dev)
{
	log_msg(1, "video_close");
	free(dev->devname);
	free(dev->pattern);
	free(dev->buffers);
	if (dev->fd >= 0)
		remote_close(dev->fd);
	if (use_remote)
		close(dev->fd_select);
}

unsigned int get_control_type(struct device *dev, unsigned int id)
{
	struct v4l2_queryctrl query;
	int ret;

	memset(&query, 0, sizeof(query));

	query.id = id;
	log_msg(3, "ioctl(%d, VIDIOC_QUERYCTRL, {id=0x%x})", dev->fd, id);
	ret = ioctl(dev->fd, VIDIOC_QUERYCTRL, &query);
	if (ret == -1)
		return V4L2_CTRL_TYPE_INTEGER;

	return query.type;
}

int get_control(struct device *dev, unsigned int id, int type, uint32_t layer,
		int64_t *val)
{
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control ctrl;
	int ret;

	memset(&ctrls, 0, sizeof(ctrls));
	memset(&ctrl, 0, sizeof(ctrl));

	ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(id);
	ctrls.count = 1;
	ctrls.reserved[0] = layer;
	ctrls.controls = &ctrl;

	ctrl.id = id;

	log_msg(3, "ioctl(%d, VIDIOC_G_EXT_CTRLS, {count=%d})", dev->fd,
		ctrls.count);
	ret = ioctl(dev->fd, VIDIOC_G_EXT_CTRLS, &ctrls);
	if (ret != -1) {
		if (type == V4L2_CTRL_TYPE_INTEGER64)
			*val = ctrl.value64;
		else
			*val = ctrl.value;
		return 0;
	}
	if (errno == EINVAL || errno == ENOTTY) {
		struct v4l2_control old;

		old.id = id;
		log_msg(3, "ioctl(%d, VIDIOC_G_CTRL, {id=%d})", dev->fd, id);
		ret = ioctl(dev->fd, VIDIOC_G_CTRL, &old);
		if (ret != -1) {
			*val = old.value;
			return 0;
		}
	}

	log_err("unable to get control 0x%8.8x: %s (%d).",
		id, strerror(errno), errno);
	return -1;
}

int set_controls(struct device *dev, struct v4l2_ext_controls *ctrls)
{
	int ret;

	log_msg(3, "ioctl(%d, VIDIOC_S_EXT_CTRLS, {count=%d})", dev->fd,
		ctrls->count);
	ret = ioctl(dev->fd, VIDIOC_S_EXT_CTRLS, ctrls);
	if (ret == -1) {
		log_err("unable to set controls: %s (%d)",
			strerror(errno), errno);
		return ret;
	}

	return 0;
}

int set_control(struct device *dev, unsigned int id, int type,
		uint32_t layer, int64_t val)
{
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control ctrl;
	int is_64 = type == V4L2_CTRL_TYPE_INTEGER64;
	int64_t old_val = val;
	int ret;

	memset(&ctrls, 0, sizeof(ctrls));
	memset(&ctrl, 0, sizeof(ctrl));

	ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(id);
	ctrls.count = 1;
	ctrls.reserved[0] = layer;
	ctrls.controls = &ctrl;

	ctrl.id = id;
	if (is_64)
		ctrl.value64 = val;
	else
		ctrl.value = val;

	log_msg(3, "ioctl(%d, VIDIOC_S_EXT_CTRLS, {reserved[0]=%d, count=%d})",
		dev->fd, ctrls.reserved[0], ctrls.count);
	ret = ioctl(dev->fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	if (ret != -1) {
		if (is_64)
			val = ctrl.value64;
		else
			val = ctrl.value;
	} else if (errno == EINVAL || errno == ENOTTY) {
		struct v4l2_control old;

		old.id = id;
		log_msg(3, "ioctl(%d, VIDIOC_S_CTRL, {id=0x%x})", dev->fd, id);
		ret = ioctl(dev->fd, VIDIOC_S_CTRL, &old);
		if (ret != -1)
			val = old.value;
	}
	if (ret == -1) {
		log_err("unable to set control 0x%8.8x: %s (%d).",
			id, strerror(errno), errno);
		return ret;
	}

	log_msg(0, "Control 0x%08x set to %" PRId64 ", is %" PRId64,
		id, old_val, val);
	return 0;
}

int video_get_format(struct device *dev)
{
	struct v4l2_format fmt;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = dev->type;

	log_msg(3, "ioctl(%d, VIDIOC_G_FMT)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
		log_err("Unable to get format: %s (%d).", strerror(errno),
			errno);
		return ret;
	}

	dev->width = fmt.fmt.pix.width;
	dev->height = fmt.fmt.pix.height;
	dev->src_fmt = fmt;

	log_msg(0, "Video format: %s (%08x) %ux%u (stride %u) buffer size %u",
		v4l2_format_name(fmt.fmt.pix.pixelformat),
		fmt.fmt.pix.pixelformat,
		fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
		fmt.fmt.pix.sizeimage);
	return 0;
}

int video_set_format(struct device *dev, unsigned int w, unsigned int h,
		     unsigned int format, unsigned int stride)
{
	struct v4l2_format fmt;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = dev->type;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = format;
	fmt.fmt.pix.bytesperline = stride;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	log_msg(3,
		"ioctl(%d, VIDIOC_S_FMT, {width=%d, height=%d, pixelformat=%s)",
		dev->fd, w, h, v4l2_format_name(format));
	ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		log_err("Unable to set format: %s (%d).", strerror(errno),
			errno);
		return ret;
	}

	log_msg(0,
		"Video format set: %s (%08x) %ux%u (stride %u) buffer size %u",
		v4l2_format_name(fmt.fmt.pix.pixelformat),
		fmt.fmt.pix.pixelformat, fmt.fmt.pix.width, fmt.fmt.pix.height,
		fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

	return 0;
}

int video_get_streamparm(struct device *dev, struct v4l2_streamparm *parm)
{
	memset(parm, 0, sizeof *parm);
	parm->type = dev->type;

	log_msg(3, "ioctl(%d, VIDIOC_G_PARM)", dev->fd);
	int ret = ioctl(dev->fd, VIDIOC_G_PARM, parm);
	if (ret < 0) {
		log_err("Unable to get streamparm: %s (%d).",
			strerror(errno), errno);
		return ret;
	}

	return 0;
}

int video_get_framerate(struct device *dev, struct v4l2_fract *time_per_frame)
{
	struct v4l2_streamparm parm;
	int ret = video_get_streamparm(dev, &parm);

	if (ret < 0)
		return ret;

	log_msg(0, "Current frame rate: %u/%u",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);

	time_per_frame->numerator = parm.parm.capture.timeperframe.numerator;
	time_per_frame->denominator =
	    parm.parm.capture.timeperframe.denominator;

	return 0;
}

int video_set_framerate(struct device *dev, struct v4l2_fract *time_per_frame)
{
	struct v4l2_streamparm parm;
	int ret;

	memset(&parm, 0, sizeof parm);
	parm.type = dev->type;

	log_msg(3, "ioctl(%d, VIDIOC_G_PARM)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		log_err("Unable to get frame rate: %s (%d).",
			strerror(errno), errno);
		return ret;
	}

	log_msg(0, "Current frame rate: %u/%u",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);

	log_msg(0, "Setting frame rate to: %u/%u",
		time_per_frame->numerator, time_per_frame->denominator);

	parm.parm.capture.timeperframe.numerator = time_per_frame->numerator;
	parm.parm.capture.timeperframe.denominator =
	    time_per_frame->denominator;

	log_msg(3, "ioctl(%d, VIDIOC_S_PARM, {timeperframe=%u/%u})", dev->fd,
		time_per_frame->numerator, time_per_frame->denominator);
	ret = ioctl(dev->fd, VIDIOC_S_PARM, &parm);
	if (ret < 0) {
		log_err("Unable to set frame rate: %s (%d).", strerror(errno),
			errno);
		return ret;
	}

	log_msg(3, "ioctl(%d, VIDIOC_G_PARM)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		log_err("Unable to get frame rate: %s (%d).", strerror(errno),
			errno);
		return ret;
	}

	log_msg(0, "Frame rate set: %u/%u",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);
	return 0;
}

static int video_alloc_buffers(struct device *dev, int nbufs,
			       unsigned int offset, unsigned int padding)
{
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer buf;
	struct buffer *buffers;
	unsigned int i;
	int ret;

	memset(&rb, 0, sizeof rb);
	rb.count = nbufs;
	rb.type = dev->type;
	rb.memory = dev->memtype;

	log_msg(3, "ioctl(%d, VIDIOC_REQBUFS, {count=%d, type=%d, memory=%d})",
		dev->fd, rb.count, rb.type, rb.memory);
	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		log_err("Unable to request buffers: %s (%d).", strerror(errno),
			errno);
		return ret;
	}

	log_msg(0, "%u buffers requested.", rb.count);

	buffers = malloc(rb.count * sizeof buffers[0]);
	if (buffers == NULL)
		return -ENOMEM;

	/* Map the buffers. */
	for (i = 0; i < rb.count; ++i) {
		memset(&buf, 0, sizeof buf);
		buf.index = i;
		buf.type = dev->type;
		buf.memory = dev->memtype;
		log_msg(3,
			"ioctl(%d, VIDIOC_QUERYBUF, {index=%d, type=%d, memory=%d})",
			dev->fd, buf.index, buf.type, buf.memory);
		ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			log_err("Unable to query buffer %u: %s (%d).", i,
				strerror(errno), errno);
			return ret;
		}
		log_msg(1, "length: %u offset: %u", buf.length, buf.m.offset);

		switch (dev->memtype) {
		case V4L2_MEMORY_MMAP:
			ret =
			    buffer_mmap(&buffers[i], buf.length, dev->fd,
					buf.m.offset);
			if (ret < 0)
				return ret;
			log_msg(1, "Buffer %u mapped at address %p.", i,
				buffers[i].mem);
			break;

		case V4L2_MEMORY_USERPTR:
			ret =
			    buffer_alloc(&buffers[i], buf.length, offset,
					 padding);
			if (ret < 0)
				return ret;
			log_msg(1, "Buffer %u allocated at address %p.", i,
				buffers[i].mem);
			break;

		default:
			break;
		}
		buffers[i].big_enough_for = dev->width * dev->height;
	}

	dev->buffers = buffers;
	dev->nbufs = rb.count;
	return 0;
}

static int video_free_buffers(struct device *dev)
{
	struct v4l2_requestbuffers rb;
	unsigned int i;
	int ret;

	if (dev->nbufs == 0)
		return 0;

	for (i = 0; i < dev->nbufs; ++i) {
		switch (dev->memtype) {
		case V4L2_MEMORY_MMAP:
		case V4L2_MEMORY_USERPTR:
			ret = buffer_free(&dev->buffers[i]);
			if (ret < 0)
				return ret;
			break;

		default:
			break;
		}
	}

	memset(&rb, 0, sizeof rb);
	rb.count = 0;
	rb.type = dev->type;
	rb.memory = dev->memtype;

	log_msg(3, "ioctl(%d, VIDIOC_REQBUFS, {count=%d})", dev->fd, rb.count);
	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		log_err("Unable to release buffers: %s (%d).",
			strerror(errno), errno);
		return ret;
	}

	log_msg(0, "%u buffers released.", dev->nbufs);

	free(dev->buffers);
	dev->nbufs = 0;
	dev->buffers = NULL;

	return 0;
}

static int video_queue_buffer(struct device *dev, struct buffer *buffer,
			      enum buffer_fill_mode fill)
{
	struct v4l2_buffer buf;
	int ret;

	memset(&buf, 0, sizeof buf);
	buf.index = buffer->index;
	buf.type = dev->type;
	buf.memory = dev->memtype;
	buf.length = buffer->size;
	if (dev->memtype == V4L2_MEMORY_USERPTR)
		buf.m.userptr = (unsigned long)buffer->mem;

	if (dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		buf.bytesused = dev->patternsize;
		memcpy(buffer->mem, dev->pattern, dev->patternsize);
	} else {
		if (fill & BUFFER_FILL_FRAME)
			memset(buffer->mem, 0x55, buffer->size);
		if (fill & BUFFER_FILL_PADDING)
			buffer_fill_padding(buffer);
	}

	log_msg(3, "ioctl(%d, VIDIOC_QBUF, {index=%d, length=%d})", dev->fd,
		buf.index, buf.length);
	ret = ioctl(dev->fd, VIDIOC_QBUF, &buf);
	log_msg(3, "ret %d", ret);
	if (ret < 0) {
		log_err("Unable to queue buffer: %s (%d).",
			strerror(errno), errno);
	} else {
		dev->nbufs_queued++;
		log_msg(4, "queued buffer in driver: %d", dev->nbufs_queued);
	}

	return ret;
}

static int video_dequeue_buffer(struct device *dev, struct v4l2_buffer *buf)
{
	int ret;

	memset(buf, 0, sizeof *buf);
	buf->type = dev->type;
	buf->memory = dev->memtype;
	log_msg(3, "ioctl(%d, VIDIOC_DQBUF)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_DQBUF, buf);
	log_msg(3, "ioctl return %d, index=%d", ret, buf->index);
	if (ret < 0) {
		if (errno != EIO) {
			log_err("Unable to dequeue buffer: %s (%d).",
				strerror(errno), errno);
			return ret;
		}
		// TODO(kcwu): may be signal loss.
		buf->type = dev->type;
		buf->memory = dev->memtype;
	}
	assert(dev->nbufs_queued > 0);
	dev->nbufs_queued--;

	return 0;
}

int video_enable(struct device *dev, int enable)
{
	int type = dev->type;
	int ret;

	log_msg(3, "ioctl(%d, %s)", dev->fd,
		enable ? "VIDIOC_STREAMON" : "VIDIOC_STREAMOFF");
	ret =
	    ioctl(dev->fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
	log_msg(3, "ret %d", ret);
	if (ret < 0) {
		log_err("Unable to %s streaming: %s (%d).",
			enable ? "start" : "stop", strerror(errno), errno);
		return ret;
	}

	return 0;
}

int video_query_menu_idx(struct device *dev, int ctrl_id, int idx,
			 struct v4l2_querymenu *menu)
{
	int ret;
	memset(menu, 0, sizeof(*menu));
	menu->id = ctrl_id;
	menu->index = idx;
	log_msg(3, "ioctl(%d, VIDIOC_QUERYMENU, {id=0x%x, index=%d})", dev->fd,
		menu->id, menu->index);
	ret = ioctl(dev->fd, VIDIOC_QUERYMENU, menu);
	return ret;
}

void video_query_menu(struct device *dev, struct v4l2_queryctrl *query)
{
	struct v4l2_querymenu menu;
	int ret;

	memset(&menu, 0, sizeof(menu));
	for (menu.index = query->minimum;
	     menu.index <= (unsigned)query->maximum; menu.index++) {
		menu.id = query->id;
		log_msg(3, "ioctl(%d, VIDIOC_QUERYMENU, {id=0x%x, index=%d})",
			dev->fd, menu.id, menu.index);
		ret = ioctl(dev->fd, VIDIOC_QUERYMENU, &menu);
		if (ret < 0)
			continue;

		if (query->type == V4L2_CTRL_TYPE_MENU)
			log_msg(0, "  %u: %.32s", menu.index, menu.name);
		else
			log_msg(0, "  %u: %lld", menu.index, menu.value);
	}
}

/* Iterator to get all v4l2_queryctrl.
 * At beginning, query->id should be initialized as zero. And caller should not
 * modify query->id.
 */
bool video_control_iter_next(struct device *dev, struct v4l2_queryctrl *query)
{
	if (query->id == 0)
		query->id = V4L2_CID_BASE;
	else {
#ifndef V4L2_CTRL_FLAG_NEXT_CTRL
		query->id++;
		if (query->id > V4L2_CID_LASTP1)
			return false;
#else
		query->id |= V4L2_CTRL_FLAG_NEXT_CTRL;
#endif
	}
	log_msg(3, "ioctl(%d, VIDIOC_QUERYCTRL, {id=0x%08x})", dev->fd,
		query->id);
	return ioctl(dev->fd, VIDIOC_QUERYCTRL, query) == 0;
}

void video_set_all_controls(struct device *dev,
			    enum ctrls_set_mode op, uint32_t layer)
{
	struct v4l2_queryctrl query;
	char value[24];
	int64_t val64;
	int ret;

	switch (op) {
	case SET_DEF:
		log_msg(0, "Setting all controls to default values");
		break;
	case SET_MIN:
		log_msg(0, "Setting all controls to minimum values");
		break;
	case SET_MAX:
		log_msg(0, "Setting all controls to maximum values");
		break;
	case SET_ALL:
		log_msg(0, "Setting all controls to all valid values");
		break;
	default:
		return;
	}

	memset(&query, 0, sizeof(query));
	while (video_control_iter_next(dev, &query)) {
		if (query.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		ret = get_control(dev, query.id, query.type, layer, &val64);
		if (ret < 0) {
			log_err("Failed getting default for control %d",
				query.id);
			continue;
		} else {
			sprintf(value, "%" PRId64, val64);
		}

		log_msg(0,
			"control 0x%08x `%s' min %d max %d step %d default %d current %s.",
			query.id, query.name, query.minimum, query.maximum,
			query.step, query.default_value, value);

		switch (op) {
		case SET_DEF:
			set_control(dev, query.id, query.type, layer,
				    query.default_value);
			break;
		case SET_MIN:
			set_control(dev, query.id, query.type, layer,
				    query.minimum);
			break;
		case SET_MAX:
			set_control(dev, query.id, query.type, layer,
				    query.maximum);
			break;

		case SET_ALL:
			if (query.minimum > query.maximum)
				log_err("Invalid control: reported MIN > MAX");
			for (val64 = query.minimum; val64 <= query.maximum;
			     val64 += query.step)
				set_control(dev, query.id, query.type,
					    layer, val64);
			break;
		default:
			return;
		}
	}
}

void video_list_controls(struct device *dev, uint32_t layer)
{
	struct v4l2_queryctrl query;
	unsigned int nctrls = 0;
	char value[24];
	int64_t val64;
	int ret;

	memset(&query, 0, sizeof(query));
	while (video_control_iter_next(dev, &query)) {
		if (query.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		if (query.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
			log_msg(0, "--- %s (class 0x%08x) ---", query.name,
				query.id);
			continue;
		}

		ret = get_control(dev, query.id, query.type, layer, &val64);
		if (ret < 0)
			strcpy(value, "n/a");
		else
			sprintf(value, "%" PRId64, val64);

		log_msg(0,
			"control 0x%08x `%s' min %d max %d step %d default %d current %s.",
			query.id, query.name, query.minimum, query.maximum,
			query.step, query.default_value, value);

		if (query.type == V4L2_CTRL_TYPE_MENU ||
		    query.type == V4L2_CTRL_TYPE_INTEGER_MENU)
			video_query_menu(dev, &query);

		nctrls++;
	}

	if (nctrls)
		log_msg(0, "%u control%s found.", nctrls,
			nctrls > 1 ? "s" : "");
	else
		log_msg(0, "No control found.");
}

int video_get_frame_interval_idx(struct device *dev, __u32 pixelformat,
				 unsigned int width, unsigned int height,
				 unsigned int idx,
				 struct v4l2_frmivalenum *ival)
{
	int ret;

	memset(ival, 0, sizeof *ival);
	ival->index = idx;
	ival->pixel_format = pixelformat;
	ival->width = width;
	ival->height = height;
	log_msg(3, "ioctl(%d, VIDIOC_ENUM_FRAMEINTERVALS)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMEINTERVALS, ival);
	if (ret < 0)
		return -1;

	if (idx != ival->index)
		log_msg(0, "Warning: driver returned wrong ival index "
			"%u.\n", ival->index);
	if (pixelformat != ival->pixel_format)
		log_msg(0, "Warning: driver returned wrong ival pixel "
			"format %08x.\n", ival->pixel_format);
	if (width != ival->width)
		log_msg(0, "Warning: driver returned wrong ival width "
			"%u.\n", ival->width);
	if (height != ival->height)
		log_msg(0, "Warning: driver returned wrong ival height "
			"%u.\n", ival->height);
	return 0;
}

void video_enum_frame_intervals(struct device *dev, __u32 pixelformat,
				unsigned int width, unsigned int height)
{
	struct v4l2_frmivalenum ival;
	unsigned int i;
	int ret;

	for (i = 0;; ++i) {
		ret =
		    video_get_frame_interval_idx(dev, pixelformat, width,
						 height, i, &ival);
		if (ret < 0)
			break;

		if (i != 0)
			printf(", ");

		switch (ival.type) {
		case V4L2_FRMIVAL_TYPE_DISCRETE:
			printf("%u/%u",
			       ival.discrete.numerator,
			       ival.discrete.denominator);
			break;

		case V4L2_FRMIVAL_TYPE_CONTINUOUS:
			printf("%u/%u - %u/%u",
			       ival.stepwise.min.numerator,
			       ival.stepwise.min.denominator,
			       ival.stepwise.max.numerator,
			       ival.stepwise.max.denominator);
			return;

		case V4L2_FRMIVAL_TYPE_STEPWISE:
			printf("%u/%u - %u/%u (by %u/%u)",
			       ival.stepwise.min.numerator,
			       ival.stepwise.min.denominator,
			       ival.stepwise.max.numerator,
			       ival.stepwise.max.denominator,
			       ival.stepwise.step.numerator,
			       ival.stepwise.step.denominator);
			return;

		default:
			break;
		}
	}
}

int video_get_frame_size_idx(struct device *dev, __u32 pixelformat,
			     unsigned int idx, struct v4l2_frmsizeenum *frame)
{
	int ret;

	memset(frame, 0, sizeof *frame);
	frame->index = idx;
	frame->pixel_format = pixelformat;
	log_msg(3, "ioctl(%d, VIDIOC_ENUM_FRAMESIZES)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, frame);
	if (ret < 0)
		return -1;

	if (idx != frame->index)
		log_msg(0, "Warning: driver returned wrong frame index "
			"%u.\n", frame->index);
	if (pixelformat != frame->pixel_format)
		log_msg(0, "Warning: driver returned wrong frame pixel "
			"format %08x.\n", frame->pixel_format);

	return 0;
}

void video_enum_frame_sizes(struct device *dev, __u32 pixelformat)
{
	struct v4l2_frmsizeenum frame;
	unsigned int i;
	int ret;

	for (i = 0;; ++i) {
		ret = video_get_frame_size_idx(dev, pixelformat, i, &frame);
		if (ret < 0)
			break;

		switch (frame.type) {
		case V4L2_FRMSIZE_TYPE_DISCRETE:
			printf("\tFrame size: %ux%u (", frame.discrete.width,
			       frame.discrete.height);
			video_enum_frame_intervals(dev, frame.pixel_format,
						   frame.discrete.width,
						   frame.discrete.height);
			printf(")\n");
			break;

		case V4L2_FRMSIZE_TYPE_CONTINUOUS:
			printf("\tFrame size: %ux%u - %ux%u (",
			       frame.stepwise.min_width,
			       frame.stepwise.min_height,
			       frame.stepwise.max_width,
			       frame.stepwise.max_height);
			video_enum_frame_intervals(dev, frame.pixel_format,
						   frame.stepwise.max_width,
						   frame.stepwise.max_height);
			printf(")\n");
			break;

		case V4L2_FRMSIZE_TYPE_STEPWISE:
			printf("\tFrame size: %ux%u - %ux%u (by %ux%u) (\n",
			       frame.stepwise.min_width,
			       frame.stepwise.min_height,
			       frame.stepwise.max_width,
			       frame.stepwise.max_height,
			       frame.stepwise.step_width,
			       frame.stepwise.step_height);
			video_enum_frame_intervals(dev, frame.pixel_format,
						   frame.stepwise.max_width,
						   frame.stepwise.max_height);
			printf(")\n");
			break;

		default:
			break;
		}
	}
}

int video_get_formats_idx(struct device *dev, enum v4l2_buf_type type,
			  unsigned int idx, struct v4l2_fmtdesc *fmt)
{
	int ret;

	memset(fmt, 0, sizeof *fmt);
	fmt->index = idx;
	fmt->type = type;
	log_msg(3, "ioctl(%d, VIDIOC_ENUM_FMT)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_ENUM_FMT, fmt);
	if (ret < 0)
		return -1;

	if (idx != fmt->index)
		log_msg(0, "Warning: driver returned wrong format index "
			"%u.\n", fmt->index);
	if (type != fmt->type)
		log_msg(0, "Warning: driver returned wrong format type "
			"%u.\n", fmt->type);
	return 0;
}

void video_enum_formats(struct device *dev, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc fmt;
	unsigned int i;
	int ret;

	for (i = 0;; ++i) {
		ret = video_get_formats_idx(dev, type, i, &fmt);
		if (ret < 0)
			break;

		printf("\tFormat %u: %s (%08x)\n", i,
		       v4l2_format_name(fmt.pixelformat), fmt.pixelformat);
		printf("\tType: %s (%u)\n", v4l2_buf_type_name(fmt.type),
		       fmt.type);
		printf("\tName: %.32s\n", fmt.description);
		video_enum_frame_sizes(dev, fmt.pixelformat);
		printf("\n");
	}
}

bool video_input_iter(struct device *dev, unsigned int idx,
		      struct v4l2_input *input)
{
	int ret;
	memset(input, 0, sizeof *input);
	input->index = idx;
	log_msg(3, "ioctl(%d, VIDIOC_ENUMINPUT, {index=%d})", dev->fd, idx);
	ret = ioctl(dev->fd, VIDIOC_ENUMINPUT, input);
	if (ret < 0)
		return false;

	if (idx != input->index)
		log_msg(0, "Warning: driver returned wrong input index "
			"%u.\n", input->index);

	return true;
}

void video_enum_inputs(struct device *dev)
{
	struct v4l2_input input;
	unsigned int i;

	for (i = 0; video_input_iter(dev, i, &input); ++i) {
		log_msg(0, "\tInput %u: %s.", i, input.name);
	}

	printf("\n");
}

int video_get_input(struct device *dev)
{
	__u32 input;
	int ret;

	log_msg(3, "ioctl(%d, VIDIOC_G_INPUT)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_G_INPUT, &input);
	if (ret < 0) {
		log_err("Unable to get current input: %s (%d).",
			strerror(errno), errno);
		return ret;
	}

	return input;
}

int video_set_input(struct device *dev, unsigned int input)
{
	__u32 _input = input;
	int ret;

	log_msg(3, "ioctl(%d, VIDIOC_S_INPUT, %d)", dev->fd, input);
	ret = ioctl(dev->fd, VIDIOC_S_INPUT, &_input);
	if (ret < 0)
		log_err("Unable to select input %u: %s (%d).", input,
			strerror(errno), errno);

	return ret;
}

int video_set_quality(struct device *dev, unsigned int quality)
{
	struct v4l2_jpegcompression jpeg;
	int ret;

	if (quality == (unsigned int)-1)
		return 0;

	memset(&jpeg, 0, sizeof jpeg);
	jpeg.quality = quality;

	log_msg(3, "ioctl(%d, VIDIOC_S_JPEGCOMP, {quality=%d})", dev->fd,
		quality);
	ret = ioctl(dev->fd, VIDIOC_S_JPEGCOMP, &jpeg);
	if (ret < 0) {
		log_err("Unable to set quality to %u: %s (%d).", quality,
			strerror(errno), errno);
		return ret;
	}

	log_msg(3, "ioctl(%d, VIDIOC_G_JPEGCOMP)", dev->fd);
	ret = ioctl(dev->fd, VIDIOC_G_JPEGCOMP, &jpeg);
	if (ret >= 0)
		log_msg(0, "Quality set to %u", jpeg.quality);

	return 0;
}

int video_load_test_pattern(struct device *dev, const char *filename)
{
	unsigned int size = dev->buffers[0].size;
	unsigned int x, y;
	uint8_t *data;
	int ret;

	/* Load or generate the test pattern */
	dev->pattern = malloc(size);
	if (dev->pattern == NULL)
		return -ENOMEM;

	if (filename == NULL) {
		if (dev->src_fmt.fmt.pix.bytesperline == 0) {
			log_err("Compressed format detect and no test pattern filename given."
			        "The test pattern can't be generated automatically.\n");
			return -EINVAL;
		}

		data = dev->pattern;

		for (y = 0; y < dev->height; ++y) {
			for (x = 0; x < dev->src_fmt.fmt.pix.bytesperline; ++x)
				*data++ = x + y;
		}

		return 0;
	}

	FILE *fp = fopen(filename, "rb");
	if (fp == NULL) {
		log_err("Unable to open test pattern file '%s': %s (%d).",
			filename, strerror(errno), errno);
		return -errno;
	}

	ret = fread(dev->pattern, 1, size, fp);
	fclose(fp);

	if (ret != (int)size && dev->bytesperline != 0) {
		log_err("Test pattern file size %u doesn't match image size %u",
			ret, size);
		return -EINVAL;
	}

	dev->patternsize = ret;
	return 0;
}

static int video_prepare_buffer(struct stream *s)
{
	struct device *dev = &s->dev;
	unsigned int padding;
	int ret;
	/* Allocate and map buffers. */
	padding = (s->option.fill_mode & BUFFER_FILL_PADDING) ? 4096 : 0;
	ret = video_alloc_buffers(dev, s->option.nbufs,
				  s->option.userptr_offset, padding);
	if (ret < 0)
		return ret;

	if (dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		ret = video_load_test_pattern(dev, s->option.filename);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int video_prepare_capture(struct stream *s)
{
	struct device *dev = &s->dev;
	unsigned int i;
	int ret;

	/* Set the compression quality. */
	if (video_set_quality(&s->dev, s->option.quality) < 0) {
		return -1;
	}

	ret = video_prepare_buffer(s);
	if (ret < 0)
		return ret;

	for (i = 0; i < dev->nbufs; ++i) {
		struct buffer *buffer = &dev->buffers[i];
		struct filter *filter = &s->device_filter;
		buffer->index = i;
		buffer->origin = filter;

		int ret = video_queue_buffer(&s->dev,
					     buffer,
					     s->option.fill_mode);
		if (ret < 0) {
			log_err("Unable to requeue buffer: %s (%d).",
				strerror(errno), errno);
			return -1;
		}
		filter->monitored_fd = s->dev.fd_select;
	}

	return 0;
}

int request_vp8_iframe(struct device *dev)
{
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control ctrl[2];

	memset(&ctrls, 0, sizeof(ctrls));
	memset(&ctrl, 0, sizeof(ctrl));

	ctrls.ctrl_class = V4L2_CID_CAMERA_CLASS;
	ctrls.count = 2;
	ctrls.reserved[0] = 7;	// all layers
	ctrls.controls = ctrl;

	ctrl[0].id = V4L2_CID_ENCODER_VP8_SYNC_FRAME_TYPE;
	ctrl[0].value = 1;

	ctrl[1].id = V4L2_CID_ENCODER_SYNC_FRAME_INTERVAL;
	ctrl[1].value = 0;

	int ret = set_controls(dev, &ctrls);
	return ret;
}

static void check_v4l2_buffer(struct device *dev, struct buffer *outbuf)
{
	const struct v4l2_buffer *buf = &outbuf->v4l2_buf;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	// determine this is VP8 I-frame
	outbuf->is_vp8_iframe = false;
	if (is_v4l2_vp8_format(outbuf->pix_fmt.pixelformat)) {
		if ((((char *)outbuf->mem)[0] & 0x1) == 0) {
			outbuf->is_vp8_iframe = true;
			if (dev->need_iframe && buf->sequence != 0)
				log_msg(0, "was waiting for I-farme, got now");
			dev->need_iframe = false;
		}

		bool is_iframe_by_flag = buf->flags & V4L2_BUF_FLAG_KEYFRAME;
		if (outbuf->is_vp8_iframe != is_iframe_by_flag) {
			log_msg(0,
				"Warning: I-frame bit inconsistent: %s by flags, %s by first byte",
				is_iframe_by_flag ? "true" : "false",
				outbuf->is_vp8_iframe ? "true" : "false");
		}
	}

	if (!use_remote) {
		if (timespec_to_double(&now) <
		    timeval_to_double(&buf->timestamp)) {
			log_msg(0,
				"Warning: v4l2_buffer.timestamp(%d.%06d) in future",
				(int)buf->timestamp.tv_sec,
				(int)buf->timestamp.tv_usec);
		}
	}

	if (dev->frame_count == 1) {
		if (buf->sequence != 0)
			log_msg(0,
				"Warning: v4l2_buffer.sequence != 0 for first frame");
		if (!outbuf->is_vp8_iframe
		    && is_v4l2_vp8_format(dev->src_fmt.fmt.pix.pixelformat)) {
			log_msg(0,
				"first frame is not an I-frame, request one");
			request_vp8_iframe(dev);
			dev->need_iframe = true;
			dev->iframe_request_time = buf->sequence;
		}
	} else {
		if (buf->sequence <= dev->last_v4l2_buffer.sequence)
			log_msg(0,
				"Warning: v4l2_buffer.sequence(%u->%u) is not increasing",
				dev->last_v4l2_buffer.sequence, buf->sequence);
		if (buf->sequence > dev->last_v4l2_buffer.sequence + 1) {
			log_msg(0,
				"Warning: frame skip: v4l2_buffer.sequence(%u->%u) jump",
				dev->last_v4l2_buffer.sequence, buf->sequence);

			if (is_v4l2_vp8_format(dev->src_fmt.fmt.pix.pixelformat)
			    && !outbuf->is_vp8_iframe) {
				log_msg(0, "request an I-frame");
				request_vp8_iframe(dev);
				dev->need_iframe = true;
				dev->iframe_request_time = buf->sequence;
			}
		}
		if (timeval_to_double(&buf->timestamp) <
		    timeval_to_double(&dev->last_v4l2_buffer.timestamp))
			log_msg(0,
				"Warning: v4l2_buffer.timestamp is not increasing (%d.%06d->%d.%06d)",
				(int)dev->last_v4l2_buffer.timestamp.tv_sec,
				(int)dev->last_v4l2_buffer.timestamp.tv_usec,
				(int)buf->timestamp.tv_sec,
				(int)buf->timestamp.tv_usec);

	}
	if (is_v4l2_vp8_format(dev->src_fmt.fmt.pix.pixelformat) &&
	    !outbuf->is_vp8_iframe &&
	    dev->need_iframe &&
	    dev->iframe_request_time + dev->nbufs * 1.5 < buf->sequence) {
		log_msg(0, "no I-frame appears, request again");
		request_vp8_iframe(dev);
		dev->iframe_request_time = buf->sequence;
	}
	dev->last_v4l2_buffer = *buf;

	if (is_v4l2_vp8_format(outbuf->pix_fmt.pixelformat)) {
		snprintf(outbuf->analyze_msg, sizeof(outbuf->analyze_msg),
			 "(%c%c%c%c)(%d)",
			 buf->flags & V4L2_BUF_FLAG_KEYFRAME ? 'I' : ' ',
			 buf->flags & V4L2_BUF_FLAG_PREV_FRAME ? 'p' : ' ',
			 buf->flags & V4L2_BUF_FLAG_GOLDEN_FRAME ? 'g' : ' ',
			 buf->flags & V4L2_BUF_FLAG_ALTREF_FRAME ? 'a' : ' ',
			 (buf->
			  flags >> V4L2_BUF_FLAG_LAYER_STRUCTURE_SHIFT) &
			 V4L2_BUF_FLAG_LAYER_STRUCTURE_MASK);
	} else {
		snprintf(outbuf->analyze_msg, sizeof(outbuf->analyze_msg),
			 "(%c%c%c)",
			 buf->flags & V4L2_BUF_FLAG_KEYFRAME ? 'I' : ' ',
			 buf->flags & V4L2_BUF_FLAG_PFRAME ? 'P' : ' ',
			 buf->flags & V4L2_BUF_FLAG_BFRAME ? 'B' : ' ');
	}
}

struct buffer *device_get_frame_buffer(struct context *ctx, struct stream *s)
{
	struct v4l2_buffer buf;
	int ret = video_dequeue_buffer(&s->dev, &buf);
	if (ret < 0)
		return NULL;

	struct buffer *buffer = &s->dev.buffers[buf.index];
	buffer->v4l2_buf = buf;
	buffer->pix_fmt = s->dev.src_fmt.fmt.pix;
	buffer->bytesused = buf.bytesused;
	buffer->serial = s->dev.frame_count++;
	if (use_remote) {
		remote_readmem(buffer->remote_mem, buffer->mem,
			       buffer->bytesused);
	}

	check_v4l2_buffer(&s->dev, buffer);

	/* Skip decode & output if no I-frame */
	/* The buffer will still be sent to next filters because
	 * there are some statistics calculation there. */
	bool skip = ctx->global_options.skip_before_iframe
	    && s->dev.need_iframe;
	buffer->skip_decode = skip;
	buffer->skip_output = skip;

	if (s->load_fp) {
		/* TODO created a file source filter instead of overwritting */
		if (strcmp(s->option.load_format, "ivf") == 0) {
			struct vp8_ivf_frame_hdr frame_hdr;
			if (fread(&frame_hdr, sizeof frame_hdr, 1, s->load_fp)
			    != 1) {
				log_err("read vp8_ivf_frame_hdr failed");
				return NULL;
			}

			/* assume little-endianness */
			unsigned int size = frame_hdr.size;
			if (size > buffer->size) {
				log_err("frame size %u too big (%u)", size,
					buffer->size);
				return NULL;
			}
			if (fread(buffer->mem, size, 1, s->load_fp) != 1) {
				log_err("read frame data failed");
				return NULL;
			}
			buf.bytesused = size;
			buf.flags &= ~V4L2_BUF_FLAG_TIMECODE;
			buf.timestamp.tv_sec = frame_hdr.timestamp_h;
			buf.timestamp.tv_usec = frame_hdr.timestamp_l;
		} else {
			unsigned int size = s->dev.src_fmt.fmt.pix.sizeimage;
			if (fread(buffer->mem, size, 1, s->load_fp) != 1) {
				log_err("read frame data failed");
				return NULL;
			}
			buf.bytesused = size;
			buf.timestamp.tv_sec = 0;
			buf.timestamp.tv_usec = 0;
		}
	}

	if (s->dev.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    buffer->pix_fmt.bytesperline != 0 &&
	    buf.bytesused != buffer->pix_fmt.sizeimage)
		log_msg(0, "Warning: bytes used %u != image size %u",
			buf.bytesused, buffer->pix_fmt.sizeimage);

	return buffer;
}

/* ------------------------------------------------------------------
 * Filter callbacks
 */
static int device_filter_init(struct filter *filter)
{
	struct stream *s = filter->stream;

	if (!s->option.no_query || s->option.do_capture) {
		struct v4l2_fract framerate;
		video_get_format(&s->dev);
		video_get_framerate(&s->dev, &framerate);
	}

	if (video_prepare_capture(s)) {
		return -1;
	}

	return 0;
}

static int device_filter_finalize(struct filter *filter)
{
	struct stream *s = filter->stream;

	/* Stop streaming. */
	if (s->dev.state == DEVICE_STATE_CAPTURE) {
		video_enable(&s->dev, 0);
		s->dev.state = DEVICE_STATE_STOP;
	}

	video_free_buffers(&s->dev);
	if (s->load_fp)
		fclose(s->load_fp);
	return 0;
}

static int device_filter_start_handler(struct filter *filter)
{
	struct stream *s = filter->stream;
	snprintf(thread_tag, sizeof(thread_tag), "%s", s->dev.devname);
	int ret = video_enable(&s->dev, 1);
	if (ret < 0) {
		log_err("video_enable failed ret=%d", ret);
		return ret;
	}
	s->dev.state = DEVICE_STATE_CAPTURE;
	return 0;
}

static int device_filter_buffer_ready_handler(struct filter *filter)
{
	struct buffer *buffer = filter_get_outbuffer(filter);
	struct stream *s = filter->stream;

	// TODO async delay
	if (s->option.delay) {
		usleep(s->option.delay * 1000);
	}

	int ret = video_queue_buffer(&s->dev,
				     buffer,
				     s->option.fill_mode);
	if (ret < 0) {
		log_err("Unable to requeue buffer: %s (%d).",
			strerror(errno), errno);
		return -1;
	}
	/* Buffers enqueued, let's start monitor when captured data are ready. */
	if (s->dev.nbufs_queued > 0)
		filter->monitored_fd = s->dev.fd_select;
	return 0;
}

static int device_filter_cmd_handler(struct filter *filter, enum filter_cmd cmd)
{
	struct stream *s = filter->stream;
	switch (cmd) {
	case CMD_DEVICE_CAPTURE_ON:
		if (s->dev.state == DEVICE_STATE_STOP) {
			int ret = video_enable(&s->dev, 1);
			if (ret < 0) {
				log_err("video_enable(1) failed");
				chan_send_reply(&filter->chan, CMD_DONE);
				return ret;
			}
			s->dev.state = DEVICE_STATE_CAPTURE;

			unsigned int i;
			for (i = 0; i < s->dev.nbufs; ++i) {
				struct buffer *buffer = &s->dev.buffers[i];
				buffer->index = i;
				buffer->origin = filter;
				filter_release_inbuffer(filter, buffer);
			}
		}
		chan_send_reply(&filter->chan, CMD_DONE);
		break;
	case CMD_DEVICE_CAPTURE_OFF:
		if (s->dev.state == DEVICE_STATE_CAPTURE) {
			int ret = video_enable(&s->dev, 0);
			if (ret < 0) {
				log_err("video_enable(0) failed");
				chan_send_reply(&filter->chan, CMD_DONE);
				return ret;
			}
			s->dev.state = DEVICE_STATE_STOP;

			/* pause next filter */
			filter_send_cmd(filter->sink, CMD_PING);
			int reply;
			filter_get_reply(filter->sink, &reply);

			/* reset buffers, but don't release */
			pthread_mutex_lock(&filter->sink->buf_mutex);
			pthread_mutex_lock(&filter->buf_mutex);
			TAILQ_INIT(&filter->freelist);
			TAILQ_INIT(&filter->sink->inbufs);
			filter->freelist_len = 0;
			filter->sink->inbufs_len = 0;
			filter->stream->dev.nbufs_queued = 0;
			filter->monitored_fd = -1;
			close(filter->pipe_buffer_ready[0]);
			close(filter->pipe_buffer_ready[1]);
			ret = pipe(filter->pipe_buffer_ready);

			pthread_mutex_unlock(&filter->buf_mutex);
			pthread_mutex_unlock(&filter->sink->buf_mutex);
			if (ret < 0) {
				log_err("pipe() failed");
				chan_send_reply(&filter->chan, CMD_DONE);
				return ret;
			}

			/* resume next filter */
			filter_send_cmd(filter->sink, CMD_DONE);
		}
		chan_send_reply(&filter->chan, CMD_DONE);
		break;
	case CMD_DEVICE_CHANGE_FORMAT:
		{
			int ret;
			int width;
			int height;
			int format;

			ret = chan_get_cmd(&filter->chan, &width);
			if (ret >= 0)
				ret = chan_get_cmd(&filter->chan, &height);
			if (ret >= 0)
				ret = chan_get_cmd(&filter->chan, &format);
			if (ret < 0) {
				log_err("failed to get args");
				filter_send_reply(filter, -1);
				return -1;
			}
			log_msg(0, "got CMD_DEVICE_CHANGE_FORMAT(%d,%d,0x%x)",
				width, height, format);

			// if changed to bigger size, we have to re-allocate
			// buffers
			bool should_change_buffer = false;
			if (format != (int)s->dev.src_fmt.fmt.pix.pixelformat ||
			    (width * height >
			     s->dev.buffers[0].big_enough_for)) {
				log_msg(0,
					"format change or size become bigger, should change buffer");
				should_change_buffer = true;
			}

			if (should_change_buffer) {
				if (s->dev.state == DEVICE_STATE_CAPTURE) {
					log_msg(0,
						"should stop streaming first");
					filter_send_reply(filter, 2);
					return 0;
				}

				/* pause next filter */
				filter_send_cmd(filter->sink, CMD_PING);
				int reply;
				filter_get_reply(filter->sink, &reply);

				/* reset and release buffers */
				pthread_mutex_lock(&filter->sink->buf_mutex);
				pthread_mutex_lock(&filter->buf_mutex);
				video_free_buffers(&s->dev);
				TAILQ_INIT(&filter->freelist);
				TAILQ_INIT(&filter->sink->inbufs);
				filter->freelist_len = 0;
				filter->sink->inbufs_len = 0;
				filter->stream->dev.nbufs_queued = 0;
				filter->monitored_fd = -1;
				close(filter->pipe_buffer_ready[0]);
				close(filter->pipe_buffer_ready[1]);
				ret = pipe(filter->pipe_buffer_ready);

				pthread_mutex_unlock(&filter->buf_mutex);
				pthread_mutex_unlock(&filter->sink->buf_mutex);
				if (ret < 0) {
					log_err("pipe() failed");
					chan_send_reply(&filter->chan,
							CMD_DONE);
					return ret;
				}

				/* resume next filter */
				filter_send_cmd(filter->sink, CMD_DONE);
			}
			// TODO error handle
			ret =
			    video_set_format(&s->dev, width, height, format, 0);
			video_get_format(&s->dev);

			if (should_change_buffer) {
				video_prepare_buffer(s);
			}
			filter_send_reply(filter, 0);
		}
		break;
	case CMD_DONE:
		// do nothing
		break;
	default:
		log_err("unknown command %d for %s", cmd, filter->cf->name);
	}
	return 0;
}

static int device_filter_before_select_monitored_fd(struct filter *filter)
{
	struct device *dev = &filter->stream->dev;
	if (use_remote) {
		/* %255+1 just in order to prevent zero due to frame_count overflow */
		int token = dev->frame_count % 255 + 1;

		if (token != dev->waiting) {
			dev->waiting = token;
			int ret =
			    remote_select_fd(dev->fd_select, dev->fd, token);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

static int device_filter_monitored_fd_handler(struct filter *filter)
{
	struct device *dev = &filter->stream->dev;
	if (use_remote) {
		int token;
		int ret = readlen(filter->monitored_fd, sizeof(token), &token);
		if (ret < 0)
			return ret;
		if (token != dev->waiting) {
			log_msg(1, "got old token due to racing (%d,%d)", token,
				dev->waiting);
			return 0;
		}
		remote_select_fd(dev->fd_select, dev->fd, 0);
		dev->waiting = 0;
	}

	struct buffer *buffer =
	    device_get_frame_buffer(filter->ctx, filter->stream);
	if (!buffer)
		return -1;
	/* All buffers are dequeued from device. Stop select() on device. */
	if (dev->nbufs_queued == 0)
		filter->monitored_fd = -1;
	filter_deliver_outbuffer(filter, buffer);
	return 0;
}

struct filter_conf device_filter_cf = {
	"device_filter",
	REQUIRE_BUFFER_ANY,
	device_filter_init,
	device_filter_finalize,
	device_filter_start_handler,
	device_filter_buffer_ready_handler,
	device_filter_cmd_handler,
	device_filter_monitored_fd_handler,
	device_filter_before_select_monitored_fd,
};
