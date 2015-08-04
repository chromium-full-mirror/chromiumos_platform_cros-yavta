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
/* Web UI
 * Commands: (note, evhttp doesn't support PUT/DELETE)
 *   - list opened devies	GET /device
 *   - list devies		GET /device/list
 *   - open device		POST /device/open?devname=video0
 *   - close device		POST /device/0/close
 *   - capture			POST /device/0/capture?on=1
 *   - get device attr		GET /device/0/ATTR (format,framerate,etc.)
 *   - set device attr		POST /device/0/ATTR (format,framerate,etc.)
 *   - get control list		GET /device/0/control
 *   - get control attr 	GET /device/0/control/foo
 *   - set control attr 	POST /device/0/control/foo
 *   - streaming WebM video	GET /webm/0
 *   - display position		POST /display/0/position?x=0&y=0
 *
 * Use libevent for http server.
 */
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "third_party/libvpx/webm.h"
#include "yavta.h"

TAILQ_HEAD(frame_list, frame_to_deliver);
struct frame_to_deliver {
	struct stream *stream;
	bool is_vp8_iframe;
	int size;
	char *mem;

	TAILQ_ENTRY(frame_to_deliver) entries;
};

TAILQ_HEAD(webui_client_list, webui_client);
struct webui_client {
	struct context *ctx;
	struct evhttp_request *req;
	struct stream *stream;
	bool wait_iframe;

	TAILQ_ENTRY(webui_client) entries;
};

struct webui {
	pthread_t thread;
	struct event_base *evbase;
	struct evhttp *evhttp;
	int terminate_pipe[2];
	int frame_ready_pipe[2];
	pthread_mutex_t frame_mutex;
	struct frame_list to_deliver;

	struct webui_client_list client_list;
};

static void webui_close_handler(int fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	event_base_loopbreak((struct event_base *)arg);
}

static void *webui_worker(void *evbase)
{
	event_base_dispatch((struct event_base *)evbase);
	return NULL;
}

/*
 * @param always_decode_plus: when true we transform plus to space even
 *     if we have not seen a ?.
 * code copied from libevent's http.c
 */
static int decode_uri(const char *uri, char *ret, int always_decode_plus)
{
	char c;
	int i, j, in_query = always_decode_plus;

	for (i = j = 0; uri[i] != '\0'; i++) {
		c = uri[i];
		if (c == '?') {
			in_query = 1;
		} else if (c == '+' && in_query) {
			c = ' ';
		} else if (c == '%' && isxdigit((unsigned char)uri[i + 1]) &&
			   isxdigit((unsigned char)uri[i + 2])) {
			char tmp[] = { uri[i + 1], uri[i + 2], '\0' };
			c = (char)strtol(tmp, NULL, 16);
			i += 2;
		}
		ret[j++] = c;
	}
	ret[j] = '\0';

	return (j);
}

/* parse body of form POST */
static void parse_post_form_body(struct evhttp_request *req,
				 struct evkeyvalq *formdata)
{
	/* most code are modified from libevent's http.c */
	size_t size = evbuffer_get_length(evhttp_request_get_input_buffer(req));
	if (size > 4096)
		return;		// ignore request insane long
	char *line = (char *)malloc(size + 1);
	evbuffer_remove(evhttp_request_get_input_buffer(req), line, size);
	line[size] = '\0';

	char *p = line;

	while (p != NULL && *p != '\0') {
		char *key, *value, *decoded_value;
		char *argument = strsep(&p, "&");

		value = argument;
		key = strsep(&value, "=");
		if (value == NULL)
			goto error;

		if ((decoded_value = malloc(strlen(value) + 1)) == NULL)
			goto error;

		decode_uri(value, decoded_value, 1 /*always_decode_plus */ );
		evhttp_add_header(formdata, key, decoded_value);
		free(decoded_value);
	}

 error:
	free(line);
}

void register_vp8_streaming(struct webui_client *client)
{
	struct context *ctx = client->ctx;
	TAILQ_INSERT_TAIL(&ctx->webui->client_list, client, entries);

	log_msg(0, "one more visitor, request an i-frame for him");
	request_vp8_iframe(&client->stream->dev);
}

