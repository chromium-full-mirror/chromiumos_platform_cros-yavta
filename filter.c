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
/*
 * Video frame streaming framework.
 *
 * All processing units are abstracted as "filter".  Video frames are enclosed
 * in "buffer".  Filters can consume or produce buffers.
 *
 * All filters are grouped as "graph". After filters are registered and
 * connected in graph, graph_run() will create threads to run filters.
 *
 * For example, "device filter" acquires video frames from camera, put the
 * frame data in buffer, and deliver buffer to "process filter". "process
 * filter" decode the video frame and pass decoded frames to "display filter".
 * "display filter" gets decoded frame and output to screen.
 *
 * For concrete filters, see video.c, process.c, and display.c for detail.
 */
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>

#include "yavta.h"

int filter_init(struct filter *filter, struct filter_conf *cf,
		struct context *ctx, struct stream *stream)
{
	memset(filter, 0, sizeof(*filter));

	graph_register(&ctx->graph, filter);
	filter->cf = cf;
	filter->ctx = ctx;
	filter->stream = stream;
	filter->monitored_fd = -1;
	pthread_mutex_init(&filter->buf_mutex, NULL);
	TAILQ_INIT(&filter->inbufs);
	TAILQ_INIT(&filter->freelist);

	if (pipe(filter->pipe_buffer_ready) < 0) {
		log_err("pipe failed");
		return -1;
	}
	if (chan_init(&filter->chan) < 0)
		return -1;
	return 0;
}

void filter_uninit(struct filter *filter)
{
	int i;
	chan_close(&filter->chan);
	for (i = 0; i < 2; i++) {
		close(filter->pipe_buffer_ready[i]);
	}
	pthread_mutex_destroy(&filter->buf_mutex);
}

static void filter_notify_buffer_ready(struct filter *filter)
{
	log_msg(3, "%s filter_notify_buffer_ready", filter->cf->name);
	char token = 0;
	int ret = write(filter->pipe_buffer_ready[1], &token, sizeof(token));
	if (ret != sizeof(token))
		log_err("write failed: %s (%d)", strerror(errno), errno);
}

/* ------------------------------------------------------------------
 * filter command
 * - Two threads are involved. One is command invoker (client) and the other
 *   is the receiving filter (handler).
 * - The whole communication is locked using mutex. Other clients won't
 *   interrupt while one client is active.
 * - Once handler got one command, it will wait for more commands from the
 *   same client until it gets CMD_DONE.
 */

int filter_get_cmd(struct filter *filter, enum filter_cmd *cmd)
{
	assert(filter->thread == pthread_self());
	assert(sizeof(int) == sizeof(*cmd));
	int ret = chan_get_cmd(&filter->chan, (int *)cmd);
	log_msg(1, "%s got cmd %d", filter->cf->name, *cmd);
	return ret;
}

int filter_send_cmd(struct filter *filter, int arg)
{
	assert(filter->thread != pthread_self());
	return chan_send_cmd(&filter->chan, arg);
}

int filter_send_reply(struct filter *filter, int arg)
{
	assert(filter->thread == pthread_self());
	return chan_send_reply(&filter->chan, arg);
}

int filter_get_reply(struct filter *filter, int *reply)
{
	assert(filter->thread != pthread_self());
	return chan_get_reply(&filter->chan, reply);
}

/* ------------------------------------------------------------------ */
static int filter_wait_event(struct filter *filter, fd_set *readset)
{
	int maxfd = 0;
	FD_ZERO(readset);
	FD_SET(filter->chan.pipe_cmd[0], readset);
	if (filter->chan.pipe_cmd[0] > maxfd)
		maxfd = filter->chan.pipe_cmd[0];
	FD_SET(filter->pipe_buffer_ready[0], readset);
	if (filter->pipe_buffer_ready[0] > maxfd)
		maxfd = filter->pipe_buffer_ready[0];
	if (filter->monitored_fd >= 0) {
		if (filter->cf->before_select_monitored_fd) {
			if (filter->cf->before_select_monitored_fd(filter) < 0)
				return -1;
		}
		FD_SET(filter->monitored_fd, readset);
		if (filter->monitored_fd > maxfd)
			maxfd = filter->monitored_fd;
	}

	while (1) {
		int nfd = select(maxfd + 1, readset, NULL, NULL, NULL);
		if (nfd < 0) {
			if (errno == EINTR)
				continue;
			log_err("select failed: %s (%d)", strerror(errno),
				errno);
		}
		return nfd;
	}
}

/* ------------------------------------------------------------------
 * Filter buffer
 */
/*
   buffer protocol:

   freelist
      | filter_get_outbuffer
      v
   (process)
      | filter_deliver_outbuffer
      v
   next->inbufs
      | filter_get_inbuffer
      v
   (process)
      | filter_release_inbuffer
      v
   freelist

   - free out-buffers are parked in each filter's freelist.
   - processed buffers are delivered to next filter's inbufs.
   - get in-buffers from each filter's inbufs.
   - When in-buffers are done, returns to previous filter's freelist.
 */

