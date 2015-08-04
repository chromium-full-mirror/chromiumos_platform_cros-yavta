#ifndef CROS_YAVTA_YAVTA_H_
#define CROS_YAVTA_YAVTA_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/queue.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XShm.h>
#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

#include "videodev2.h"

#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))

struct v4lconvert_data;
struct evbuffer;
struct webui;
struct filter;
struct stream;
struct save_context;

enum save_phase {
	SAVE_PHASE_CAPTURED,
	SAVE_PHASE_DECODED,
};

/* ------------------------------------------------------------------
 * buffer is general memory block container. It holds media data and is been
 * passed around filters.
 */
enum buffer_fill_mode {
	BUFFER_FILL_NONE = 0,
	BUFFER_FILL_FRAME = 1 << 0,
	BUFFER_FILL_PADDING = 1 << 1,
};

/* The memory pointer keeped inside buffer could be allocated by mmap, malloc,
 * or foreign. Foreign memory is allocated by others and buffer doesn't own it.
 */
enum buffer_alloc_type {
	BUFFER_ALLOC_TYPE_NONE,
	BUFFER_ALLOC_TYPE_MMAP,
	BUFFER_ALLOC_TYPE_MALLOC,
	BUFFER_ALLOC_TYPE_FOREIGN,
};

TAILQ_HEAD(buffer_list, buffer);
struct buffer {
	enum buffer_alloc_type alloc_type;
	unsigned int padding; /* extra bytes allocated in the tail. for
				 checking buffer overrun */
	unsigned int size; /* buffer capacity */
	int offset; /* extra bytes allocated before mem block */
	void *mem;
	void *remote_mem;

	/* -----------------------------------------------
	 * Extra user data for convenient, not maintained by buffer itself.
	 * These fields are filled only when necessary.
	 */
	struct v4l2_pix_format pix_fmt;
	int index;
	unsigned int serial; /* serial number per stream, 0 base. */
	unsigned int bytesused; /* data size */
	struct stream *stream; /* for display filter */
	struct filter *origin; /* this buffer is owned by which filter */
	struct v4l2_buffer v4l2_buf; /* metadata for buffer acquired from v4l2 device */
	int big_enough_for; /* keep the target resolution. So we can reuse it if resolution changed within range without reallocate */
	bool is_vp8_iframe; /* VP8 I-frame */
	char analyze_msg[1024];
	bool skip_decode;
	bool skip_output;
	TAILQ_ENTRY(buffer) entries;
};


/* ------------------------------------------------------------------
 * Command channel
 * "client" sends commands to "handler" via pipe_cmd. "handler" sends
 * response to "client" via pipe_reply.
 * Clients will lock the mutex to protect the communication session.
 */
struct command_channel {
	pthread_mutex_t mutex;
	int pipe_cmd[2];
	int pipe_reply[2];
};

/* ------------------------------------------------------------------
 * filter and graph
 */

/* The command value for command event. */
enum filter_cmd {
	CMD_DONE,
	CMD_EXIT,
	CMD_PING,
	CMD_PONG,

	CMD_LOCK,
	CMD_UNLOCK,
	CMD_SCAN_DEVICE,

	CMD_GRAPH_ADD_DEVICE,
	CMD_GRAPH_DEL_DEVICE,
	CMD_GRAPH_CAPTURE,
	CMD_GRAPH_STOP,

	CMD_DEVICE_CAPTURE_ON,
	CMD_DEVICE_CAPTURE_OFF,
	CMD_DEVICE_CHANGE_FORMAT,

        CMD_DISPLAY_ADD_STREAM,
};

/* Filter can specify when to trigger buffer ready event. For BOTH, ready
 * events will be triggered only if inbuf and outbuf are both ready.
 * Degenerated filters (pure consumer or producer) should set ANY.
 */
enum filter_buffer_requirement {
	REQUIRE_BUFFER_ANY,
	REQUIRE_BUFFER_BOTH,
};