void unregister_vp8_streaming(struct webui_client *client)
{
	struct context *ctx = client->ctx;
	struct webui_client *c;
	TAILQ_FOREACH(c, &ctx->webui->client_list, entries) {
		if (c == client) {
			log_msg(0, "unregistered");
			TAILQ_REMOVE(&ctx->webui->client_list, c, entries);
			return;
		}
	}
}

struct webui_client *create_client(struct context *ctx,
				   struct evhttp_request *req,
				   struct stream *stream)
{
	struct webui_client *client =
	    (struct webui_client *)malloc(sizeof(struct webui_client));
	client->ctx = ctx;
	client->req = req;
	client->stream = stream;
	client->wait_iframe = true;
	return client;
}

void webui_client_close(struct webui_client *client)
{
	unregister_vp8_streaming(client);
	free(client);
}

void streaming_frame_ready(struct context *ctx, struct stream *s,
			   struct buffer *buf)
{
	struct frame_to_deliver *f = (struct frame_to_deliver *)
	    malloc(sizeof(struct frame_to_deliver));
	f->stream = s;
	f->is_vp8_iframe = buf->is_vp8_iframe;
	f->size = buf->bytesused;
	f->mem = malloc(f->size);
	memcpy(f->mem, buf->mem, f->size);

	pthread_mutex_lock(&ctx->webui->frame_mutex);
	TAILQ_INSERT_TAIL(&ctx->webui->to_deliver, f, entries);
	char token = 1;
	int ret = write(ctx->webui->frame_ready_pipe[1], &token, sizeof(token));
	if (ret < 0) {
		log_err("write() failed");
	}
	pthread_mutex_unlock(&ctx->webui->frame_mutex);
}

static void webui_frame_ready_handler(int fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct webui *webui = (struct webui *)arg;
	struct webui_client *c;
	struct frame_to_deliver *f;

	char token;
	int ret = read(webui->frame_ready_pipe[0], &token, sizeof(token));
	if (ret < 0) {
		log_err("read failed");
		return;
	}

	pthread_mutex_lock(&webui->frame_mutex);
	f = TAILQ_FIRST(&webui->to_deliver);
	TAILQ_REMOVE(&webui->to_deliver, f, entries);
	pthread_mutex_unlock(&webui->frame_mutex);

	TAILQ_FOREACH(c, &webui->client_list, entries) {
		if (f->stream == c->stream) {
			if (c->wait_iframe && !f->is_vp8_iframe) {
				log_msg(0, "waiting iframe");
				continue;
			}
			c->wait_iframe = false;

			int frame_header_size;
			unsigned char *frame_header =
			    webm_get_block_header(0, (int)f->is_vp8_iframe,
						  f->size, &frame_header_size);
			if (frame_header == NULL) {
				log_msg(0,
					"webm_get_block_header returns NULL");
				evhttp_send_reply_end(c->req);
				webui_client_close(c);
				continue;
			}
			struct evbuffer *evb = evbuffer_new();
			evbuffer_add(evb, frame_header, frame_header_size);
			evbuffer_add(evb, f->mem, f->size);
			log_msg(0, "frame_header_size %d", frame_header_size);
			free(frame_header);
			evhttp_send_reply_chunk(c->req, evb);
			evbuffer_free(evb);
		}
	}

	free(f->mem);
	free(f);
}

static void client_close_handler(struct evhttp_connection *evcon, void *arg)
{
	(void)evcon;
	struct webui_client *client = (struct webui_client *)arg;
	log_msg(0, "streaming client disconnected");
	webui_client_close(client);
}

static void webui_webm_handler(struct context *ctx, struct evhttp_request *req,
			       const char *path, struct evkeyvalq *args)
{
	int sid;
	if (sscanf(path, "/webm/%d", &sid) != 1) {
		evhttp_send_error(req, 404, "Incorrect path");
		return;
	}
	if (!(0 <= sid && sid < ctx->stream_num)) {
		evhttp_send_error(req, 404, "Stream id error");
		return;
	}

	struct stream *s = stream_get_by_id(ctx, sid);

	struct webui_client *client = create_client(ctx, req, s);
	register_vp8_streaming(client);
	evhttp_connection_set_closecb(evhttp_request_get_connection(req),
				      client_close_handler, client);

	evhttp_clear_headers(args);
	evhttp_add_header(evhttp_request_get_output_headers(req),
			  "Content-Type", "video/webm");
	evhttp_send_reply_start(req, HTTP_OK, "OK");

	int header_size;
	unsigned char *header = webm_get_file_header(0,
						     s->dev.src_fmt.fmt.pix.
						     width,
						     s->dev.src_fmt.fmt.pix.
						     height,
						     &header_size);
	if (header == NULL) {
		log_msg(0, "webm_get_file_header return NULL");
		evhttp_send_reply_end(req);
		webui_client_close(client);
		return;
	}
	struct evbuffer *evb = evbuffer_new();
	evbuffer_add(evb, header, header_size);
	log_msg(0, "header_size %d", header_size);
	free(header);
	evhttp_send_reply_chunk(req, evb);
	evbuffer_free(evb);
}

