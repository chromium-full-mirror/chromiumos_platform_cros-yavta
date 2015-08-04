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

#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>

#include "yavta.h"
#include "remote.h"

int yavta_init(struct context *ctx, int argc, char *argv[])
{
	struct stream *s;

	/* omit SIGPIPE when socket streaming disconnected */
	signal(SIGPIPE, SIG_IGN);

	memset(ctx, 0, sizeof(*ctx));
	ctx->x11.fd = -1;
	TAILQ_INIT(&ctx->streams);
	int ret = pipe(ctx->pipe_stop);
	if (ret < 0) {
		log_err("pipe failed");
		return ret;
	}

	if (parse_options(argc, argv, ctx, &ctx->global_options))
		return -1;

	TAILQ_FOREACH(s, &ctx->streams, entries) {
		video_init(&s->dev);
	}

	if (ctx->global_options.http_port) {
		if (webui_init(ctx) < 0)
			return -1;
	}

	/* Open the video device. If the device type isn't recognized, set the
	 * --no-query option to avoid querying V4L2 subdevs.
	 */
	TAILQ_FOREACH(s, &ctx->streams, entries) {

		if (video_open(&s->dev, s->option.devname, s->option.no_query) < 0)
			return -1;

		if (s->dev.type == (enum v4l2_buf_type)-1) {
			log_msg(0, "%s device type isn't recognized",
				s->option.devname);
			s->option.no_query = 1;
		}

		s->dev.memtype = s->option.memtype;
	}

	if (graph_init(&ctx->graph, ctx) < 0)
		return -1;

	return 0;
}

int yavta_operation_at_start(struct context *ctx)
{
	/* TODO: rewrite as filter command */
	int ret;
	struct stream *s;
	TAILQ_FOREACH(s, &ctx->streams, entries) {

		if (s->option.do_set_input) {
			video_set_input(&s->dev, s->option.input);
			ret = video_get_input(&s->dev);
			log_msg(0, "Input %d selected", ret);
		}

		/* Set the video format. */
		if (s->option.do_set_format) {
			if (video_set_format(&s->dev, s->option.width,
					     s->option.height,
					     s->option.pixelformat,
					     s->option.stride) < 0) {
				return -1;
			}
		}

		/* Set the frame rate. */
		if (s->option.do_set_time_per_frame) {
			if (video_set_framerate(&s->dev,
						&s->option.time_per_frame) < 0) {
				return -1;
			}
		}

		if (s->option.do_get_control) {
			int64_t val;
			ret = get_control(&s->dev, s->option.ctrl_name,
					  get_control_type(&s->dev,
							   s->option.ctrl_name),
					  s->option.ctrl_layer, &val);
			if (ret >= 0)
				log_msg(0, "Control 0x%08x value %" PRId64,
					s->option.ctrl_name, val);
		}

		if (s->option.do_set_control)
			set_control(&s->dev, s->option.ctrl_name,
				    get_control_type(&s->dev,
						     s->option.ctrl_name),
				    s->option.ctrl_layer, s->option.ctrl_value);

		if (s->option.do_list_controls)
			video_list_controls(&s->dev, s->option.ctrl_layer);

		if (s->option.ctrls_op != SET_NONE)
			video_set_all_controls(&s->dev, s->option.ctrls_op,
					       s->option.ctrl_layer);

		if (s->option.do_enum_formats) {
			log_msg(0, "- Available formats:");
			video_enum_formats(&s->dev,
					   V4L2_BUF_TYPE_VIDEO_CAPTURE);
			video_enum_formats(&s->dev, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		}

		if (s->option.do_enum_inputs) {
			log_msg(0, "- Available inputs:");
			video_enum_inputs(&s->dev);
		}

	}
	return 0;
}

int yavta_prepare_capture(struct context *ctx)
{
	struct stream *s;

	TAILQ_FOREACH(s, &ctx->streams, entries) {
		if (!s->option.do_capture)
			continue;

		if (filter_init(&s->device_filter, &device_filter_cf, ctx, s) < 0)
			return -1;
		graph_connect(&s->device_filter, &s->process_filter);
	}

	/* build filter graph */
	// this must init after device in order to know initial size
	if (filter_init(&ctx->display_filter, &display_filter_cf, ctx, NULL) < 0)
		return -1;

	// this must init after x11 initialized
	TAILQ_FOREACH(s, &ctx->streams, entries) {
		if (!s->option.do_capture)
			continue;
		if (filter_init(&s->process_filter, &process_filter_cf, ctx, s) < 0)
			return -1;
		graph_connect(&s->process_filter, &ctx->display_filter);
	}

	if (ctx->global_options.do_rt) {
		struct sched_param sched;
		memset(&sched, 0, sizeof sched);
		sched.sched_priority = ctx->global_options.rt_priority;
		int ret = sched_setscheduler(0, SCHED_RR, &sched);
		if (ret < 0)
			log_err("Failed to select RR scheduler: %s (%d)",
				strerror(errno), errno);
	}

	if (ctx->global_options.do_pause) {
		log_msg(0, "Press enter to start capture");
		getchar();
	}

	return 0;
}

void yavta_finalize(struct context *ctx)
{
	int i;
	struct stream *s;
	for (i = 0; i < 2; i++)
		close(ctx->pipe_stop[i]);
	graph_finalize(&ctx->graph);

	TAILQ_FOREACH(s, &ctx->streams, entries) {
		video_close(&s->dev);
		free(s->option.devname);
	}
	while (!TAILQ_EMPTY(&ctx->streams)) {
		s = TAILQ_FIRST(&ctx->streams);
		TAILQ_REMOVE(&ctx->streams, s, entries);
		free(s);
	}
	webui_close(ctx);
}

void program_terminate(struct context *ctx, char token)
{
	int ret = write(ctx->pipe_stop[1], &token, sizeof(token));
	if (ret != sizeof(token))
		log_err("write failed: %s (%d)", strerror(errno), errno);
}

int main(int argc, char *argv[])
{
	struct context ctx;
	int ret = 0;

	if (yavta_init(&ctx, argc, argv))
		goto fail;

	chan_send_cmd(&ctx.graph.chan, CMD_LOCK);

	if (yavta_operation_at_start(&ctx))
		goto fail;

	if (ctx.global_options.do_benchmark) {
		ret = benchmark_remote_overhead();
		if (ret < 0)
			goto fail;
		goto end;
	}

	if (ctx.global_options.do_capture_any) {
		while (ctx.global_options.do_sleep_forever)
			sleep(1000);

		if (yavta_prepare_capture(&ctx) < 0)
			goto fail;

		ret = graph_run(&ctx.graph);
		if (ret < 0)
			goto fail;
	} else {
		if (!ctx.global_options.http_port)
			program_terminate(&ctx, 0);
	}

	chan_send_cmd(&ctx.graph.chan, CMD_UNLOCK);

	char token;
	/* wait until somebody want the graph stop */
	if (read(ctx.pipe_stop[0], &token, sizeof(token)) < 0)
		goto fail;
	log_msg(0, "main got token %d", token);
	if (token)
		ret = 1;

	goto end;
 fail:
	ret = 1;
 end:
	yavta_finalize(&ctx);
	return ret;
}
