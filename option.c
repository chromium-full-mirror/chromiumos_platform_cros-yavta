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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <getopt.h>
#include <sys/queue.h>

#include "yavta.h"
#include "remote.h"

#define V4L_BUFFERS_DEFAULT	8
#define V4L_BUFFERS_MAX		32

static void usage(const char *argv0)
{
	printf("Usage: %s [options] device\n", argv0);
	printf("Supported global options:\n");
	printf("-h, --help			Show this help screen\n");
	printf("-p, --pause			Pause before starting the video stream\n");
	printf("    --port port                 HTTP port for web UI\n");
	printf("-R, --realtime=[priority]	Enable realtime RR scheduling\n");
	printf("    --sleep-forever		Sleep forever after configuring the device\n");
	printf("-v, --verbose n			Verbose level\n");
	printf("    --log_verbose n		Log verbose level\n");
	printf("\tverbose level:\n");
	printf("\t\t0 start up msg, error msg, info\n");
	printf("\t\t1 detail not per-frame msg\n");
	printf("\t\t2 brief per-frame msg\n");
	printf("\t\t3 detail per-frame msg, ioctl\n");
	printf("    --strict			Treat decoder warnings as error\n");
	printf("    --skip-before-iframe	Don't decode+display before I-frame arrived\n");
	printf("    --remote server-ip		Remote server IP address\n");
	printf("    --bench			Benchmark remote overhead\n");
	printf("    --check-low-fps		Exit if fps is low\n");
	printf("-1, -2, ...			Specify which device to apply the following device options\n");
	printf("\n");
	printf("Supported device options:\n");
	printf("-c, --capture[=nframes]		Capture frames\n");
	printf("-C, --check-overrun		Verify dequeued frames for buffer overrun\n");
	printf("-d, --delay			Delay (in ms) before requeuing buffers\n");
	printf("-f, --format format		Set the video format\n");
	printf("-i, --input input		Select the video input\n");
	printf("-I, --fill-frames		Fill frames with check pattern before queuing them\n");
	printf("-l, --list-controls		List available controls\n");
	printf("-n, --nbufs n			Set the number of video buffers\n");
	printf("-o, --output			Output to an X11 window\n");
	printf("-q, --quality n			MJPEG quality (0-100)\n");
	printf("-r, --get-control ctrl		Get control 'ctrl'\n");
	printf("-s, --size WxH			Set the frame size\n");
	printf("-t, --time-per-frame num/denom	Set the time per frame (eg. 1/25 = 25 fps)\n");
	printf("-u, --userptr			Use the user pointers streaming method\n");
	printf("-w, --set-control 'ctrl value'	Set control 'ctrl' to 'value'\n");
	printf("    --enum-formats		Enumerate formats\n");
	printf("    --enum-inputs		Enumerate inputs\n");
	printf("    --no-query			Don't query capabilities on open\n");
	printf("    --offset			User pointer buffer offset from page start\n");
	printf("    --requeue-last		Requeue the last buffers before streamoff\n");
	printf("    --skip n			Skip the first n frames\n");
	printf("    --stride value		Line stride in bytes\n");
	printf("    --set-ctrls-default		Set all controls to their default values\n");
	printf("    --set-ctrls-min		Set all controls to their minimum values\n");
	printf("    --set-ctrls-max		Set all controls to their maximum values\n");
	printf("    --set-ctrls-all		Set all controls to each of their valid values in sequence\n");
	printf("    --control-layer layer	Specify which temporal layer for control operations (get/set/list)\n");
	printf("    --geometry +X+Y		Set window position\n");
	printf("    --deblock			Enable VP8 decoder deblock post-processing\n");
	printf("    --load[=format]		Load frames from disk\n");
	printf("    --dump[=format]		Write frames to disk\n");
	printf("\tformat could be 'raw', 'ivf'. default=raw.\n");
	printf("    --dump_decoded[=format]	Write decoded frames to disk\n");
	printf("\tformat could be 'ppm', 'y4m'. default=ppm.\n");
	printf("-F, --file name			The filename to read/write frames from/to disk\n");
	printf("\tFor video capture devices, the first '#' character in the file name is\n");
	printf("\texpanded to the frame sequence number. The default file name is\n");
	printf("\t'frame-#.bin'.\n");
}