static void webui_device_one_handler(struct context *ctx,
				     struct evhttp_request *req,
				     const char *path, int sid,
				     struct evkeyvalq *args,
				     struct evbuffer *buffer)
{
	if (!(0 <= sid && sid < ctx->stream_num)) {
		evbuffer_add_printf(buffer, "'stream id error'");
		return;
	}

	struct stream *s = stream_get_by_id(ctx, sid);
	int control_id;
	int readlen;
	enum evhttp_cmd_type cmd_type = evhttp_request_get_command(req);

	if (strcmp(path, "/") == 0) {	/* /device/0 */
	} else if (strcmp(path, "/capture") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			evbuffer_add_printf(buffer, "%d", s->dev.state);
		} else if (cmd_type == EVHTTP_REQ_POST) {
			const char *on_str = evhttp_find_header(args, "on");
			if (on_str) {
				int on = atoi(on_str);
				if (on)
					chan_send_cmd(&s->device_filter.chan,
						      CMD_DEVICE_CAPTURE_ON);
				else
					chan_send_cmd(&s->device_filter.chan,
						      CMD_DEVICE_CAPTURE_OFF);
				int reply;
				chan_get_reply(&s->device_filter.chan, &reply);
				chan_send_cmd(&s->device_filter.chan, CMD_DONE);
				// TODO
				evbuffer_add_printf(buffer, "%d", s->dev.state);
			}
		}
	} else if (strcmp(path, "/format") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			video_get_format(&s->dev);
			evbuffer_add_v4l2_format(buffer, &s->dev.src_fmt);
		} else if (cmd_type == EVHTTP_REQ_POST) {
			// TODO dump with format=ivf
			const char *format_str =
			    evhttp_find_header(args, "format");
			const char *width_str =
			    evhttp_find_header(args, "width");
			const char *height_str =
			    evhttp_find_header(args, "height");
			if (width_str && height_str) {
				int width = atoi(width_str);
				int height = atoi(height_str);
				int format = atoi(format_str);
				chan_send_cmd(&s->device_filter.chan,
					      CMD_DEVICE_CHANGE_FORMAT);
				chan_send_cmd(&s->device_filter.chan, width);
				chan_send_cmd(&s->device_filter.chan, height);
				chan_send_cmd(&s->device_filter.chan, format);

				int reply;
				chan_get_reply(&s->device_filter.chan, &reply);
				evbuffer_add_printf(buffer, "%d", reply);
				chan_send_cmd(&s->device_filter.chan, CMD_DONE);
			}
		}
	} else if (strcmp(path, "/streamparm") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			struct v4l2_streamparm parm;
			int ret = video_get_streamparm(&s->dev, &parm);
			if (ret < 0)
				evbuffer_add_printf(buffer,
						    "get_streamparm fail");
			else
				evbuffer_add_v4l2_streamparm(buffer, &parm);
		} else if (cmd_type == EVHTTP_REQ_POST) {
		}
	} else if (strcmp(path, "/formats") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			const char *type_str = evhttp_find_header(args, "type");
			if (type_str) {
				unsigned int i;
				int type = atoi(type_str);
				evbuffer_add_printf(buffer, "[");

				for (i = 0;; i++) {
					struct v4l2_fmtdesc fmt;
					if (video_get_formats_idx(&s->dev, type,
								  i, &fmt) < 0)
						break;
					if (i)
						evbuffer_add_printf(buffer,
								    ",");
					evbuffer_add_v4l2_fmtdesc(buffer, &fmt);
				}
				evbuffer_add_printf(buffer, "]");
			}
		} else if (cmd_type == EVHTTP_REQ_POST) {
		}
	} else if (strcmp(path, "/frame_sizes") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			const char *format_str =
			    evhttp_find_header(args, "format");
			if (format_str) {
				unsigned int i;
				struct v4l2_frmsizeenum frame;
				uint32_t format = atoi(format_str);
				evbuffer_add_printf(buffer, "[");
				for (i = 0;; i++) {
					if (video_get_frame_size_idx(&s->dev,
					      			     format,
								     i,
								     &frame) < 0)
						break;
					if (i)
						evbuffer_add_printf(buffer,
								    ",");
					evbuffer_add_v4l2_frmsizeenum(buffer,
								      &frame);
				}
				evbuffer_add_printf(buffer, "]");
			}
		} else if (cmd_type == EVHTTP_REQ_POST) {
		}
	} else if (strcmp(path, "/frame_intervals") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			const char *pixelformat_str =
			    evhttp_find_header(args, "format");
			const char *width_str =
			    evhttp_find_header(args, "width");
			const char *height_str =
			    evhttp_find_header(args, "height");
			if (pixelformat_str && width_str && height_str) {
				uint32_t pixelformat = atoi(pixelformat_str);
				unsigned int width = atoi(width_str);
				unsigned int height = atoi(height_str);
				struct v4l2_frmivalenum ival;
				unsigned int i;
				evbuffer_add_printf(buffer, "[");

				for (i = 0;; i++) {
					if (video_get_frame_interval_idx(
					      &s->dev, pixelformat, width,
					      height, i, &ival) < 0)
						break;
					if (i)
						evbuffer_add_printf(buffer,
								    ",");
					evbuffer_add_v4l2_frmivalenum(buffer,
								      &ival);
				}

				evbuffer_add_printf(buffer, "]");
			}
		} else if (cmd_type == EVHTTP_REQ_POST) {
		}
	} else if (strcmp(path, "/control") == 0) {
		/* /device/0/control */
		struct v4l2_queryctrl query;
		const char *reserved_0_str =
		    evhttp_find_header(args, "reserved_0");
		uint32_t reserved_0 = reserved_0_str ? atoi(reserved_0_str) : 0;
		memset(&query, 0, sizeof(query));
		evbuffer_add_printf(buffer, "[");
		char *delim = "";
		while (video_control_iter_next(&s->dev, &query)) {
			if (query.flags & V4L2_CTRL_FLAG_DISABLED)
				continue;
			int64_t val64, *val64p = NULL;
			evbuffer_add_printf(buffer, "%s", delim);
			int ret = get_control(&s->dev, query.id, query.type,
					      reserved_0, &val64);
			if (ret >= 0)
				val64p = &val64;
			struct evbuffer *menu_buffer = NULL;
			if (query.type == V4L2_CTRL_TYPE_MENU ||
			    query.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
				menu_buffer = evbuffer_new();
				evbuffer_add_printf(menu_buffer, "[");
				int i;
				const char *delim2 = "";
				for (i = query.minimum; i <= query.maximum; i++) {
					struct v4l2_querymenu menu;
					int ret = video_query_menu_idx(&s->dev,
								       query.id,
								       i,
								       &menu);
					if (ret < 0)
						continue;
					menu.index = i;
					evbuffer_add_printf(menu_buffer, "%s",
							    delim2);
					evbuffer_add_v4l2_querymenu(menu_buffer,
								    &menu,
								    query.type
								    ==
								    V4L2_CTRL_TYPE_MENU);
					delim2 = ",";
				}
				evbuffer_add_printf(menu_buffer, "]");
			}
			evbuffer_add_v4l2_queryctrl(buffer, &query, val64p,
						    menu_buffer);
			delim = ",";
			if (menu_buffer)
				evbuffer_free(menu_buffer);
		}
		evbuffer_add_printf(buffer, "]");
	} else if (sscanf(path, "/control/%n%d", &readlen, &control_id) == 1) {
		/* /device/0/control/NNN */
		if (cmd_type == EVHTTP_REQ_GET) {
			const char *type_str = evhttp_find_header(args, "type");
			int type = type_str ? atoi(type_str) : 0;
			const char *reserved_0_str =
			    evhttp_find_header(args, "reserved_0");
			uint32_t reserved_0 =
			    reserved_0_str ? atoi(reserved_0_str) : 0;
			int64_t val64;
			int ret =
			    get_control(&s->dev, control_id, type, reserved_0,
					&val64);
			if (ret < 0) {
				evbuffer_add_printf(buffer,
						    "{\"error\": \"failed to get value\"}");
			} else {
				evbuffer_add_printf(buffer, "[%" PRId64 "]",
						    val64);
			}
		} else if (cmd_type == EVHTTP_REQ_POST) {
			const char *id_str = path + readlen;
			const char *value_str =
			    evhttp_find_header(args, "value");
			const char *type_str = evhttp_find_header(args, "type");
			const char *reserved_0_str =
			    evhttp_find_header(args, "reserved_0");
			uint32_t reserved_0 =
			    reserved_0_str ? atoi(reserved_0_str) : 0;
			struct v4l2_ext_controls ctrls;
			memset(&ctrls, 0, sizeof(ctrls));

			ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(control_id);
			ctrls.reserved[0] = reserved_0;

			while (id_str && value_str && type_str) {
				int id = atoi(id_str);
				int64_t value = atoll(value_str);
				int type = atoi(type_str);
				struct v4l2_ext_control *ctrl;

				ctrls.count++;
				// assume they all are in the same class.
				ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(id);
				ctrls.controls = realloc(ctrls.controls,
							 ctrls.count *
							 sizeof(*ctrl));
				ctrl = &ctrls.controls[ctrls.count - 1];

				memset(ctrl, 0, sizeof(*ctrl));
				ctrl->id = id;
				if (type == V4L2_CTRL_TYPE_INTEGER64)
					ctrl->value64 = value;
				else
					ctrl->value = value;
				log_msg(1, "set control 0x%x to %" PRId64, id,
					value);

				id_str = strchr(id_str, ',');
				if (id_str)
					id_str++;
				value_str = strchr(value_str, ',');
				if (value_str)
					value_str++;
				type_str = strchr(type_str, ',');
				if (type_str)
					type_str++;
			}
			int ret = set_controls(&s->dev, &ctrls);
			if (ret < 0) {
				evbuffer_add_printf(buffer,
						    "{\"error\": \"failed to set value\"}");
			} else {
				evbuffer_add_printf(buffer, "0");
			}
		}
	} else if (strcmp(path, "/reset_control") == 0) {
		if (cmd_type == EVHTTP_REQ_POST) {
			const char *reserved_0_str =
			    evhttp_find_header(args, "reserved_0");
			uint32_t reserved_0 =
			    reserved_0_str ? atoi(reserved_0_str) : 0;
			video_set_all_controls(&s->dev, SET_DEF, reserved_0);
		}
	} else if (strcmp(path, "/close") == 0) {
		if (cmd_type == EVHTTP_REQ_POST) {
			// TODO
		}
	}
}

