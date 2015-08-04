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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>

#include "yavta.h"

struct save_handler {
	enum save_phase phase;
	const char *format;
	int (*open)(struct save_context*, struct device*, const char *filename);
	int (*image)(const struct save_context*, const struct buffer*,
		     const struct v4l2_buffer*);
	int (*close)(struct save_context*);
};

struct save_context {
	struct save_handler *handler;
	const char *filename_pattern;
	FILE *fp;
	int frame_count;
};

/* Expands the first '#' character in the file name. And appends extension name.
 */
char *expand_filename(const char *pattern, unsigned int sequence,
		      const char *extname)
{
	unsigned int size = strlen(pattern) + strlen(extname);
	char *filename = malloc(size + 12);

	char *p = strchr(pattern, '#');
	if (p != NULL) {
		sprintf(filename, "%.*s%06u%s", (int)(p - pattern), pattern,
			sequence, p + 1);
	} else {
		strcpy(filename, pattern);
	}

	// Appends extension name
	if (!strchr(pattern, '.'))
		strcat(filename, extname);
	return filename;
}

int save_raw_image(const struct save_context *sctx, const struct buffer *buf,
		   const struct v4l2_buffer *v4l2_buf)
{
	int ret = 0;
	char *filename = expand_filename(sctx->filename_pattern,
					 sctx->frame_count, ".bin");
	FILE *fp = fopen(filename, "w");
	if (!fp) {
		log_err("Failed to open '%s'", filename);
		ret = -1;
	}
	free(filename);

	size_t outsize = v4l2_buf->bytesused;
	if (fp && fwrite(buf->mem, 1, outsize, fp) != outsize) {
		log_err("fwrite error");
		ret = -1;
	}
	fclose(fp);
	return ret;
}

static void ivfify_frame(const struct v4l2_buffer *buf,
			 struct vp8_ivf_frame_hdr *frm_hdr)
{
	/* Note: this function assumes little-endianness */
	uint64_t pts = buf->sequence;
	memset(frm_hdr, 0, sizeof(struct vp8_ivf_frame_hdr));
	frm_hdr->size = buf->bytesused;
	frm_hdr->timestamp_h = pts >> 32;
	frm_hdr->timestamp_l = pts & 0xffffffff;
}

int save_raw_ivf_open(struct save_context *sctx, struct device *dev,
		      const char *filename)
{
	struct vp8_ivf_file_hdr fhdr;
	struct v4l2_fract framerate;
	int ret;

	if (!is_v4l2_vp8_format(dev->src_fmt.fmt.pix.pixelformat)) {
		log_err("Format '%s' is not compatible with IVF format.",
			v4l2_format_name(dev->src_fmt.fmt.pix.pixelformat));
		return -1;
	}

	log_msg(1, "Prepending output file with IVF file header.");

	FILE *fp = fopen(filename, "w");
	if (!fp) {
		log_err("Failed to open '%s'", filename);
		return -1;
	}

	memset(&fhdr, 0, sizeof(struct vp8_ivf_file_hdr));
	fhdr.signature[0] = 'D';
	fhdr.signature[1] = 'K';
	fhdr.signature[2] = 'I';
	fhdr.signature[3] = 'F';
	fhdr.version = 0;
	fhdr.hdr_len = sizeof(struct vp8_ivf_file_hdr);
	fhdr.fourcc = v4l2_format_code("VP8");
	fhdr.w = dev->width;
	fhdr.h = dev->height;
	if (video_get_framerate(dev, &framerate)) {
		log_err("Error getting framerate");
	} else {
		/* fps = 1 / framerate */
		fhdr.fps_denom = framerate.numerator;
		fhdr.fps_num = framerate.denominator;
	}

	ret = write(fileno(fp), &fhdr, sizeof(struct vp8_ivf_file_hdr));
	if (ret < 0)
		log_err("write error: %s (%d)", strerror(errno), errno);
	else if (ret != sizeof(struct vp8_ivf_file_hdr))
		log_err("write error: only %d bytes written instead of %lu",
			ret, sizeof(struct vp8_ivf_file_hdr));

	sctx->fp = fp;

	return 0;
}

int save_raw_ivf_image(const struct save_context *sctx,
		       const struct buffer *buf,
		       const struct v4l2_buffer *v4l2_buf)
{
	struct vp8_ivf_frame_hdr frm_hdr;
	struct iovec iov[2];
	int outsize;

	ivfify_frame(v4l2_buf, &frm_hdr);
	iov[0].iov_base = &frm_hdr;
	iov[0].iov_len = sizeof(frm_hdr);
	iov[1].iov_base = (void*) buf;
	iov[1].iov_len = v4l2_buf->bytesused;
	outsize = sizeof(frm_hdr) + v4l2_buf->bytesused;
	int ret = writev(fileno(sctx->fp), iov, 2);

	if (ret < 0) {
		log_err("write error: %s (%d)", strerror(errno), errno);
		return -1;
	} else if (ret != outsize) {
		log_err("write error: only %d bytes written instead of %u",
			ret, outsize);
		return -1;
	}
	return 0;
}

int save_raw_ivf_close(struct save_context *sctx)
{
	int ret;
	struct vp8_ivf_file_hdr fhdr;

	fseek(sctx->fp, 0, SEEK_SET);
	ret = fread(&fhdr, 1, sizeof(fhdr), sctx->fp);
	if (ret != sizeof(struct vp8_ivf_file_hdr)) {
		log_err("read error: only %d bytes read instead of %lu",
			ret, sizeof(struct vp8_ivf_file_hdr));
		fclose(sctx->fp);
		return -1;
	}

	fhdr.num_frames = sctx->frame_count;

	fseek(sctx->fp, 0, SEEK_SET);

	ret = fwrite(&fhdr, 1, sizeof(fhdr), sctx->fp);
	fclose(sctx->fp);
	if (ret != sizeof(struct vp8_ivf_file_hdr)) {
		log_err("write error: only %d bytes written instead of %lu",
			ret, sizeof(struct vp8_ivf_file_hdr));
		return -1;
	}
	return 0;
}