#define OPT_ENUM_FORMATS	256
#define OPT_ENUM_INPUTS		257
#define OPT_SKIP_FRAMES		258
#define OPT_NO_QUERY		259
#define OPT_SLEEP_FOREVER	260
#define OPT_USERPTR_OFFSET	261
#define OPT_REQUEUE_LAST	262
#define OPT_STRIDE		263
#define OPT_DUMP		264
#define OPT_CTRLS_SET_MIN	265
#define OPT_CTRLS_SET_MAX	266
#define OPT_CTRLS_SET_DEF	267
#define OPT_CTRLS_SET_ALL	268
#define OPT_GEOMETRY		269
#define OPT_LOAD		270
#define OPT_HTTP_PORT		271
#define OPT_LOG_VERBOSE		272
#define OPT_DUMP_DECODED	273
#define OPT_STRICT		274
#define OPT_SKIP_BEFORE_IFRAME	275
#define OPT_REMOTE		276
#define OPT_BENCH		277
#define OPT_DEBLOCK		278
#define OPT_CTRL_LAYER		279
#define OPT_CHECK_LOW_FPS	280

static struct option opts[] = {
	{"bench", 0, 0, OPT_BENCH},
	{"capture", 2, 0, 'c'},
	{"check-low-fps", 0, 0, OPT_CHECK_LOW_FPS},
	{"check-overrun", 0, 0, 'C'},
	{"control-layer", 1, 0, OPT_CTRL_LAYER},
	{"deblock", 0, 0, OPT_DEBLOCK},
	{"set-ctrls-default", 0, 0, OPT_CTRLS_SET_DEF},
	{"set-ctrls-min", 0, 0, OPT_CTRLS_SET_MIN},
	{"set-ctrls-max", 0, 0, OPT_CTRLS_SET_MAX},
	{"set-ctrls-all", 0, 0, OPT_CTRLS_SET_ALL},
	{"delay", 1, 0, 'd'},
	{"dump", 2, 0, OPT_DUMP},
	{"dump_decoded", 2, 0, OPT_DUMP_DECODED},
	{"enum-formats", 0, 0, OPT_ENUM_FORMATS},
	{"enum-inputs", 0, 0, OPT_ENUM_INPUTS},
	{"file", 1, 0, 'F'},
	{"fill-frames", 0, 0, 'I'},
	{"geometry", 1, 0, OPT_GEOMETRY},
	{"format", 1, 0, 'f'},
	{"help", 0, 0, 'h'},
	{"input", 1, 0, 'i'},
	{"list-controls", 0, 0, 'l'},
	{"load", 2, 0, OPT_LOAD},
	{"log_verbose", 1, 0, OPT_LOG_VERBOSE},
	{"nbufs", 1, 0, 'n'},
	{"no-query", 0, 0, OPT_NO_QUERY},
	{"offset", 1, 0, OPT_USERPTR_OFFSET},
	{"output", 0, 0, 'o'},
	{"pause", 0, 0, 'p'},
	{"port", 1, 0, OPT_HTTP_PORT},
	{"quality", 1, 0, 'q'},
	{"get-control", 1, 0, 'r'},
	{"remote", 1, 0, OPT_REMOTE},
	{"requeue-last", 0, 0, OPT_REQUEUE_LAST},
	{"realtime", 2, 0, 'R'},
	{"size", 1, 0, 's'},
	{"set-control", 1, 0, 'w'},
	{"skip", 1, 0, OPT_SKIP_FRAMES},
	{"sleep-forever", 0, 0, OPT_SLEEP_FOREVER},
	{"strict", 0, 0, OPT_STRICT},
	{"skip-before-iframe", 0, 0, OPT_SKIP_BEFORE_IFRAME},
	{"stride", 1, 0, OPT_STRIDE},
	{"time-per-frame", 1, 0, 't'},
	{"userptr", 0, 0, 'u'},
	{"verbose", 1, 0, 'v'},
	{0, 0, 0, 0}
};

static void log_cmdline(int argc, char *argv[])
{
	int i;
	log_msg(1, "argc = %d", argc);
	for (i = 0; i < argc; i++) {
		log_msg(1, "argv[%d] = \"%s\"", i, argv[i]);
	}
}

struct stream *stream_get_by_id(struct context *ctx, int id)
{
	struct stream *s;
	TAILQ_FOREACH(s, &ctx->streams, entries) {
		if (s->id == id)
			return s;
	}
	return NULL;
}

struct stream *stream_new(struct context *ctx)
{
	struct stream *s = calloc(1, sizeof(struct stream));
	if (!s)
		return NULL;
	s->id = ctx->last_stream_id++;
	init_device_options(&s->option);
	ctx->stream_num++;
	TAILQ_INSERT_TAIL(&ctx->streams, s, entries);
	return s;
}

int parse_options(int argc, char *argv[], struct context *ctx,
		  struct global_options *global_opt)
{
	/* Options parsings */
	char *endptr;
	int c;
	int i;

	struct stream *stream = stream_new(ctx);
	struct device_options *opt = &stream->option;
	opt->use = true;

	opterr = 0;