static void webui_device_handler(struct context *ctx,
				 struct evhttp_request *req, const char *path,
				 struct evkeyvalq *args,
				 struct evbuffer *buffer)
{
	int readlen;
	int sid;
	if (sscanf(path, "/device/%d%n", &sid, &readlen) == 1) {
		webui_device_one_handler(ctx, req, path + readlen, sid, args,
					 buffer);
		return;
	}

	enum evhttp_cmd_type cmd_type = evhttp_request_get_command(req);
	if (strcmp(path, "/device") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			evbuffer_add_printf(buffer, "[");
			char *delim = "";
			struct stream *s;
			TAILQ_FOREACH(s, &ctx->streams, entries) {
				evbuffer_add_printf(buffer,
						    "%s{\"id\": %d, \"devname\":\"%s\"}",
						    delim, s->id,
						    s->option.devname);
				delim = ", ";
			}
			evbuffer_add_printf(buffer, "]");
		}
	} else if (strcmp(path, "/device/list") == 0) {
		if (cmd_type == EVHTTP_REQ_GET) {
			struct dirent **namelist;
			int n;

			n = scandir("/dev", &namelist, 0, alphasort);
			evbuffer_add_printf(buffer, "[");
			if (n >= 0) {
				char *delim = "";
				int i;
				for (i = 0; i < n; i++) {
					struct dirent *ent = namelist[i];
					if (strncmp("video", ent->d_name, 5) == 0) {
						evbuffer_add_printf(buffer,
								    "%s\"/dev/%s\"",
								    delim,
								    ent->d_name);
						delim = ", ";
					}
					free(namelist[i]);
				}
				free(namelist);
			}
			evbuffer_add_printf(buffer, "]");
		}
	} else if (strcmp(path, "/device/open") == 0) {
		if (cmd_type == EVHTTP_REQ_POST) {
			const char *devname =
			    evhttp_find_header(args, "devname");
			struct stream *stream_new(struct context *ctx);
			bool is_safe_devname(const char *name);
			if (!devname || !is_safe_devname(devname)) {
				evbuffer_add_printf(buffer,
						    "{\"error\": \"devname is bad\"}");
				return;
			}

			int ret;
			struct stream *s = stream_new(ctx);
			s->option.devname = strdup(devname);
			s->option.do_output = true;
			s->option.do_capture = true;
			video_init(&s->dev);
			if (video_open(&s->dev, s->option.devname,
				       s->option.no_query) < 0) {
				evbuffer_add_printf(buffer,
						    "{\"error\": \"video_open failed\"}");
				return;
			}
			// TODO error handling
			// connect graph
			if (filter_init(&s->device_filter, &device_filter_cf,
					ctx, s) < 0)
				return;
			if (filter_init(&s->process_filter, &process_filter_cf,
					ctx, s) < 0)
				return;
			graph_connect(&s->device_filter, &s->process_filter);
			graph_connect(&s->process_filter, &ctx->display_filter);

			// start
			ret = s->device_filter.cf->init(&s->device_filter);
			if (ret < 0) {
				evbuffer_add_printf(buffer,
						    "{\"error\": \"device init failed\"}");
			}
			ret = s->process_filter.cf->init(&s->process_filter);
			if (ret < 0) {
				evbuffer_add_printf(buffer,
						    "{\"error\": \"process init failed\"}");
			}
			void *filter_worker(void *arg);
			pthread_create(&s->device_filter.thread, NULL,
				       filter_worker, &s->device_filter);
			pthread_create(&s->process_filter.thread, NULL,
				       filter_worker, &s->process_filter);

			chan_send_cmd(&ctx->display_filter.chan,
				      CMD_DISPLAY_ADD_STREAM);
			chan_send_cmd(&ctx->display_filter.chan, s->id);
			int reply;
			chan_get_reply(&ctx->display_filter.chan, &reply);

			chan_send_cmd(&ctx->display_filter.chan, CMD_DONE);

			evbuffer_add_printf(buffer, "%d", s->id);
		}
	}
}

