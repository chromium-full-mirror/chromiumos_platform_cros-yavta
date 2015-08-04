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
#include <event.h>

#include "yavta.h"

void evbuffer_add_v4l2_streamparm(struct evbuffer *buffer,
				  struct v4l2_streamparm *parm)
{
	switch (parm->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		evbuffer_add_printf(buffer,
				    "{\"type\":%d, \"capability\":%d, \"capturemode\":%d, \"timeperframe\":[%d,%d],"
				    " \"extendedmode\":%d, \"readbuffers\":%d}",
				    parm->type,
				    parm->parm.capture.capability,
				    parm->parm.capture.capturemode,
				    parm->parm.capture.timeperframe.numerator,
				    parm->parm.capture.timeperframe.denominator,
				    parm->parm.capture.extendedmode,
				    parm->parm.capture.readbuffers);
		break;
	default:
		evbuffer_add_printf(buffer, "{type:%d}", parm->type);
	}
}

void evbuffer_add_v4l2_input(struct evbuffer *buffer, struct v4l2_input *input)
{
	evbuffer_add_printf(buffer, "{\"index\":%d, \"name\":\"%s\"}",
			    input->index, input->name);
}

void evbuffer_add_v4l2_queryctrl(struct evbuffer *buffer,
				 struct v4l2_queryctrl *query, int64_t *value,
				 struct evbuffer *menu_buffer)
{
	evbuffer_add_printf(buffer,
			    "{\"id\":%d, \"cls\":%lu, \"type\":%u, \"name\":\"%s\", \"minimum\":%d, \"maximum\":%d,"
			    " \"step\":%d, \"default_value\":%d, \"flags\":%d",
			    query->id, V4L2_CTRL_ID2CLASS(query->id),
			    query->type, query->name, query->minimum,
			    query->maximum, query->step, query->default_value,
			    query->flags);
	if (value) {
		evbuffer_add_printf(buffer, ", \"value\":%" PRId64, *value);
	}
	if (menu_buffer) {
		evbuffer_add_printf(buffer, ", \"menu\":");
		evbuffer_add_buffer(buffer, menu_buffer);
	}
	evbuffer_add_printf(buffer, "}");
}

void evbuffer_add_v4l2_querymenu(struct evbuffer *buffer,
				 struct v4l2_querymenu *menu, bool is_menu)
{
	if (is_menu) {
		evbuffer_add_printf(buffer,
				    "{\"id\":%d, \"index\":%d, \"name\":\"%s\"}",
				    menu->id, menu->index, menu->name);
	} else {
		evbuffer_add_printf(buffer,
				    "{\"id\":%d, \"index\":%d, \"value\":%"
				    PRId64 "}", menu->id, menu->index,
				    (int64_t) menu->value);
	}
}

void evbuffer_add_v4l2_format(struct evbuffer *buffer,
			      struct v4l2_format *format)
{
	evbuffer_add_printf(buffer,
			    "{\"format\":%d, \"format_name\":\"%s\", \"width\":%d, \"height\":%d}",
			    format->fmt.pix.pixelformat,
			    v4l2_format_name(format->fmt.pix.pixelformat),
			    format->fmt.pix.width, format->fmt.pix.height);
}

void evbuffer_add_v4l2_fmtdesc(struct evbuffer *buffer,
			       struct v4l2_fmtdesc *fmtdesc)
{
	// TODO(kcwu): js escape
	evbuffer_add_printf(buffer,
			    "{\"format\":%d, \"format_name\":\"%s\", \"type\":%d, \"description\":\"%s\"}",
			    fmtdesc->pixelformat,
			    v4l2_format_name(fmtdesc->pixelformat),
			    fmtdesc->type, fmtdesc->description);
}

void evbuffer_add_v4l2_frmivalenum(struct evbuffer *buffer,
				   struct v4l2_frmivalenum *ival)
{
	switch (ival->type) {
	case V4L2_FRMIVAL_TYPE_DISCRETE:
		evbuffer_add_printf(buffer,
				    "{\"type\":\"discrete\", \"min\":[%d,%d], \"max\":[%d,%d]}",
				    ival->discrete.numerator,
				    ival->discrete.denominator,
				    ival->discrete.numerator,
				    ival->discrete.denominator);
		break;
	case V4L2_FRMIVAL_TYPE_CONTINUOUS:
		evbuffer_add_printf(buffer,
				    "{\"type\":\"continuous\", \"min\":[%d,%d], \"max\":[%d,%d]}",
				    ival->stepwise.min.numerator,
				    ival->stepwise.min.denominator,
				    ival->stepwise.max.numerator,
				    ival->stepwise.max.denominator);
		break;
	case V4L2_FRMIVAL_TYPE_STEPWISE:
		evbuffer_add_printf(buffer,
				    "{\"type\":\"stepwise\", \"min\":[%d,%d], \"max\":[%d,%d], \"step\":[%d,%d]}",
				    ival->stepwise.min.numerator,
				    ival->stepwise.min.denominator,
				    ival->stepwise.max.numerator,
				    ival->stepwise.max.denominator,
				    ival->stepwise.step.numerator,
				    ival->stepwise.step.denominator);
		break;
	}
}

void evbuffer_add_v4l2_frmsizeenum(struct evbuffer *buffer,
				   struct v4l2_frmsizeenum *frame)
{
	switch (frame->type) {
	case V4L2_FRMSIZE_TYPE_DISCRETE:
		evbuffer_add_printf(buffer,
				    "{\"type\":\"discrete\", \"min\":[%u,%u], \"max\":[%u,%u]}",
				    frame->discrete.width,
				    frame->discrete.height,
				    frame->discrete.width,
				    frame->discrete.height);
		break;

	case V4L2_FRMSIZE_TYPE_CONTINUOUS:
		evbuffer_add_printf(buffer,
				    "{\"type\":\"continuous\", \"min\":[%u,%u], \"max\":[%u,%u]}",
				    frame->stepwise.min_width,
				    frame->stepwise.min_height,
				    frame->stepwise.max_width,
				    frame->stepwise.max_height);
		break;

	case V4L2_FRMSIZE_TYPE_STEPWISE:
		evbuffer_add_printf(buffer,
				    "{\"type\":\"stepwise\", \"min\":[%u,%u], \"max\":[%u,%u], \"step\":[%u,%u]}",
				    frame->stepwise.min_width,
				    frame->stepwise.min_height,
				    frame->stepwise.max_width,
				    frame->stepwise.max_height,
				    frame->stepwise.step_width,
				    frame->stepwise.step_height);
		break;
	}
}
