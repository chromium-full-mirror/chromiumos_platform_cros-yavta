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
/* Process filter
 *
 * It transforms data from input buffer to output buffer. "transform" may
 * includes:
 * - Decode compressed image, ex. MJPEG and VP8.
 * - Color space conversion
 * - Dump buffer to disk files
 * - Load buffer from disk files
 */
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <setjmp.h>
#include "yavta.h"
#include "remote.h"
#include "third_party/libjpeg/jpeg.h"

#include <libv4lconvert.h>
#include <jpeglib.h>
#include <jerror.h>

#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

struct my_jpeg_error_mgr {
	struct jpeg_error_mgr jerr;
	jmp_buf setjmp_buffer;
	void (*original_emit_message) (j_common_ptr, int);
};

static void my_error_exit(j_common_ptr cinfo)
{
	/* cinfo->err really points to a my_error_mgr struct, so coerce pointer
	 */
	struct my_jpeg_error_mgr *myerr =
	    (struct my_jpeg_error_mgr *)cinfo->err;
	(*cinfo->err->output_message) (cinfo);

	/* Return control to the setjmp point */
	longjmp(myerr->setjmp_buffer, 1);
}

static void my_emit_message(j_common_ptr cinfo, int msg_level)
{
	struct my_jpeg_error_mgr *myerr =
	    (struct my_jpeg_error_mgr *)cinfo->err;

	// skip annoying message
	if (cinfo->err->msg_code == JWRN_EXTRANEOUS_DATA)
		return;

	myerr->original_emit_message(cinfo, msg_level);
}

static int convert_buffer(struct context *ctx, struct stream *s,
			  struct buffer *inbuf, struct buffer *outbuf)
{
	if (!s->option.do_output)
		return 0;
	if (inbuf->pix_fmt.pixelformat != outbuf->pix_fmt.pixelformat)
		log_msg(3, "Will convert from %s to %s",
			v4l2_format_name(inbuf->pix_fmt.pixelformat),
			v4l2_format_name(outbuf->pix_fmt.pixelformat));