static void webui_display_handler(struct context *ctx,
				  struct evhttp_request *req, const char *uri,
				  struct evkeyvalq *args,
				  struct evbuffer *buffer)
{
	(void)ctx;
	(void)req;
	(void)uri;
	(void)args;
	(void)buffer;
}

static bool is_safe_path(const char *path)
{
	return path[0] != '/' && strstr(path, "..") == NULL;
}

static char *method_name(enum evhttp_cmd_type method)
{
	switch (method) {
	case EVHTTP_REQ_GET:
		return "GET";
	case EVHTTP_REQ_POST:
		return "POST";
	case EVHTTP_REQ_HEAD:
		return "HEAD";
	default:
		return "OTHER";
	}
}

static const char *guess_mimetype(const char *uri)
{
	if (strstr(uri, ".js"))
		return "text/javascript; charset=UTF-8";
	if (strstr(uri, ".css"))
		return "text/css; charset=UTF-8";
	if (strstr(uri, ".png"))
		return "image/png";
	if (strstr(uri, ".webm"))
		return "video/webm";

	return "text/html; charset=UTF-8";
}

static void webui_handler(struct evhttp_request *req, void *arg)
{
	struct context *ctx = (struct context *)arg;
	(void)ctx;
	const char *uri = evhttp_request_get_uri(req);
	log_msg(1, "request %s %s",
		method_name(evhttp_request_get_command(req)), uri);

	struct evkeyval *header;
	TAILQ_FOREACH(header, evhttp_request_get_input_headers(req), next) {
		log_msg(0, "header %s: %s", header->key, header->value);
	}

	// extract path
	char *path = strdup(uri);
	if (strchr(path, '?'))
		*strchr(path, '?') = '\0';

	struct evbuffer *buffer = evbuffer_new();

	// url rewrite
	if (strcmp(path, "/") == 0) {
		free(path);
		path = strdup("/webui.html");
	}

	enum evhttp_cmd_type cmd_type = evhttp_request_get_command(req);
	struct evkeyvalq *output_headers =
	    evhttp_request_get_output_headers(req);
	if (strncmp(path, "/device", 7) == 0) {
		struct evkeyvalq args;
		evhttp_parse_query(uri, &args);
		/* form data will override query args */
		if (cmd_type == EVHTTP_REQ_POST)
			parse_post_form_body(req, &args);
		webui_device_handler(ctx, req, path, &args, buffer);
		evhttp_clear_headers(&args);
		evhttp_add_header(output_headers, "Content-Type",
				  "application/json");
		evhttp_send_reply(req, HTTP_OK, "OK", buffer);
	} else if (strncmp(path, "/display", 8) == 0) {
		struct evkeyvalq args;
		evhttp_parse_query(uri, &args);
		if (cmd_type == EVHTTP_REQ_POST)
			parse_post_form_body(req, &args);
		webui_display_handler(ctx, req, path, &args, buffer);
		evhttp_clear_headers(&args);
		evhttp_add_header(output_headers, "Content-Type",
				  "application/json");
		evhttp_send_reply(req, HTTP_OK, "OK", buffer);
	} else if (strncmp(path, "/webm", 5) == 0) {
		struct evkeyvalq args;
		evhttp_parse_query(uri, &args);
		if (cmd_type == EVHTTP_REQ_POST)
			parse_post_form_body(req, &args);
		webui_webm_handler(ctx, req, path, &args);
	} else {
		// static content
		bool ok = false;
		if (is_safe_path(path + 1)) {
			char filepath[PATH_MAX];
			snprintf(filepath, sizeof(filepath), "static/%s",
				 path + 1);
			int fd = open(filepath, O_RDONLY);

			if (fd) {
				const char *mimetype = guess_mimetype(path);
				evhttp_add_header(output_headers,
						  "Content-Type", mimetype);
				struct stat st;
				fstat(fd, &st);
				// evbuffer_add_file() owns the fd
				evbuffer_add_file(buffer, fd, 0, st.st_size);
				evhttp_send_reply(req, HTTP_OK, "OK", buffer);
				ok = true;
			}
		}

		if (!ok) {
			evbuffer_add_printf(buffer, "Not Found\n");
			evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found",
					  buffer);
		}
	}
	free(path);
	if (buffer)
		evbuffer_free(buffer);
}

