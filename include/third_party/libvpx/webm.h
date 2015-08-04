#ifndef CROS_THIRD_PARTY_LIBVPX_WEBM_H_
#define CROS_THIRD_PARTY_LIBVPX_WEBM_H_

#define WEBM_MAX_NUM_STREAMS 8

// Returns webm header and the size is output_size.
unsigned char *webm_get_file_header(
    int stream_index, // the index of the stream. 0 to WEBM_MAX_NUM_STREAMS - 1
    int width,        // the width of frames
    int height,       // the height of frames
    int *output_size  // the size of the file header
);

// Returns block header and the size is output_size.
unsigned char *webm_get_block_header(
    int stream_index, // the index of the stream. 0 to WEBM_MAX_NUM_STREAMS - 1
    int is_iframe,    // 1 if this is an I frame. 0 otherwise.
    int buffer_size,  // the buffer size
    int *output_size  // the size of the block header
);

// Returns file footer and the size is output_size.
unsigned char *webm_get_file_footer(
    int stream_index, // the index of the stream. 0 to WEBM_MAX_NUM_STREAMS - 1
    int *output_size  // the size of the file rooter
);

#endif  // CROS_THIRD_PARTY_LIBVPX_WEBM_H_