	if (inbuf->pix_fmt.pixelformat == V4L2_PIX_FMT_YUYV) {
		if (inbuf->bytesused > outbuf->size) {
			log_err
			    ("camera image size (%d) exceeded buffer size (%d)",
			     inbuf->bytesused, outbuf->size);
			return -1;
		}
		memcpy(outbuf->mem, inbuf->mem, inbuf->bytesused);
	} else if (is_v4l2_vp8_format(inbuf->pix_fmt.pixelformat)) {
		vpx_codec_iter_t iter = NULL;
		vpx_image_t *img;
		if (vpx_codec_decode(s->vp8_codec,
				     inbuf->mem, inbuf->bytesused, NULL, 0)) {
			const char *detail =
			    vpx_codec_error_detail(s->vp8_codec);

			log_msg(0, "vpx_codec_decode failed: %s, detail: %s",
				vpx_codec_error(s->vp8_codec),
				detail ? detail : "(none)");
			if (ctx->global_options.strict)
				return -1;
			return 0;
		}

		img = vpx_codec_get_frame(s->vp8_codec, &iter);
		if (img == NULL) {
			log_msg(0,
				"Warning: VP8 decoded successfully but no frames retrieved");
			if (ctx->global_options.strict)
				return -1;
			return 0;
		}

		if (img->d_w != inbuf->pix_fmt.width ||
		    img->d_h != inbuf->pix_fmt.height) {
			/* TODO support on the fly resolution change */
			log_msg(0,
				"Warning: image size mismatch: decoded size=(%d,%d) != expected (%d,%d) (G_FMT last time)",
				img->d_w, img->d_h, inbuf->pix_fmt.width,
				inbuf->pix_fmt.height);
		}
		if (img->d_w != outbuf->pix_fmt.width ||
		    img->d_h != outbuf->pix_fmt.height) {
			/* TODO support on the fly resolution change */
			log_msg(0,
				"image size changed: decoded size=(%d,%d) != window was (%d,%d)",
				img->d_w, img->d_h, outbuf->pix_fmt.width,
				outbuf->pix_fmt.height);
			maybe_resize_xvimage(&ctx->x11, s, &s->x_window,
					     img->d_w, img->d_h);
		}

		/* VP8 decoded format is YUV420 (aka I420) */
		convert_yuv420_to_yuyv(img->planes, img->stride,
				       img->d_w, img->d_h, img->d_w * 2,
				       outbuf->mem);

		img = vpx_codec_get_frame(s->vp8_codec, &iter);
		if (img != NULL) {
			log_msg(0, "Warning: more than 1 frame after decode");
			if (ctx->global_options.strict)
				return -1;
		}
	} else if (inbuf->pix_fmt.pixelformat == V4L2_PIX_FMT_MJPEG) {
		struct jpeg_decompress_struct cinfo;
		JSAMPARRAY buffer;

		// set up error handler
		struct my_jpeg_error_mgr myjerr;
		cinfo.err = jpeg_std_error(&myjerr.jerr);
		// remember original one
		myjerr.original_emit_message = myjerr.jerr.emit_message;
		// override with my version
		myjerr.jerr.error_exit = my_error_exit;
		myjerr.jerr.emit_message = my_emit_message;
		if (setjmp(myjerr.setjmp_buffer)) {
			/* jpeg decoding failed */
			jpeg_destroy_decompress(&cinfo);
			if (ctx->global_options.strict)
				return -1;
			return 0;
		}

		jpeg_create_decompress(&cinfo);

		// MJPEG may skip huffman table, fill default first.
		std_huff_tables(&cinfo);

		// set input buffer
		jpeg_mem_src(&cinfo, inbuf->mem, inbuf->bytesused);
		jpeg_read_header(&cinfo, TRUE);

		// set output format
		cinfo.out_color_space = JCS_YCbCr;
		cinfo.num_components = 3;

		// buffer for one scanline
		jpeg_calc_output_dimensions(&cinfo);
		int row_stride = cinfo.output_width * cinfo.output_components;
		buffer = (*cinfo.mem->alloc_sarray)
		    ((j_common_ptr) & cinfo, JPOOL_IMAGE, row_stride, 1);

		jpeg_start_decompress(&cinfo);

		while (cinfo.output_scanline < cinfo.output_height) {
			int stride = outbuf->pix_fmt.width * 2;
			JSAMPLE *p = buffer[0];
			unsigned char *q =
			    outbuf->mem + cinfo.output_scanline * stride;

			(void)jpeg_read_scanlines(&cinfo, buffer, 1);

			/* convert YCbCr to YUYV */
			unsigned int x;
			for (x = 0; x < outbuf->pix_fmt.width; x += 2) {
				*q++ = p[0];	/* y */
				*q++ = (p[1] + p[4]) / 2;	/* u */
				*q++ = p[3];	/* y */
				*q++ = (p[2] + p[5]) / 2;	/* v */
				p += 6;
			}
		}
		(void)jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
	} else {
		if (use_remote) {
			log_err("remote v4lconvert is not supported");
			return -1;
		}
		struct v4l2_format tmp_fmt;
		int w = outbuf->pix_fmt.width;
		int h = outbuf->pix_fmt.height;

		/* v4lconvert doesn't support destination format = YUYV,
		 * so we have to convert it to YUV420 first.
		 */
		memset(&tmp_fmt, 0, sizeof tmp_fmt);
		tmp_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
		tmp_fmt.fmt.pix.field = V4L2_FIELD_NONE;
		tmp_fmt.fmt.pix.width = w;
		tmp_fmt.fmt.pix.height = h;
		tmp_fmt.fmt.pix.bytesperline = w;

		int tmpsize = w * h + w * h / 4 * 2;
		unsigned char *tmp_buf = (unsigned char *)alloca(tmpsize);

		struct v4l2_format src_fmt;
		src_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		src_fmt.fmt.pix = inbuf->pix_fmt;
		if (v4lconvert_convert(s->dev.convert,
				       &src_fmt, &tmp_fmt,
				       inbuf->mem,
				       inbuf->bytesused,
				       tmp_buf, tmpsize) < 0) {
			log_msg(0, "Failed converting: %s",
				v4lconvert_get_error_message(s->dev.convert));
			if (ctx->global_options.strict)
				return -1;
			return 0;
		}

		unsigned char *tmp_planes[3] = {
			tmp_buf,
			tmp_buf + w * h,
			tmp_buf + w * h + w * h / 4,
		};
		int tmp_stride[3] = {
			w,
			w / 2,
			w / 2,
		};
		convert_yuv420_to_yuyv(tmp_planes, tmp_stride, w, h,
				       w * 2, outbuf->mem);
	}