/* The callbacks to implement filter */
struct filter_conf {
	/* The name is used when logging for debugging purpose */
	const char *name;
	enum filter_buffer_requirement require;
	int (*init)(struct filter*);
	int (*finalize)(struct filter*);
	/* Callback handler when worker started. */
	int (*start_handler)(struct filter*);
	/* When buffer is ready (see comment of filter_buffer_requirement),
	 * buffer_ready_handler will be invoked. */
	int (*buffer_ready_handler)(struct filter*);
	/* cmd_handler will be invoked if there is command event.
	 * If cmd_handler is NULL, the command is silent ignored. */
	int (*cmd_handler)(struct filter*, enum filter_cmd);
	/* If monitored_fd is set, monitored_fd_handler will be invoked when
	 * monitored_fd is ready to read.
	 * Examples:
	 * - For device, when monitored_fd is ready to read, we are ready to
	 *   DQBUF.
	 * - For X11, when monitored_fd is ready to read, there are X events to
	 *   handle (eg. window closed).
	 */
	int (*monitored_fd_handler)(struct filter*);
	/* Callback before selecting monitored_fd. This is the hook for remote
	 * protocol. See detail in device.c */
	int (*before_select_monitored_fd)(struct filter*);
};

struct filter {
	struct filter_conf *cf;
	struct context *ctx;
	struct stream *stream;
	pthread_t thread;

	/* If monitored_fd is not -1, the file descriptor will be watched and
	 * its callback monitored_fd_handler will be invoked when it is ready
	 * to read.
	 */
	int monitored_fd;

	/* command */
	struct command_channel chan;

	/* buffers */
	pthread_mutex_t buf_mutex; /* protects inbufs, freelist, and their len */
	int inbufs_len;
	struct buffer_list inbufs;
	int freelist_len;
	struct buffer_list freelist;
	int pipe_buffer_ready[2];

	/* filter connections is n-to-1 relationship. sink is referred to the buffer
	 * destination of this filter */
	struct filter *sink;
	TAILQ_ENTRY(filter) entries;
};

TAILQ_HEAD(filter_list, filter);
struct filter_graph {
	struct context *ctx;
	pthread_t thread;
	int exit_status;
	struct command_channel chan;

	/* filters holds all registered filters */
	struct filter_list filters;
};

/* ------------------------------------------------------------------
 * device
 */
enum device_state {
	DEVICE_STATE_STOP,
	DEVICE_STATE_CAPTURE,
};

struct device {
	enum device_state state;
	/* device descriptor */
	int fd;
	/* The descriptor to select(). For standalone yavta, fd_select equals to
	 * device descriptor. For remote yavta, it is the socket descriptor
	 * used in remote_select_fd(). */
	int fd_select;
	char *devname;
	int frame_count;

	enum v4l2_buf_type type;
	enum v4l2_memory memtype;
	unsigned int nbufs;
	struct buffer *buffers;
	unsigned int nbufs_queued;

	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	uint32_t pixelformat;

	struct v4l2_format src_fmt;

	void *pattern;
	unsigned int patternsize;

	struct v4lconvert_data *convert;

	/* Keeps last buffer in order to detect strange values */
	struct v4l2_buffer last_v4l2_buffer;

	/* VP8 only */
	/* flag to indicate we are requesting an I-frame. iframe_request_time
	 * is sequence of v4l2_buffer when we requested I-frame */
	bool need_iframe;
	uint32_t iframe_request_time;

	/* For remote use. Indicates the token we are waiting for. See
	 * remote_server.c for detail. */
	int waiting;
};

/* ------------------------------------------------------------------
 * display
 */
/* Holds X display */
struct x11 {
	/* display != NULL iff initialized successfully */
	Display *display;
	Atom wmDeleteMessage;
	int fd;
	int pressed_x, pressed_y;
	int window_x, window_y;
	bool EWMH_supported;
};

/* To display for each stream, this structure holds the info of the display
 * region and buffers.
 */