int save_decoded_ppm_image(const struct save_context *sctx,
			   const struct buffer *buf,
			   const struct v4l2_buffer *v4l2_buf)
{
	(void) v4l2_buf;
	int ret = 0;
	unsigned int w = buf->pix_fmt.width;
	unsigned int h = buf->pix_fmt.height;
	unsigned char *rgb = malloc(w * h * 3);
	if (!rgb) {
		log_err("malloc() failed");
		return -1;
	}

	printf("%s\n", sctx->filename_pattern);
	char *filename = expand_filename(sctx->filename_pattern,
					 sctx->frame_count, ".ppm");
	FILE *fp = fopen(filename, "w");
	if (!fp) {
		log_err("Failed to open '%s'", filename);
		ret = -1;
	} else {
		convert_yuyv_to_rgb24(buf->mem, w, h, rgb);
		fprintf(fp, "P6\n%d %d\n255\n", w, h);
		size_t size = w * h * 3;
		size_t ret = fwrite(rgb, 1, size, fp);
		if (ret != size) {
			log_err("fwrite return value(%d) less than size(%d)",
					(int)ret, (int)size);
		}
		fclose(fp);
	}
	free(filename);
	free(rgb);
	return ret;
}

int save_decoded_y4m_open(struct save_context *sctx, struct device *dev,
		      const char *filename)
{
	log_msg(1, "Prepending output file with IVF file header.");

	FILE *fp = fopen(filename, "w");
	if (!fp) {
		log_err("Failed to open '%s'", filename);
		return -1;
	}

	struct v4l2_fract framerate;
	if (video_get_framerate(dev, &framerate)) {
		log_err("Error getting framerate");
	}

	char header[1024];
	int len = sprintf(header, "YUV4MPEG2 W%d H%d F%d:%d A0:0\n",
			  dev->width, dev->height, framerate.denominator,
			  framerate.numerator);
	fwrite(header, 1, len, fp);
	fflush(fp);

	sctx->fp = fp;

	return 0;
}

int save_decoded_y4m_image(const struct save_context *sctx,
			   const struct buffer *buf,
			   const struct v4l2_buffer *v4l2_buf)
{
	(void) v4l2_buf;
	char header[] = "FRAME\n";
	struct iovec iov[2];
	int outsize;
	unsigned int w = buf->pix_fmt.width;
	unsigned int h = buf->pix_fmt.height;

	unsigned char *yuv420 = malloc(w * h * 3 / 2);
	if (!yuv420) {
		log_err("malloc() failed");
		return -1;
	}
	convert_yuyv_to_yuv420(buf->mem, w, h, yuv420);

	iov[0].iov_base = &header;
	iov[0].iov_len = strlen(header);;
	iov[1].iov_base = yuv420;
	iov[1].iov_len = w * h * 3 / 2;
	outsize = iov[0].iov_len + iov[1].iov_len;
	int ret = writev(fileno(sctx->fp), iov, 2);
	free(yuv420);

	if (ret < 0) {
		log_err("write error: %s (%d)", strerror(errno), errno);
		return -1;
	} else if (ret != outsize) {
		log_err("write error: only %d bytes written instead of %u",
			ret, outsize);
		return -1;
	}
	return 0;
}

int save_decoded_y4m_close(struct save_context *sctx)
{
	fclose(sctx->fp);
	return 0;
}

struct save_handler save_handlers[] = {
	{
		SAVE_PHASE_CAPTURED,
		"raw",
		NULL,
		save_raw_image,
		NULL
	},
	{
		SAVE_PHASE_CAPTURED,
		"ivf",
		save_raw_ivf_open,
		save_raw_ivf_image,
		save_raw_ivf_close
	},
	{
		SAVE_PHASE_DECODED,
		"ppm",
		NULL,
		save_decoded_ppm_image,
		NULL
	},
	{
		SAVE_PHASE_DECODED,
		"y4m",
		save_decoded_y4m_open,
		save_decoded_y4m_image,
		save_decoded_y4m_close,
	},
	{  // dummy sentinel
		SAVE_PHASE_DECODED,
		NULL,
		NULL,
		NULL,
		NULL
	}
};

struct save_handler *get_save_handler(enum save_phase phase, const char *format)
{
	struct save_handler *h;
	for (h = save_handlers; h->format; h++) {
		if (phase == h->phase && strcmp(h->format, format) == 0)
			return h;
	}
	return NULL;
}

struct save_context *save_open(enum save_phase phase, struct stream *s,
			       const char *format)
{
	struct save_context *sctx;
	struct save_handler *handler = get_save_handler(phase, format);
	if (!handler) {
		log_err("Unknown format: %s", format);
		return NULL;
	}

	sctx = (struct save_context*) calloc(1, sizeof(struct save_context));

	sctx->handler = handler;
	sctx->filename_pattern = s->option.filename;

	if (handler->open) {
		int ret = handler->open(sctx, &s->dev, s->option.filename);
		if (ret < 0) {
			free(sctx);
			return NULL;
		}
	}

	return sctx;
}

int save_image(struct save_context *sctx, const struct buffer *buf)
{
	sctx->frame_count++;
	return sctx->handler->image(sctx, buf, &buf->v4l2_buf);
}

int save_close(struct save_context *sctx)
{
	int ret = 0;
	if (sctx->handler->close)
		ret = sctx->handler->close(sctx);
	free(sctx);
	return ret;
}