int webui_init(struct context *ctx)
{
	struct webui *webui;
	ctx->webui = webui = calloc(1, sizeof(struct webui));
	webui->terminate_pipe[0] = -1;
	webui->frame_ready_pipe[0] = -1;
	webui->evbase = event_base_new();

	TAILQ_INIT(&webui->to_deliver);
	pthread_mutex_init(&webui->frame_mutex, NULL);
	/* web handler */
	webui->evhttp = evhttp_new(webui->evbase);
	if (evhttp_bind_socket(webui->evhttp, "*",
			       ctx->global_options.http_port) == -1) {
		log_err("failed to bind socket: %s (%d)", strerror(errno),
			errno);
		return -1;
	}
	evhttp_set_gencb(webui->evhttp, webui_handler, ctx);

	/* close handler */
	if (pipe(webui->terminate_pipe) < 0) {
		perror("pipe");
		return -1;
	}
	if (pipe(webui->frame_ready_pipe) < 0) {
		perror("pipe");
		return -1;
	}
	event_base_once(webui->evbase, webui->terminate_pipe[0], EV_READ,
			webui_close_handler, webui->evbase, NULL);
	TAILQ_INIT(&webui->client_list);

	struct event *ev = event_new(webui->evbase, webui->frame_ready_pipe[0],
				     EV_READ | EV_PERSIST,
				     webui_frame_ready_handler, webui);
	event_add(ev, NULL);

	pthread_create(&webui->thread, NULL, webui_worker, webui->evbase);
	return 0;
}

int webui_close(struct context *ctx)
{
	struct webui *webui = ctx->webui;
	if (!webui)
		return 0;

	if (webui->terminate_pipe[0] >= 0) {
		close(webui->terminate_pipe[1]);
		pthread_join(webui->thread, NULL);
		close(webui->terminate_pipe[0]);
	}
	if (webui->evhttp)
		evhttp_free(webui->evhttp);
	if (webui->evbase)
		event_base_free(webui->evbase);
	free(webui);
	return 0;
}