	return 0;
}

static int analyze_buffer(struct context *ctx, struct stream *s,
			  struct buffer *inbuf, struct buffer *outbuf)
{
	// TODO calculate bitrate using CPB_size/peak_bitrate
	// maintain circular queue, to track bitrate within 1 second
	int size = s->stats.tail - s->stats.head;
	if (size < 0)
		size += FRAME_STATISTIC_SIZE;
	// first, drop records exceeded 1 sec
	double tn = timeval_to_double(&inbuf->v4l2_buf.timestamp);
	while (size > 0) {
		double t0 =
		    timeval_to_double(&s->stats.frame_time[s->stats.head]);
		if (tn - t0 <= 1)
			break;
		s->stats.head = (s->stats.head + 1) % FRAME_STATISTIC_SIZE;
		size--;
	}
	// insert one new record
	s->stats.frame_time[s->stats.tail] = inbuf->v4l2_buf.timestamp;
	s->stats.frame_size[s->stats.tail] = inbuf->v4l2_buf.bytesused;
	s->stats.tail = (s->stats.tail + 1) % FRAME_STATISTIC_SIZE;
	size++;
	if (size == FRAME_STATISTIC_SIZE) {
		log_msg(0,
			"Warning: too many frames (%d) in one second, the statistics will be wrong",
			size);
		s->stats.head = (s->stats.head + 1) % FRAME_STATISTIC_SIZE;
	}
	// calculate bitfrate
	int i;
	int amount = 0;
	for (i = s->stats.head; i != s->stats.tail;
	     i = (i + 1) % FRAME_STATISTIC_SIZE) {
		amount += s->stats.frame_size[i];
	}
	snprintf(outbuf->analyze_msg, sizeof(outbuf->analyze_msg),
		 "%s bitrate %.0fKbps, %d fps",
		 inbuf->analyze_msg, amount * 8.0 / 1024, size);

	if (ctx->global_options.check_low_fps &&
	    s->dev.frame_count > 30 && size < 10) {
		log_err("low fps, terminate!");
		return -1;
	}
	return 0;
}

static int process_buffer(struct context *ctx, struct stream *s,
			  struct buffer *inbuf, struct buffer *outbuf)
{
	int ret = 0;
	outbuf->serial = inbuf->serial;
	if (inbuf->serial < s->option.skip)
		return 0;