	while ((c =
		getopt_long(argc, argv,
			    "123456789c::Cd:f:F:hi:Iln:opq:r:R::s:t:uw:v:",
			    opts, NULL)) != -1) {

		switch (c) {
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			{
				int sidx = c - '1';
				if (sidx >= ctx->stream_num) {
					for (i = ctx->stream_num; i <= sidx;
					     i++)
						stream = stream_new(ctx);
				}
				stream = stream_get_by_id(ctx, sidx);
				assert(stream);
				opt = &stream->option;
				opt->use = true;
				break;
			}

		case 'c':
			opt->do_capture = 1;
			global_opt->do_capture_any = 1;
			if (optarg)
				opt->nframes = atoi(optarg);
			break;
		case 'C':
			opt->fill_mode |= BUFFER_FILL_PADDING;
			break;
		case 'd':
			opt->delay = atoi(optarg);
			break;
		case 'f':
			opt->do_set_format = 1;
			opt->pixelformat = v4l2_format_code(optarg);
			if (opt->pixelformat == 0) {
				log_err("Unsupported video format '%s'",
					optarg);
				return 1;
			}
			break;
		case 'F':
			opt->filename = strdup(optarg);
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'i':
			opt->do_set_input = 1;
			opt->input = atoi(optarg);
			break;
		case 'I':
			opt->fill_mode |= BUFFER_FILL_FRAME;
			break;
		case 'l':
			opt->do_list_controls = 1;
			break;
		case 'n':
			opt->nbufs = atoi(optarg);
			if (opt->nbufs > V4L_BUFFERS_MAX)
				opt->nbufs = V4L_BUFFERS_MAX;
			break;
		case 'o':
			opt->do_output = 1;
			global_opt->do_output_any = 1;
			break;
		case 'p':
			global_opt->do_pause = 1;
			break;
		case 'q':
			opt->quality = atoi(optarg);
			break;
		case 'r':
			opt->ctrl_name = strtol(optarg, &endptr, 0);
			if (*endptr != 0) {
				log_err("Invalid control name '%s'", optarg);
				return 1;
			}
			opt->do_get_control = 1;
			break;
		case 'R':
			global_opt->do_rt = 1;
			global_opt->rt_priority = 1;
			if (optarg)
				global_opt->rt_priority = atoi(optarg);
			break;
		case 's':
			opt->do_set_format = 1;
			opt->width = strtol(optarg, &endptr, 10);
			if (*endptr != 'x' || endptr == optarg) {
				log_err("Invalid size '%s'", optarg);
				return 1;
			}
			opt->height = strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				log_err("Invalid size '%s'", optarg);
				return 1;
			}
			break;
		case 't':
			opt->do_set_time_per_frame = 1;
			opt->time_per_frame.numerator =
			    strtol(optarg, &endptr, 10);
			if (*endptr != '/' || endptr == optarg) {
				log_err("Invalid time per frame '%s'", optarg);
				return 1;
			}
			opt->time_per_frame.denominator =
			    strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				log_err("Invalid time per frame '%s'", optarg);
				return 1;
			}
			break;
		case 'u':
			opt->memtype = V4L2_MEMORY_USERPTR;
			break;
		case 'w':
			opt->ctrl_name = strtol(optarg, &endptr, 0);
			log_msg(0, "ctrlname: %d %x", opt->ctrl_name,
				opt->ctrl_name);
			if (*endptr != ' ' || endptr == optarg) {
				log_err("Invalid control name '%s'", optarg);
				return 1;
			}
			opt->ctrl_value = strtol(endptr + 1, &endptr, 0);
			if (*endptr != 0) {
				log_err("Invalid control value '%s'", optarg);
				return 1;
			}
			opt->do_set_control = 1;
			break;
		case 'v':
			verbose = atoi(optarg);
			break;
		case OPT_ENUM_FORMATS:
			opt->do_enum_formats = 1;
			break;
		case OPT_ENUM_INPUTS:
			opt->do_enum_inputs = 1;
			break;
		case OPT_NO_QUERY:
			opt->no_query = 1;
			break;
		case OPT_REQUEUE_LAST:
			opt->do_requeue_last = 1;
			break;
		case OPT_SKIP_FRAMES:
			opt->skip = atoi(optarg);
			break;
		case OPT_SLEEP_FOREVER:
			global_opt->do_sleep_forever = 1;
			break;
		case OPT_STRIDE:
			opt->stride = atoi(optarg);
			break;
		case OPT_USERPTR_OFFSET:
			opt->userptr_offset = atoi(optarg);
			break;
		case OPT_DUMP:
			opt->do_dump = 1;
			if (optarg)
				opt->dump_format = strdup(optarg);
			break;
		case OPT_DUMP_DECODED:
			opt->do_dump_decoded = 1;
			if (optarg)
				opt->dump_decoded_format = strdup(optarg);
			break;
		case OPT_LOAD:
			opt->do_load = 1;
			if (optarg)
				opt->load_format = strdup(optarg);
			break;
		case OPT_CTRLS_SET_MIN:
			opt->ctrls_op = SET_MIN;
			break;
		case OPT_CTRLS_SET_MAX:
			opt->ctrls_op = SET_MAX;
			break;
		case OPT_CTRLS_SET_DEF:
			opt->ctrls_op = SET_DEF;
			break;
		case OPT_CTRLS_SET_ALL:
			opt->ctrls_op = SET_ALL;
			break;
		case OPT_GEOMETRY:
			if (sscanf(optarg, "+%d+%d", &opt->window_x,
				   &opt->window_y) != 2) {
				log_err("Invalid geometry spec. Only supports +X+Y");
				return 1;
			}
			break;
		case OPT_HTTP_PORT:
			global_opt->http_port = atoi(optarg);
			break;
		case OPT_LOG_VERBOSE:
			log_verbose = atoi(optarg);
			break;
		case OPT_STRICT:
			global_opt->strict = true;
			break;
		case OPT_SKIP_BEFORE_IFRAME:
			global_opt->skip_before_iframe = true;
			break;
		case OPT_REMOTE:
			use_remote = true;
			strcpy(remote_server_name, optarg);
			break;
		case OPT_BENCH:
			global_opt->do_benchmark = true;
		case OPT_DEBLOCK:
			opt->deblock = true;
			break;
		case OPT_CTRL_LAYER:
			opt->ctrl_layer = atoi(optarg);
			break;
		case OPT_CHECK_LOW_FPS:
			global_opt->check_low_fps = true;
			break;
		default:
			printf("Invalid option -%c\n", c);
			printf("Run %s -h for help.\n", argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}
	log_cmdline(argc, argv);