struct x11_window {
	int width, height;
	Window window;
	XvPortID xv_port;
	XvImage *xv_image;
	XShmSegmentInfo shminfo;
	/* out_buffer holds Xv image pointer */
	struct buffer out_buffer;
	unsigned int frame_count;
};

/* ------------------------------------------------------------------
 * Structures hold parsed command line options
 */
struct global_options {
	int do_rt;
	int do_capture_any;
	int do_output_any;
	unsigned int rt_priority;
	int http_port;
	bool strict;
	bool skip_before_iframe;
	bool check_low_fps;

	int do_sleep_forever;
	int do_pause;
	int do_benchmark;
};

/* Setting all controls to default, minimum, or maximum values */
enum ctrls_set_mode {
	SET_NONE,
	SET_DEF,
	SET_MIN,
	SET_MAX,
	SET_ALL,
};

struct device_options {
	bool use;
	char *devname;

	/* operations before capture loop */
	int do_set_time_per_frame;
	int do_enum_formats, do_set_format;
	int do_enum_inputs, do_set_input;
	int do_requeue_last;
	int no_query;

	/* output */
	int do_output;
	int window_x, window_y;

	/* Controls */
	int do_list_controls, do_get_control, do_set_control;
	enum ctrls_set_mode ctrls_op;
	int ctrl_name;
	int ctrl_layer;
	int ctrl_value;

	/* Video buffers */
	enum v4l2_memory memtype;
	unsigned int pixelformat;
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	unsigned int nbufs;
	unsigned int input;
	unsigned int quality;
	unsigned int userptr_offset;
	struct v4l2_fract time_per_frame;

	/* Capture loop */
	int do_capture;
	unsigned int skip;
	enum buffer_fill_mode fill_mode;
	unsigned int delay, nframes;

	/* files */
	const char *filename;
	int do_load, do_dump, do_dump_checksum, do_dump_decoded;
	const char *load_format;
	const char *dump_format;
	const char *dump_decoded_format;

	bool deblock;
};

#define FRAME_STATISTIC_SIZE 100
struct stats_data {
	/* circurlar queue, to keep track bitrate for recent 1 second.  */
	int head, tail;
	struct timeval frame_time[FRAME_STATISTIC_SIZE];
	int frame_size[FRAME_STATISTIC_SIZE];
};

/* ------------------------------------------------------------------
 * The primary structures of this program
 */
/* All per stream states are put into this structure */
TAILQ_HEAD(stream_list, stream);
struct stream {
	int id;
	struct device_options option;
	struct x11_window x_window;
	struct device dev;
	struct filter device_filter;
	struct filter process_filter;
	struct save_context *dump_ctx;
	struct save_context *dump_decoded_ctx;

	/* vp8 */
	vpx_codec_ctx_t *vp8_codec;
	FILE *load_fp;

	/* values for analyze */
	struct stats_data stats;

	TAILQ_ENTRY(stream) entries;
};

/* The root of all structures of this program. All global program states are put into
 * this structure.
 */
struct context {
	struct global_options global_options;
	struct webui *webui;
	struct x11 x11;
	int stream_num;
	int last_stream_id;
	struct stream_list streams;
	struct filter_graph graph;
	struct filter display_filter;
	/* If write to this pipe, filter graph will terminate. */
	int pipe_stop[2];
};

/* ------------------------------------------------------------------
 * VP8 ivf format headers
 */
struct vp8_ivf_file_hdr {
	char signature[4];
	uint16_t version;
	uint16_t hdr_len;
	uint32_t fourcc;
	uint16_t w;
	uint16_t h;
	uint32_t fps_num;
	uint32_t fps_denom;
	uint32_t num_frames;
	uint32_t unused;
} __attribute__((packed));

struct vp8_ivf_frame_hdr {
	uint32_t size;
	/* Presentation time in timebase unit. Typical 0,1,2,... */
	uint32_t timestamp_l;
	uint32_t timestamp_h;
} __attribute__((packed));

/* ------------------------------------------------------------------
 * prototypes
 */