struct buffer *filter_get_inbuffer(struct filter *filter)
{
	struct buffer *result;
	log_msg(3, "%s filter_get_inbuffer", filter->cf->name);
	pthread_mutex_lock(&filter->buf_mutex);
	assert(!TAILQ_EMPTY(&filter->inbufs));
	result = TAILQ_FIRST(&filter->inbufs);
	TAILQ_REMOVE(&filter->inbufs, result, entries);
	filter->inbufs_len--;
	pthread_mutex_unlock(&filter->buf_mutex);
	return result;
}

struct buffer *filter_get_outbuffer(struct filter *filter)
{
	struct buffer *result;
	log_msg(3, "%s filter_get_outbuffer", filter->cf->name);
	pthread_mutex_lock(&filter->buf_mutex);
	assert(!TAILQ_EMPTY(&filter->freelist));
	result = TAILQ_FIRST(&filter->freelist);
	TAILQ_REMOVE(&filter->freelist, result, entries);
	filter->freelist_len--;
	pthread_mutex_unlock(&filter->buf_mutex);
	return result;
}

void filter_get_both_buffer(struct filter *filter, struct buffer **inbuf,
			    struct buffer **outbuf)
{
	log_msg(3, "%s filter_get_both_buffer", filter->cf->name);
	pthread_mutex_lock(&filter->buf_mutex);
	assert(!TAILQ_EMPTY(&filter->inbufs));
	assert(!TAILQ_EMPTY(&filter->freelist));
	*inbuf = TAILQ_FIRST(&filter->inbufs);
	*outbuf = TAILQ_FIRST(&filter->freelist);
	TAILQ_REMOVE(&filter->inbufs, *inbuf, entries);
	TAILQ_REMOVE(&filter->freelist, *outbuf, entries);
	filter->inbufs_len--;
	filter->freelist_len--;
	pthread_mutex_unlock(&filter->buf_mutex);
}

void filter_release_inbuffer(struct filter *filter, struct buffer *buffer)
{
	struct filter *origin = buffer->origin;
	assert(origin);
	log_msg(3, "%s filter_release_inbuffer ->%s",
		filter->cf->name, origin->cf->name);
	pthread_mutex_lock(&origin->buf_mutex);
	TAILQ_INSERT_TAIL(&origin->freelist, buffer, entries);
	if (origin->cf->require == REQUIRE_BUFFER_ANY ||
	    (origin->cf->require == REQUIRE_BUFFER_BOTH &&
	     origin->freelist_len < origin->inbufs_len))
		filter_notify_buffer_ready(origin);
	origin->freelist_len++;

	pthread_mutex_unlock(&origin->buf_mutex);
}

void filter_deliver_outbuffer(struct filter *filter, struct buffer *buffer)
{
	struct filter *sink = filter->sink;
	log_msg(3, "%s filter_deliver_outbuffer ->%s",
		filter->cf->name, sink->cf->name);
	pthread_mutex_lock(&sink->buf_mutex);
	TAILQ_INSERT_TAIL(&sink->inbufs, buffer, entries);
	if (sink->cf->require == REQUIRE_BUFFER_ANY ||
	    (sink->cf->require == REQUIRE_BUFFER_BOTH &&
	     sink->freelist_len > sink->inbufs_len))
		filter_notify_buffer_ready(sink);
	sink->inbufs_len++;
	pthread_mutex_unlock(&sink->buf_mutex);

}

bool filter_is_buffer_ready(struct filter *filter)
{
	bool result = false;
	pthread_mutex_lock(&filter->buf_mutex);
	switch (filter->cf->require) {
	case REQUIRE_BUFFER_ANY:
		result = filter->inbufs_len > 0 || filter->freelist_len > 0;
		break;
	case REQUIRE_BUFFER_BOTH:
		result = filter->inbufs_len > 0 && filter->freelist_len > 0;
		break;
	}
	pthread_mutex_unlock(&filter->buf_mutex);
	return result;
}

/* ------------------------------------------------------------------ */
/* The thread worker which drive filters run. It's the main loop of each
 * filter. It waits for async events and call filter's callback functions.
 */