	if (argc - optind < ctx->stream_num) {
		log_err("There are options specified for stream#%d, but no device name specified.",
		        argc - optind);
		return 1;
	}

	for (i = 0; argv[optind + i]; i++) {
		char *devname = argv[optind + i];
		if (i >= ctx->stream_num) {
			log_msg(0,
				"Warning: no options specified for device '%s'",
				devname);
			stream_new(ctx);
		}
		struct stream *s = stream_get_by_id(ctx, i);
		s->option.devname = strdup(devname);
	}

	log_msg(1, "stream_num = %d", ctx->stream_num);

	/* make sure options are consistent */
	if (global_opt->do_benchmark && !use_remote) {
		log_err("--remote should be used with --bench");
		return 1;
	}
	TAILQ_FOREACH(stream, &ctx->streams, entries) {
		opt = &stream->option;
		if (opt->do_output) {
			opt->do_capture = 1;
			global_opt->do_capture_any = 1;
		}

		if ((opt->fill_mode & BUFFER_FILL_PADDING) &&
		    opt->memtype != V4L2_MEMORY_USERPTR) {
			log_err("Buffer overrun can only be checked in USERPTR mode.");
			return 1;
		}

		/* dump/load filename and format */
		if (opt->do_dump && opt->do_load) {
			log_err("Cannot --dump and --load at the same time");
			return 1;
		}
		if (strcmp(opt->load_format, "raw") != 0
		    && strcmp(opt->load_format, "ivf") != 0) {
			log_err("File format '%s' is not supported.",
				opt->load_format);
			return 1;
		}
		if (opt->do_dump) {
			if (strcmp(opt->load_format, "ivf") == 0
			    && strchr(opt->filename, '#')) {
				log_err("Cannot use '#' character in filename with ivf format.");
				return 1;
			}
		}
		if (opt->do_load) {
			if (strchr(opt->filename, '#')) {
				log_err("Loading frames from filename with  '#' character is not implemented yet.");
				return 1;
			}
		}

		if (opt->memtype == V4L2_MEMORY_USERPTR && use_remote) {
			log_err("Not supported userptr with remote yet");
			return 1;
		}
	}

	return 0;
}

void init_device_options(struct device_options *opt)
{
	memset(opt, 0, sizeof(*opt));

	/* Video buffers */
	opt->memtype = V4L2_MEMORY_MMAP;
	opt->pixelformat = V4L2_PIX_FMT_YUYV;
	opt->width = 640;
	opt->height = 480;
	opt->nbufs = V4L_BUFFERS_DEFAULT;
	opt->quality = (unsigned int)-1;
	opt->userptr_offset = 0;
	opt->time_per_frame.numerator = 1;
	opt->time_per_frame.denominator = 25;

	/* Capture loop */
	opt->fill_mode = BUFFER_FILL_NONE;
	opt->nframes = (unsigned int)-1;
	opt->filename = "frame-#";
	opt->load_format = "raw";
	opt->dump_format = "raw";
	opt->dump_decoded_format = "ppm";
}