/* util */
extern int verbose; /* verbose level to console output */
extern int log_verbose; /* verbose level to file logging */
extern __thread char thread_tag[64]; /* thread local string, make log more readable */

void timespec_add(const struct timespec *a, const struct timespec *b,
		struct timespec *c);
void timespec_sub(const struct timespec *a, const struct timespec *b,
		struct timespec *c);
bool timespec_less(const struct timespec *a, const struct timespec *b);
double timespec_to_double(const struct timespec *a);
void timeval_to_timespec(const struct timeval *a, struct timespec *b);
double timeval_to_double(const struct timeval *a);
void timecode_to_timespec(const struct v4l2_timecode *a, struct timespec *b);

const char *v4l2_buf_type_name(enum v4l2_buf_type type);
const char *v4l2_format_name(unsigned int fourcc);
unsigned int v4l2_format_code(const char *name);
bool is_v4l2_vp8_format(unsigned int format);
void convert_yuv420_to_yuyv(unsigned char *src_planes[3], int src_stride[3],
		unsigned int width, unsigned int height, int dst_stride,
		unsigned char *dst_buf);
void convert_yuyv_to_yuv420(const unsigned char *src_buf,
			    unsigned int width, unsigned int height,
			    unsigned char *dst_buf);
void convert_yuyv_to_rgb24(const unsigned char *src_buf,
		unsigned int width, unsigned int height,
		unsigned char *dst_buf);
void log_msg(int level, const char *format, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
;
void log_err(const char *format, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 1, 2)))
#endif
;
int buffer_alloc(struct buffer *buf, unsigned int size, int offset, int padding);
int buffer_mmap(struct buffer *buf, unsigned int size, int fd, unsigned int offset);
void buffer_foreign(struct buffer *buf, void *block, unsigned int size, bool erase);
int buffer_free(struct buffer *buf);
void buffer_verify(struct buffer *buf);
void buffer_fill_padding(struct buffer *buf);
int chan_init(struct command_channel *chan);
void chan_close(struct command_channel *chan);
void chan_lock(struct command_channel *chan);
void chan_unlock(struct command_channel *chan);
int chan_send_cmd(struct command_channel *chan, int arg);
int chan_get_cmd(struct command_channel *chan, int *cmd);
int chan_send_reply(struct command_channel *chan, int arg);
int chan_get_reply(struct command_channel *chan, int *reply);

/* option */
void init_device_options(struct device_options *opt);
int parse_options(int argc, char *argv[], struct context *ctx, struct global_options *global_opt);

/* stream */
struct stream* stream_get_by_id(struct context *ctx, int id);

/* device */
void video_init(struct device *dev);
int video_open(struct device *dev, const char *devname, int no_query);
void video_close(struct device *dev);
unsigned int get_control_type(struct device *dev, unsigned int id);
int get_control(struct device *dev, unsigned int id, int type, uint32_t layer, int64_t *val);
int set_controls(struct device *dev, struct v4l2_ext_controls *ctrls);
int set_control(struct device *dev, unsigned int id, int type,
		uint32_t layer, int64_t val);
int video_get_format(struct device *dev);
int video_set_format(struct device *dev, unsigned int w, unsigned int h,
		unsigned int format, unsigned int stride);
int video_get_streamparm(struct device *dev, struct v4l2_streamparm *parm);
int video_get_framerate(struct device *dev,
		struct v4l2_fract *time_per_frame);
int video_set_framerate(struct device *dev,
		struct v4l2_fract *time_per_frame);
int video_query_menu_idx(struct device *dev, int ctrl_id, int idx, struct v4l2_querymenu *menu);
void video_query_menu(struct device *dev, struct v4l2_queryctrl *query);
bool video_control_iter_next(struct device *dev, struct v4l2_queryctrl *query);
void video_set_all_controls(struct device *dev,
					enum ctrls_set_mode op, uint32_t layer);
void video_list_controls(struct device *dev, uint32_t layer);
void video_enum_frame_intervals(struct device *dev, __u32 pixelformat,
	unsigned int width, unsigned int height);
