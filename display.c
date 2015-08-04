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
/* Display filter
 *
 * One XDisplay and multiple X windows for each stream.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/shm.h>
#include <pthread.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XShm.h>
#define XK_MISCELLANY
#include <X11/keysymdef.h>

#include "yavta.h"

static bool is_xvport_used(struct context *ctx, XvPortID port)
{
	struct stream *s;
	TAILQ_FOREACH(s, &ctx->streams, entries) {
		if (s->x_window.xv_port == port)
			return true;
	}
	return false;
}

#define ID_YUY2 0x32595559
static int init_xvideo(struct context *ctx, struct x11 *x,
		       struct x11_window *x_window)
{
	unsigned int version;
	unsigned int revision;
	unsigned int request_base;
	unsigned int event_base;
	unsigned int error_base;
	unsigned int num_adaptors;
	XvAdaptorInfo *adaptor_info;
	int i;
	int found_port = -1;

	if (XvQueryExtension(x->display, &version, &revision, &request_base,
			     &event_base, &error_base) != Success) {
		log_err("Failed to query XVideo extension");
		return -1;
	}

	if (XvQueryAdaptors(x->display, x_window->window, &num_adaptors,
			    &adaptor_info) != Success) {
		log_err("Failed to query adaptors");
		return -1;
	}

	for (i = 0; i < (int)num_adaptors; i++) {
		XvPortID base_id = adaptor_info[i].base_id;
		int j;
		if (!(adaptor_info[i].type & XvInputMask))
			continue;
		if (!(adaptor_info[i].type & XvImageMask))
			continue;
		for (j = 0; j < (int)adaptor_info[i].num_ports; j++) {
			XvPortID port = base_id + j;
			int num_formats;
			if (is_xvport_used(ctx, port))
				continue;
			XvImageFormatValues *formats =
			    XvListImageFormats(x->display, port, &num_formats);
			int k;
			for (k = 0; k < num_formats; k++) {
				if (formats[k].id == ID_YUY2) {
					break;
				}
			}
			// If this port has YUY2 and can be grabbed,
			// we found what we want.
			if (k < num_formats
			    && XvGrabPort(x->display, port,
					  CurrentTime) == Success) {
				found_port = port;
			}
			XFree(formats);
			if (found_port != -1)
				goto done;
		}
	}

 done:
	XvFreeAdaptorInfo(adaptor_info);
	if (found_port == -1) {
		log_err("Failed to find a suitable port");
		return -1;
	}

	x_window->xv_port = found_port;
	return 0;
}

static void create_xvimage(struct x11 *x, struct x11_window *x_window,
			   int width, int height)
{
	x_window->xv_image =
	    XvShmCreateImage(x->display, x_window->xv_port, ID_YUY2, NULL,
			     width, height, &x_window->shminfo);
	x_window->shminfo.shmid =
	    shmget(IPC_PRIVATE, x_window->xv_image->data_size,
		   IPC_CREAT | 0777);
	x_window->shminfo.shmaddr = x_window->xv_image->data =
	    shmat(x_window->shminfo.shmid, 0, 0);
	x_window->shminfo.readOnly = False;

	XShmAttach(x->display, &x_window->shminfo);
	XSync(x->display, False);
	shmctl(x_window->shminfo.shmid, IPC_RMID, 0);

	struct buffer *buf = &x_window->out_buffer;
	/* erase=false, in order to preserve existing data for resize */
	buffer_foreign(buf, x_window->xv_image->data, width * height * 2,
		       false);
	log_msg(1, "display buffer size: %d", buf->size);

	buf->pix_fmt.width = width;
	buf->pix_fmt.height = height;
	buf->pix_fmt.bytesperline = width * 2;
	buf->pix_fmt.pixelformat = V4L2_PIX_FMT_YUYV;
	buf->pix_fmt.field = V4L2_FIELD_NONE;
}

static void release_xvimage(struct x11 *x, struct x11_window *x_window)
{
	XShmDetach(x->display, &x_window->shminfo);
	shmdt(x_window->shminfo.shmaddr);
	XFree(x_window->xv_image);
	buffer_free(&x_window->out_buffer);
}

void maybe_resize_xvimage(struct x11 *x, struct stream *s,
			  struct x11_window *x_window, int width, int height)
{
	XLockDisplay(x->display);
	if (width != x_window->xv_image->width
	    || height != x_window->xv_image->height) {
		log_msg(0, "change xvimage size from (%d,%d) to (%d,%d)",
			x_window->xv_image->width, x_window->xv_image->height,
			width, height);

		release_xvimage(x, x_window);
		create_xvimage(x, x_window, width, height);

		struct buffer *buf = &x_window->out_buffer;
		buf->index = 0;
		buf->stream = s;
		buf->origin = &s->process_filter;
		XResizeWindow(x->display, x_window->window, width, height);
		x_window->width = width;
		x_window->height = height;
	}
	XUnlockDisplay(x->display);
}

