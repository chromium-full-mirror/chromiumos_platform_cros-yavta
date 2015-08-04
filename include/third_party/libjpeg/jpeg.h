#ifndef CROS_YAVTA_THIRD_PARTY_LIBJPEG_JPEG_H_
#define CROS_YAVTA_THIRD_PARTY_LIBJPEG_JPEG_H_

#include <stdio.h>
#include <jpeglib.h>

void std_huff_tables (j_decompress_ptr cinfo);
void jpeg_mem_src(j_decompress_ptr cinfo,
		  unsigned char *inbuffer, unsigned long insize);

#endif  // CROS_YAVTA_THIRD_PARTY_LIBJPEG_JPEG_H_