void video_enum_frame_sizes(struct device *dev, __u32 pixelformat);
void video_enum_formats(struct device *dev, enum v4l2_buf_type type);
bool video_input_iter(struct device *dev, unsigned int idx, struct v4l2_input *input);
void video_enum_inputs(struct device *dev);
int video_get_input(struct device *dev);
int video_set_input(struct device *dev, unsigned int input);
int video_set_quality(struct device *dev, unsigned int quality);
int video_get_frame_interval_idx(struct device *dev, __u32 pixelformat,
		unsigned int width, unsigned int height, unsigned int idx,
		struct v4l2_frmivalenum *ival);
int video_get_frame_size_idx(struct device *dev, __u32 pixelformat, unsigned int idx,
		struct v4l2_frmsizeenum *frame);
int video_get_formats_idx(struct device *dev, enum v4l2_buf_type type,
		unsigned int idx, struct v4l2_fmtdesc *fmt);
int request_vp8_iframe(struct device *dev);
extern struct filter_conf device_filter_cf;

/* display */
extern struct filter_conf display_filter_cf;
void maybe_resize_xvimage(struct x11 *x, struct stream *s, struct x11_window *x_window, int width, int height);

/* webui */
int webui_init(struct context *ctx);
int webui_close(struct context *ctx);
void streaming_frame_ready(struct context *ctx, struct stream *s, struct buffer *buf);

/* process */
extern struct filter_conf process_filter_cf;

/* save */
extern struct save_context *save_open(enum save_phase phase,
				      struct stream *s, const char *format);
extern int save_image(struct save_context *sctx, const struct buffer *buf);
extern int save_close(struct save_context *sctx);

/* filter */
int filter_init(struct filter *filter, struct filter_conf *cf, struct context *ctx, struct stream *stream);
void filter_uninit(struct filter *filter);
int filter_get_cmd(struct filter *filter, enum filter_cmd *cmd);
int filter_send_cmd(struct filter *filter, int arg);
int filter_send_reply(struct filter *filter, int arg);
int filter_get_reply(struct filter *filter, int *reply);
struct buffer *filter_get_inbuffer(struct filter *filter);
struct buffer *filter_get_outbuffer(struct filter *filter);
void filter_get_both_buffer(struct filter *filter, struct buffer **inbuf, struct buffer **outbuf);
void filter_release_inbuffer(struct filter *filter, struct buffer *buffer);
void filter_deliver_outbuffer(struct filter *filter, struct buffer *buffer);
int graph_init(struct filter_graph *graph, struct context *ctx);
void graph_finalize(struct filter_graph *graph);
void graph_terminate(struct filter_graph *graph);
void graph_connect(struct filter *front, struct filter *back);
int graph_run(struct filter_graph *graph);
void graph_register(struct filter_graph *graph, struct filter *filter);
void program_terminate(struct context *ctx, char token);

/* v4l2_json */
void evbuffer_add_v4l2_streamparm(struct evbuffer *buffer, struct v4l2_streamparm *parm);
void evbuffer_add_v4l2_input(struct evbuffer *buffer, struct v4l2_input *input);
void evbuffer_add_v4l2_queryctrl(struct evbuffer *buffer, struct v4l2_queryctrl *query, int64_t *value, struct evbuffer *menu_buffer);
void evbuffer_add_v4l2_querymenu(struct evbuffer *buffer, struct v4l2_querymenu *menu, bool is_menu);
void evbuffer_add_v4l2_format(struct evbuffer *buffer, struct v4l2_format *format);
void evbuffer_add_v4l2_fmtdesc(struct evbuffer *buffer, struct v4l2_fmtdesc *fmtdesc);
void evbuffer_add_v4l2_frmivalenum(struct evbuffer *buffer, struct v4l2_frmivalenum *ival);
void evbuffer_add_v4l2_frmsizeenum(struct evbuffer *buffer, struct v4l2_frmsizeenum *frame);

#endif  // CROS_YAVTA_YAVTA_H_