void *filter_worker(void *arg)
{
	int ret = 0;
	struct filter *filter = (struct filter *)arg;
	struct filter_conf *cf = filter->cf;
	bool loop = true;
	log_msg(1, "%s worker start", cf->name);

	if (cf->start_handler)
		ret = cf->start_handler(filter);

	while (loop && ret >= 0) {
		fd_set readset;
		int nfd = filter_wait_event(filter, &readset);
		if (nfd < 0)
			break;

		if (FD_ISSET(filter->pipe_buffer_ready[0], &readset) &&
		    filter_is_buffer_ready(filter)) {
			log_msg(3, "%s buffer_ready", filter->cf->name);
			char token;
			ret =
			    read(filter->pipe_buffer_ready[0], &token,
				 sizeof(token));
			if (ret < 0) {
				log_err("read failed: %s (%d)", strerror(errno),
					errno);
				break;
			}
			assert(cf->buffer_ready_handler);

			ret = cf->buffer_ready_handler(filter);
			if (ret < 0) {
				loop = false;
				break;
			}
		}

		if (FD_ISSET(filter->chan.pipe_cmd[0], &readset)) {
			log_msg(3, "%s cmd", filter->cf->name);
			enum filter_cmd cmd;
			do {
				ret = filter_get_cmd(filter, &cmd);
				if (ret < 0)
					break;
				ret = cf->cmd_handler(filter, cmd);
				if (ret < 0)
					break;
			} while (cmd != CMD_DONE && cmd != CMD_EXIT);
			if (ret < 0 || cmd == CMD_EXIT)
				break;
		}

		if (filter->monitored_fd >= 0
		    && FD_ISSET(filter->monitored_fd, &readset)) {
			log_msg(3, "%s monitored_fd", filter->cf->name);
			assert(cf->monitored_fd_handler);
			ret = cf->monitored_fd_handler(filter);
			if (ret < 0)
				break;
		}
	}
	log_msg(1, "%s stop (%d)", filter->cf->name, ret);
	if (ret < 0)
		program_terminate(filter->ctx, ret);
	return NULL;
}

/* ------------------------------------------------------------------
 * graph
 */
void *graph_worker(void *arg);
int graph_init(struct filter_graph *graph, struct context *ctx)
{
	graph->ctx = ctx;
	TAILQ_INIT(&graph->filters);
	if (chan_init(&graph->chan) < 0)
		return -1;
	pthread_create(&graph->thread, NULL, graph_worker, graph);
	return 0;
}

void graph_finalize(struct filter_graph *graph)
{
	struct filter *filter;
	if (!graph->ctx)
		return;

	graph_terminate(graph);

	TAILQ_FOREACH(filter, &graph->filters, entries) {
		filter->cf->finalize(filter);
		filter_uninit(filter);
	}
}

void graph_connect(struct filter *front, struct filter *back)
{
	front->sink = back;
}

static int graph_cmd_handler(struct filter_graph *graph, enum filter_cmd cmd)
{
	(void)graph;
	(void)cmd;
	switch (cmd) {
	case CMD_GRAPH_ADD_DEVICE:
		break;
	case CMD_GRAPH_DEL_DEVICE:
		break;
	case CMD_GRAPH_CAPTURE:
		break;
	case CMD_GRAPH_STOP:
		break;
	default:
		log_err("unknown command %d for graph", cmd);
	}
	return 0;
}

int graph_run(struct filter_graph *graph)
{
	struct filter *filter;
	log_msg(1, "filters init");
	TAILQ_FOREACH(filter, &graph->filters, entries) {
		log_msg(1, "%s init", filter->cf->name);
		int ret = filter->cf->init(filter);
		if (ret < 0)
			return ret;
	}

	log_msg(1, "filters create worker");
	TAILQ_FOREACH(filter, &graph->filters, entries) {
		pthread_create(&filter->thread, NULL, filter_worker, filter);
	}
	return 0;
}

void graph_terminate(struct filter_graph *graph)
{
	struct filter *filter;
	log_msg(1, "notify filters to terminate");
	TAILQ_FOREACH(filter, &graph->filters, entries) {
		chan_lock(&filter->chan);
		filter_send_cmd(filter, CMD_EXIT);
		chan_unlock(&filter->chan);
	}

	log_msg(1, "wait filters to terminate");
	TAILQ_FOREACH(filter, &graph->filters, entries) {
		pthread_join(filter->thread, NULL);
	}
	log_msg(1, "all filters terminated");

	log_msg(1, "notify graph to terminate");
	chan_send_cmd(&graph->chan, CMD_EXIT);
	log_msg(1, "wait graph to terminate");
	pthread_join(graph->thread, NULL);
	log_msg(1, "graph terminated");
}

void *graph_worker(void *arg)
{
	struct filter_graph *graph = (struct filter_graph *)arg;
	int ret;
	while (1) {
		enum filter_cmd cmd;
		ret = chan_get_cmd(&graph->chan, (int *)&cmd);
		log_msg(1, "graph got cmd %d", cmd);
		if (ret < 0)
			break;
		ret = graph_cmd_handler(graph, cmd);
		if (cmd == CMD_EXIT || ret < 0)
			break;
	}
	log_msg(1, "graph stop (%d)", ret);
	if (ret < 0)
		program_terminate(graph->ctx, ret);
	return NULL;
}

void graph_register(struct filter_graph *graph, struct filter *filter)
{
	TAILQ_INSERT_TAIL(&graph->filters, filter, entries);
}