static int init_x11(struct x11 *x)
{
	log_msg(1, "init_x11");
	XInitThreads();
	x->display = XOpenDisplay(NULL);
	if (!x->display) {
		log_err("Error opening display");
		return -EINVAL;
	}

	x->fd = ConnectionNumber(x->display);
	return 0;
}

/* In X, window managers are responsible to handle window position. Modern
 * window managers all support EWMH (Extended Window Manager Hints). Windows
 * can send "hints" to window manager for new position. If there are no window
 * managers (e.g. ChromeOS) or the window manager doesn't support EWMH, we have
 * to use X api to move window.
 */
static bool is_EWMH_supported(Display *display)
{
	Window root = DefaultRootWindow(display);
	Atom type;
	int format;
	unsigned long nitems, bytesafter;
	unsigned char *args;
	if (Success != XGetWindowProperty(display, root,
	      				  XInternAtom(display, "_NET_SUPPORTED",
					    	      False),
					  0, 1, False, AnyPropertyType, &type,
					  &format, &nitems, &bytesafter, &args))
		return false;
	if (type != XA_ATOM)
		return false;
	XFree(args);
	return true;
}

static void x11_window_keep_above(Display *display, Window window)
{
	XEvent xe;
	memset(&xe, 0, sizeof(xe));
	xe.xclient.type = ClientMessage;
	xe.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
	xe.xclient.window = window;
	xe.xclient.format = 32;
	xe.xclient.data.l[0] = 1;	// _NET_WM_STATE_ADD
	xe.xclient.data.l[1] =
	    XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
	xe.xclient.data.l[2] = 0;
	xe.xclient.data.l[3] = 1;
	xe.xclient.data.l[4] = 0;
	XSendEvent(display, DefaultRootWindow(display),
		   False,
		   SubstructureNotifyMask | SubstructureRedirectMask, &xe);
}