	if (s->dev.type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		buffer_verify(inbuf);

	/* Save captured raw buffer. */
	if (s->option.do_dump) {
		ret = save_image(s->dump_ctx, inbuf);
		if (ret <0)
			return ret;
	}

	// Suppose the size won't be changed during sreaming if not VP8
	//maybe_resize_xvimage(&ctx->x11, s, &s->x_window, inbuf->pix_fmt.width, inbuf->pix_fmt.height);

	ret = analyze_buffer(ctx, s, inbuf, outbuf);
	if (ret < 0)
		return ret;

	if (ctx->webui) {
		streaming_frame_ready(ctx, s, inbuf);
	}

	if (inbuf->skip_decode)
		return 0;

	ret = convert_buffer(ctx, s, inbuf, outbuf);
	if (ret < 0)
		return ret;

	if (s->option.do_dump_decoded)
		ret = save_image(s->dump_decoded_ctx, outbuf);
	return ret;
}

/* ------------------------------------------------------------------
 * Filter callbacks
 */
static int process_filter_init(struct filter *filter)
{
	struct stream *s = filter->stream;

	if (!use_remote) {
		s->dev.convert = v4lconvert_create(s->dev.fd);
		if (!s->dev.convert) {
			log_err("Error initializing v4lconvert");
			return -EINVAL;
		}
	}

	if (s->option.do_load) {
		s->load_fp = fopen(s->option.filename, "rb");
		if (!s->load_fp) {
			log_err("failed to open file '%s'", s->option.filename);
			return -1;
		}

		if (strcmp(s->option.load_format, "ivf") == 0) {
			struct vp8_ivf_file_hdr hdr;
			if (fread(&hdr, sizeof hdr, 1, s->load_fp) != 1) {
				log_err("read vp8_ivf_file_hdr failed");
				return -1;
			}

			log_err("load ivf file, size %dx%d, %d frames",
				hdr.w, hdr.h, hdr.num_frames);
			if (s->dev.width != hdr.w || s->dev.height != hdr.h) {
				log_err
				    ("Please specify command flag '--size %dx%d'",
				     hdr.w, hdr.h);
				return -1;
			}

			/* overrides values */
			s->dev.src_fmt.fmt.pix.bytesperline = 0;
			s->dev.src_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_VP8;
			s->dev.src_fmt.fmt.pix.sizeimage = 0;
			s->option.nframes = hdr.num_frames;
		}
	}

	if (is_v4l2_vp8_format(s->dev.src_fmt.fmt.pix.pixelformat)) {
		int flags = 0;
		if (s->option.deblock) {
			flags = VPX_CODEC_USE_POSTPROC;
		}
		s->vp8_codec =
		    (vpx_codec_ctx_t *) calloc(1, sizeof(vpx_codec_ctx_t));
		if (vpx_codec_dec_init
		    (s->vp8_codec, vpx_codec_vp8_dx(), NULL, flags)) {
			const char *detail =
			    vpx_codec_error_detail(s->vp8_codec);

			log_err("vpx_codec_dec_init failed: %s, detail: %s",
				vpx_codec_error(s->vp8_codec),
				detail ? detail : "(none)");
			return -1;
		}
		if (s->option.deblock) {
			vp8_postproc_cfg_t ppcfg;
			ppcfg.post_proc_flag = VP8_DEMACROBLOCK | VP8_DEBLOCK;
			ppcfg.deblocking_level = 3;
			if (vpx_codec_control
			    (s->vp8_codec, VP8_SET_POSTPROC, &ppcfg)) {
				const char *detail =
				    vpx_codec_error_detail(s->vp8_codec);
				log_msg(0,
					"Warning: failed to turn on postproc: %s, detail: %s",
					vpx_codec_error(s->vp8_codec),
					detail ? detail : "(none)");
			}
		}
	}

	if (s->option.do_dump) {
		s->dump_ctx = save_open(SAVE_PHASE_CAPTURED, s,
					s->option.dump_format);
		if (!s->dump_ctx)
			return -1;
	}
	if (s->option.do_dump_decoded) {
		s->dump_decoded_ctx = save_open(SAVE_PHASE_DECODED, s,
						s->option.dump_decoded_format);
		if (!s->dump_decoded_ctx)
			return -1;
	}
	return 0;
}

static int process_filter_finalize(struct filter *filter)
{
	struct stream *s = filter->stream;
	if (s->dump_ctx)
		save_close(s->dump_ctx);
	if (s->dump_decoded_ctx)
		save_close(s->dump_decoded_ctx);

	if (s->dev.convert)
		v4lconvert_destroy(s->dev.convert);
	if (s->vp8_codec) {
		vpx_codec_destroy(s->vp8_codec);
		free(s->vp8_codec);
	}
	return 0;
}

static int process_filter_start_handler(struct filter *filter)
{
	struct stream *s = filter->stream;
	snprintf(thread_tag, sizeof(thread_tag), "%s", s->dev.devname);
	return 0;
}

static int process_filter_buffer_ready_handler(struct filter *filter)
{
	struct buffer *inbuf, *outbuf;
	filter_get_both_buffer(filter, &inbuf, &outbuf);

	outbuf->v4l2_buf = inbuf->v4l2_buf;
	outbuf->skip_output = inbuf->skip_output;
	int ret = process_buffer(filter->ctx, filter->stream, inbuf, outbuf);

	filter_release_inbuffer(filter, inbuf);
	filter_deliver_outbuffer(filter, outbuf);
	return ret;
}

static int process_filter_cmd_handler(struct filter *filter,
				      enum filter_cmd cmd)
{
	switch (cmd) {
	case CMD_PING:
		chan_send_reply(&filter->chan, CMD_PONG);
		break;
	case CMD_DONE:
		break;
	default:
		log_err("unknown command %d for %s", cmd, filter->cf->name);
	}
	return 0;
}

struct filter_conf process_filter_cf = {
	"process_filter",
	REQUIRE_BUFFER_BOTH,
	process_filter_init,
	process_filter_finalize,
	process_filter_start_handler,
	process_filter_buffer_ready_handler,
	process_filter_cmd_handler,
	NULL,
	NULL,
};