static int init_x11_window(struct context *ctx, struct x11 *x,
			   struct x11_window *x_window, int window_x,
			   int window_y, int width, int height)
{
	int ret;
	if (x->display == NULL) {
		ret = init_x11(x);
		if (ret < 0) {
			log_err("Failed initializing X11");
			return ret;
		}
	}

	XLockDisplay(x->display);
	x_window->width = width;
	x_window->height = height;
	x_window->window =
	    XCreateSimpleWindow(x->display, RootWindow(x->display, 0), 1, 1,
				width, height, 0, BlackPixel(x->display, 0),
				BlackPixel(x->display, 0));

	x->wmDeleteMessage = XInternAtom(x->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(x->display, x_window->window, &x->wmDeleteMessage, 1);

	XMapWindow(x->display, x_window->window);
	XMoveWindow(x->display, x_window->window, window_x, window_y);
	XSelectInput(x->display, x_window->window,
		     ButtonPressMask | ButtonReleaseMask | Button1MotionMask |
		     KeyPressMask);
	x->EWMH_supported = is_EWMH_supported(x->display);
	log_msg(1, "is_EWMH_supported: %d", x->EWMH_supported);

	/* hint Window Manager keep above */
	x11_window_keep_above(x->display, x_window->window);

	if (init_xvideo(ctx, x, x_window)) {
		XDestroyWindow(x->display, x_window->window);
		XUnlockDisplay(x->display);
		return -EINVAL;
	}
	create_xvimage(x, x_window, width, height);
	XUnlockDisplay(x->display);

	return 0;
}

static int create_dummy_window(struct x11_window *x_window, int width,
			       int height)
{
	struct buffer *buf = &x_window->out_buffer;
	int size = width * height * 2;
	int ret = buffer_alloc(buf, size, 0, 0);
	if (ret < 0)
		return ret;
	buf->pix_fmt.width = width;
	buf->pix_fmt.height = height;
	buf->pix_fmt.bytesperline = width * 2;
	buf->pix_fmt.pixelformat = V4L2_PIX_FMT_YUYV;
	buf->pix_fmt.field = V4L2_FIELD_NONE;
	return 0;
}

static void close_x11_window(struct x11 *x, struct x11_window *x_window)
{
	if (!x_window->out_buffer.mem)
		return;

	XLockDisplay(x->display);
	release_xvimage(x, x_window);
	XvUngrabPort(x->display, x_window->xv_port, CurrentTime);
	XDestroyWindow(x->display, x_window->window);
	XUnlockDisplay(x->display);
}

static void close_x11(struct x11 *x)
{
	if (x->display == NULL)
		return;

	log_msg(1, "close_x11");
	XCloseDisplay(x->display);
	x->display = NULL;
}

static void display_image_x11(struct x11 *x11, struct x11_window *x_window)
{
	/* assume image data is already filled into x_window->out_buffer.mem */
	XLockDisplay(x11->display);
	XvImage *image = x_window->xv_image;
	GC gc = XCreateGC(x11->display, x_window->window, 0, NULL);
	XvShmPutImage(x11->display, x_window->xv_port, x_window->window, gc,
		      image,
		      0, 0, image->width, image->height,
		      0, 0, x_window->width, x_window->height, False);
	XFlush(x11->display);
	XFreeGC(x11->display, gc);
	XUnlockDisplay(x11->display);
}

static int display_open(struct filter *filter, struct stream *s)
{
	struct context *ctx = filter->ctx;
	struct filter *source = &s->process_filter;
	struct buffer *buf = &s->x_window.out_buffer;
	if (s->option.do_output) {
		int ret = init_x11_window(ctx, &ctx->x11, &s->x_window,
					  s->option.window_x,
					  s->option.window_y,
					  s->dev.width, s->dev.height);
		if (ret < 0) {
			log_err("Failed initializing X11 window");
			return ret;
		}

	} else if (s->option.do_capture) {
		int ret =
		    create_dummy_window(&s->x_window, s->dev.width,
					s->dev.height);
		if (ret < 0)
			return ret;
	}
	buf->index = 0;
	buf->stream = s;
	buf->origin = source;
	filter_release_inbuffer(filter, buf);

	return 0;
}

static int display_close(struct filter *filter, struct stream *s)
{
	struct context *ctx = filter->ctx;

	if (s->option.do_output) {
		close_x11_window(&ctx->x11, &s->x_window);
	} else {
		buffer_free(&s->x_window.out_buffer);
	}
	return 0;
}

/* ------------------------------------------------------------------
 * Filter callbacks
 */
static int display_filter_init(struct filter *filter)
{
	struct context *ctx = filter->ctx;
	struct stream *s;
	TAILQ_FOREACH(s, &ctx->streams, entries) {
		int ret = display_open(filter, s);
		if (ret < 0)
			return ret;
	}
	filter->monitored_fd = filter->ctx->x11.fd;

	return 0;
}

static int display_filter_finalize(struct filter *filter)
{
	struct context *ctx = filter->ctx;
	struct stream *s;
	TAILQ_FOREACH(s, &ctx->streams, entries) {
		display_close(filter, s);
	}
	close_x11(&ctx->x11);
	return 0;
}

static int display_filter_buffer_ready_handler(struct filter *filter)
{
	struct context *ctx = filter->ctx;
	struct buffer *inbuf = filter_get_inbuffer(filter);
	struct stream *s = inbuf->stream;

	if (s->option.do_output && inbuf->serial >= s->option.skip) {
		if (!inbuf->skip_output)
			display_image_x11(&ctx->x11, &s->x_window);
	}
	filter_release_inbuffer(filter, inbuf);

	struct v4l2_buffer *buf = &inbuf->v4l2_buf;
#if 1
	log_msg(0, "%s %u (%u) [%c] %u %u bytes %ld.%06ld; %s",
		s->option.devname,
		s->x_window.frame_count, buf->index,
		(buf->flags & V4L2_BUF_FLAG_ERROR) ? 'E' : '-',
		buf->sequence, buf->bytesused, buf->timestamp.tv_sec,
		buf->timestamp.tv_usec, inbuf->analyze_msg);
#endif
	s->x_window.frame_count++;

	if (s->x_window.frame_count == s->option.nframes) {
		program_terminate(filter->ctx, 0);
	}
	return 0;
}

static int display_filter_cmd_handler(struct filter *filter,
				      enum filter_cmd cmd)
{
	switch (cmd) {
	case CMD_DISPLAY_ADD_STREAM:
		{
			int ret;
			int sid;
			ret = chan_get_cmd(&filter->chan, &sid);
			if (ret < 0) {
				log_err("failed to get args");
				filter_send_reply(filter, -1);
				return -1;
			}
			log_msg(0, "got CMD_DISPLAY_ADD_STREAM(%d)", sid);
			struct stream *s = stream_get_by_id(filter->ctx, sid);
			if (s) {
				ret = display_open(filter, s);
				// TODO error handling
			}
		}
		chan_send_reply(&filter->chan, CMD_DONE);
		break;
	case CMD_DONE:
		// do nothing
		break;
	default:
		log_err("unknown command %d for %s", cmd, filter->cf->name);
	}
	return 0;
}

static int display_filter_monitored_fd_handler(struct filter *filter)
{
	struct context *ctx = filter->ctx;
	Display *display = ctx->x11.display;

	XLockDisplay(display);
	while (XPending(ctx->x11.display)) {
		// Pump out all pending events.
		XEvent e;
		XNextEvent(ctx->x11.display, &e);

		switch (e.type) {
		case ClientMessage:
			if ((unsigned)e.xclient.data.l[0] ==
			    ctx->x11.wmDeleteMessage) {
				log_msg(0, "X window closed");
				XUnlockDisplay(display);
				return -1;
			}
			break;
		case ButtonPress:
			{
				log_msg(0, "ButtonPress %d at (%d,%d)",
					e.xbutton.button, e.xbutton.x_root,
					e.xbutton.y_root);
				/* If the window manager support EWMH,
				 * remember the start coordinate is
				 * enough. */
				ctx->x11.pressed_x = e.xbutton.x_root;
				ctx->x11.pressed_y = e.xbutton.y_root;

				/* Otherwise, we have to use XMoveWindow() to
				 * change window position. The difficulty is,
				 * no standard method to know the size of
				 * window decorator (title bar, etc.) if window
				 * manager exists. */

				/* get window position relative to root */
				Window child;
				int wx, wy;
				XTranslateCoordinates(display,
						      e.xmotion.window,
						      DefaultRootWindow
						      (display), 0, 0, &wx, &wy,
						      &child);

				/* get window position relative to parent */
				int x, y;
				unsigned int w, h;
				unsigned int boarder, depth;
				Window root;
				XGetGeometry(display, e.xbutton.window, &root,
					     &x, &y, &w, &h, &boarder, &depth);

				if (wx != x || wy != y) {
					/* Hack: Window Manager exists.
					 * (x, y) is the offset due to window
					 * decorator I'm not sure whether this
					 * heuristic works for all window
					 * managers. */
					ctx->x11.window_x = wx - x;
					ctx->x11.window_y = wy - y;
				} else {
					ctx->x11.window_x = wx;
					ctx->x11.window_y = wy;
				}
			}
			break;
		case MotionNotify:
			if (e.xmotion.same_screen) {
				if (ctx->x11.EWMH_supported) {
					XUngrabPointer(display, CurrentTime);
					XEvent xe;
					memset(&xe, 0, sizeof(xe));
					xe.xclient.type = ClientMessage;
					xe.xclient.message_type =
					    XInternAtom(display,
							"_NET_WM_MOVERESIZE",
							False);
					xe.xclient.window = e.xmotion.window;
					xe.xclient.format = 32;
					xe.xclient.data.l[0] =
					    ctx->x11.pressed_x;
					xe.xclient.data.l[1] =
					    ctx->x11.pressed_y;
					xe.xclient.data.l[2] = 8;	// _NET_WM_MOVERESIZE_MOVE
					xe.xclient.data.l[3] = 0;
					xe.xclient.data.l[4] = 0;
					XSendEvent(display,
						   DefaultRootWindow(display),
						   False,
						   SubstructureNotifyMask |
						   SubstructureRedirectMask,
						   &xe);
					XFlush(display);
				} else {
					int wx = ctx->x11.window_x, wy =
					    ctx->x11.window_y;
					/* dx,dy is the delta of mouse move */
					int dx =
					    e.xmotion.x_root -
					    ctx->x11.pressed_x;
					int dy =
					    e.xmotion.y_root -
					    ctx->x11.pressed_y;

					XMoveWindow(display, e.xmotion.window,
						    wx + dx, wy + dy);
				}
			}
			break;
		case KeyPress:
			log_msg(0, "key (%d) pressed", e.xkey.keycode);
			KeySym ks = XLookupKeysym(&e.xkey, 0);
			if (ks == XK_Escape) {
				log_msg(0, "Key [Esc] pressed");
				program_terminate(filter->ctx, 0);
			}
			break;
		}
	}
	XUnlockDisplay(display);
	return 0;
}

struct filter_conf display_filter_cf = {
	"display_filter",
	REQUIRE_BUFFER_ANY,
	display_filter_init,
	display_filter_finalize,
	NULL,
	display_filter_buffer_ready_handler,
	display_filter_cmd_handler,
	display_filter_monitored_fd_handler,
	NULL,
};
