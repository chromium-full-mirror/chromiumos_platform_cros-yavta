/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


// This is a simple program that wraps vp8 raw data to webm format.
// The code was copied from libvpx vpxenc.c
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include "libmkv/EbmlWriter.h"
#include "libmkv/EbmlIDs.h"
#include "third_party/libvpx/webm.h"
#include "yavta.h"

#define LITERALU64(n) n##LLU
#define MAX_OUTPUT 1024
#define MAX_CUE_SIZE 64
#define STEREO_FORMAT_MONO  0


typedef off_t EbmlLoc;


struct cue_entry
{
    unsigned int time;
    uint64_t     loc;
};

struct EbmlGlobal
{
    int64_t last_pts_ms;
    int framerate_den;
    int framerate_num;

    /* These pointers are to the start of an element */
    off_t    position_reference;
    off_t    segment_info_pos;
    off_t    track_pos;
    off_t    cue_pos;
    off_t    cluster_pos;

    /* These pointers are to the size field of the element */
    EbmlLoc  startSegment;
    EbmlLoc  startCluster;

    uint32_t cluster_timecode;
    int      cluster_open;

    struct cue_entry *cue_list;
    unsigned int      cues;

    unsigned char *buf;
    unsigned int buf_max_length;
    unsigned int length;
    unsigned int offset;
};

struct stream_state
{
    int                       g_timebase_den;
    int                       g_timebase_num;
    int                       width;
    int                       height;
    int                       frame_num; // number of frames written
    EbmlGlobal                ebml;
};

// Global variables.
struct stream_state     *streams[WEBM_MAX_NUM_STREAMS];

void Ebml_Write(EbmlGlobal *glob, const void *buffer_in, unsigned long len)
{
    unsigned char *src;
    if (glob->offset + len > glob->buf_max_length) {
      printf("Buffer too small!");
      exit(1);
    }
    src = glob->buf;
    src += glob->offset;
    memcpy(src, buffer_in, len);
    glob->offset += len;
    if (glob->offset > glob->length) glob->length = glob->offset;
}

#define WRITE_BUFFER(s) \
for(i = len-1; i>=0; i--)\
{ \
    x = *(const s *)buffer_in >> (i * CHAR_BIT); \
    Ebml_Write(glob, &x, 1); \
}
void Ebml_Serialize(EbmlGlobal *glob, const void *buffer_in, int buffer_size, unsigned long len)
{
    char x;
    int i;

    /* buffer_size:
     * 1 - int8_t;
     * 2 - int16_t;
     * 3 - int32_t;
     * 4 - int64_t;
     */
    switch (buffer_size)
    {
        case 1:
            WRITE_BUFFER(int8_t)
            break;
        case 2:
            WRITE_BUFFER(int16_t)
            break;
        case 4:
            WRITE_BUFFER(int32_t)
            break;
        case 8:
            WRITE_BUFFER(int64_t)
            break;
        default:
            break;
    }
}
#undef WRITE_BUFFER

/* Need a fixed size serializer for the track ID. libmkv provides a 64 bit
 * one, but not a 32 bit one.
 */
static void Ebml_SerializeUnsigned32(EbmlGlobal *glob, unsigned long class_id, uint64_t ui)
{
    unsigned char sizeSerialized = 4 | 0x80;
    Ebml_WriteID(glob, class_id);
    Ebml_Serialize(glob, &sizeSerialized, sizeof(sizeSerialized), 1);
    Ebml_Serialize(glob, &ui, sizeof(ui), 4);
}


static void
Ebml_StartSubElement(EbmlGlobal *glob, EbmlLoc *ebmlLoc,
                          unsigned long class_id)
{
    //todo this is always taking 8 bytes, this may need later optimization
    //this is a key that says length unknown
    uint64_t unknownLen =  LITERALU64(0x01FFFFFFFFFFFFFF);

    Ebml_WriteID(glob, class_id);
    *ebmlLoc = glob->offset;
    Ebml_Serialize(glob, &unknownLen, sizeof(unknownLen), 8);
}

static void
Ebml_EndSubElement(EbmlGlobal *glob, EbmlLoc *ebmlLoc)
{
    off_t pos;
    uint64_t size;

    /* Save the current stream pointer */
    pos = glob->offset;

    /* Calculate the size of this element */
    size = pos - *ebmlLoc - 8;
    size |=  LITERALU64(0x0100000000000000);

    /* Seek back to the beginning of the element and write the new size */
    glob->offset = *ebmlLoc;
    Ebml_Serialize(glob, &size, sizeof(size), 8);

    /* Reset the stream pointer */
    glob->offset = pos;
}


static void
write_webm_seek_element(EbmlGlobal *ebml, unsigned long id, off_t pos)
{
    uint64_t offset = pos - ebml->position_reference;
    EbmlLoc start;
    Ebml_StartSubElement(ebml, &start, Seek);
    Ebml_SerializeBinary(ebml, SeekID, id);
    Ebml_SerializeUnsigned64(ebml, SeekPosition, offset);
    Ebml_EndSubElement(ebml, &start);
}


static void
write_webm_seek_info(EbmlGlobal *ebml)
{
    EbmlLoc start;
    EbmlLoc startInfo;
    uint64_t frame_time;
    char version_string[64];

    Ebml_StartSubElement(ebml, &start, SeekHead);
    write_webm_seek_element(ebml, Tracks, ebml->track_pos);
    write_webm_seek_element(ebml, Cues,   ebml->cue_pos);
    write_webm_seek_element(ebml, Info,   ebml->segment_info_pos);
    Ebml_EndSubElement(ebml, &start);

    //segment info
    /* Assemble version string */
    strcpy(version_string, "vp8_to_web8");

    frame_time = (uint64_t)1000 * ebml->framerate_den / ebml->framerate_num;
    ebml->segment_info_pos = ebml->offset;
    Ebml_StartSubElement(ebml, &startInfo, Info);
    Ebml_SerializeUnsigned(ebml, TimecodeScale, 1000000);
    Ebml_SerializeFloat(ebml, Segment_Duration,
                        ebml->last_pts_ms + frame_time);
    Ebml_SerializeString(ebml, 0x4D80, version_string);
    Ebml_SerializeString(ebml, 0x5741, version_string);
    Ebml_EndSubElement(ebml, &startInfo);
}


static void
write_webm_file_header(EbmlGlobal *glob,
                       int width,
                       int height,
                       int framerate_den,
                       int framerate_num)
{
    {
        EbmlLoc start;
        Ebml_StartSubElement(glob, &start, EBML);
        Ebml_SerializeUnsigned(glob, EBMLVersion, 1);
        Ebml_SerializeUnsigned(glob, EBMLReadVersion, 1); //EBML Read Version
        Ebml_SerializeUnsigned(glob, EBMLMaxIDLength, 4); //EBML Max ID Length
        Ebml_SerializeUnsigned(glob, EBMLMaxSizeLength, 8); //EBML Max Size Length
        Ebml_SerializeString(glob, DocType, "webm"); //Doc Type
        Ebml_SerializeUnsigned(glob, DocTypeVersion, 2); //Doc Type Version
        Ebml_SerializeUnsigned(glob, DocTypeReadVersion, 2); //Doc Type Read Version
        Ebml_EndSubElement(glob, &start);
    }
    {
        Ebml_StartSubElement(glob, &glob->startSegment, Segment); //segment
        glob->position_reference = glob->offset;
        glob->framerate_den = framerate_den;
        glob->framerate_num = framerate_num;
        write_webm_seek_info(glob);

        {
            EbmlLoc trackStart;
            glob->track_pos = glob->offset;
            Ebml_StartSubElement(glob, &trackStart, Tracks);
            {
                unsigned int trackNumber = 1;
                uint64_t     trackID = 0;

                EbmlLoc start;
                Ebml_StartSubElement(glob, &start, TrackEntry);
                Ebml_SerializeUnsigned(glob, TrackNumber, trackNumber);
                Ebml_SerializeUnsigned32(glob, TrackUID, trackID);
                Ebml_SerializeUnsigned(glob, TrackType, 1); //video is always 1
                Ebml_SerializeString(glob, CodecID, "V_VP8");
                {
                    unsigned int pixelWidth = width;
                    unsigned int pixelHeight = height;
                    //float        frameRate   = (float)framerate_num/(float)framerate_den;

                    EbmlLoc videoStart;
                    Ebml_StartSubElement(glob, &videoStart, Video);
                    Ebml_SerializeUnsigned(glob, PixelWidth, pixelWidth);
                    Ebml_SerializeUnsigned(glob, PixelHeight, pixelHeight);
                    Ebml_SerializeUnsigned(glob, StereoMode, STEREO_FORMAT_MONO);
                    // Since we are streaming, skip frame rate
                    //Ebml_SerializeFloat(glob, FrameRate, frameRate);
                    Ebml_EndSubElement(glob, &videoStart); //Video
                }
                Ebml_EndSubElement(glob, &start); //Track Entry
            }
            Ebml_EndSubElement(glob, &trackStart);
        }
        // segment element is open
    }
}


static void
write_webm_block(EbmlGlobal               *glob,
                 int g_timebase_num, int g_timebase_den,
                 int                       is_keyframe,
                 size_t                    sz,      // length of compressed data
                 int64_t                   pts      // time stamp to show frame (in timebase)
)
{
    unsigned long  block_length;
    unsigned char  track_number;
    unsigned short block_timecode = 0;
    unsigned char  flags;
    int64_t        pts_ms;
    int            start_cluster = 0;

    /* Calculate the PTS of this frame in milliseconds */
    pts_ms = pts * 1000
             * (uint64_t)g_timebase_num / (uint64_t)g_timebase_den;
    if(pts_ms <= glob->last_pts_ms)
        pts_ms = glob->last_pts_ms + 1;
    glob->last_pts_ms = pts_ms;

    /* Calculate the relative time of this block */
    if(pts_ms - glob->cluster_timecode > SHRT_MAX)
        start_cluster = 1;
    else
        block_timecode = pts_ms - glob->cluster_timecode;

    // In order to send frames one by one, make one cluster for one frame
    start_cluster = 1;
    if(start_cluster || is_keyframe)
    {
        // Do not seek back.
        //if(glob->cluster_open)
        //    Ebml_EndSubElement(glob, &glob->startCluster);

        /* Open the new cluster */
        block_timecode = 0;
        glob->cluster_open = 1;
        glob->cluster_timecode = pts_ms;
        glob->cluster_pos = glob->offset;
        Ebml_StartSubElement(glob, &glob->startCluster, Cluster); //cluster
        Ebml_SerializeUnsigned(glob, Timecode, glob->cluster_timecode);

#if 0  // Don't need this for streaming
        /* Save a cue point if this is a keyframe. */
        if(is_keyframe)
        {
            struct cue_entry *cue, *new_cue_list;

            new_cue_list = realloc(glob->cue_list,
                                   (glob->cues+1) * sizeof(struct cue_entry));
            if(new_cue_list)
                glob->cue_list = new_cue_list;
            else {
                log_err("Failed to realloc cue list.");
                exit(EXIT_FAILURE);
            }

            cue = &glob->cue_list[glob->cues];
            cue->time = glob->cluster_timecode;
            cue->loc = glob->cluster_pos;
            glob->cues++;
        }
#endif
    }

    /* Write the Simple Block */
    Ebml_WriteID(glob, SimpleBlock);

    block_length = sz + 4;
    block_length |= 0x10000000;
    Ebml_Serialize(glob, &block_length, sizeof(block_length), 4);

    track_number = 1;
    track_number |= 0x80;
    Ebml_Write(glob, &track_number, 1);

    Ebml_Serialize(glob, &block_timecode, sizeof(block_timecode), 2);

    flags = 0;
    if(is_keyframe) flags |= 0x80;
    Ebml_Write(glob, &flags, 1);

    // end Cluster for each frame
    if(glob->cluster_open) {
        // In order to let Ebml_EndSubElement calculate correct size (including frame data),
        // shift offset and length. But do not really copy frame data.
        glob->offset += sz;
        glob->length += sz;
        Ebml_EndSubElement(glob, &glob->startCluster);
        glob->cluster_open = 0;

        // Shift back.
        glob->offset -= sz;
        glob->length -= sz;
    }
}

static void write_webm_file_footer(EbmlGlobal *glob)
{
    // Do not seek back.
    //if(glob->cluster_open)
    //    Ebml_EndSubElement(glob, &glob->startCluster);

    {
        EbmlLoc start;
        unsigned int i;

        glob->cue_pos = glob->offset;
        Ebml_StartSubElement(glob, &start, Cues);
        for(i=0; i<glob->cues; i++)
        {
            struct cue_entry *cue = &glob->cue_list[i];
            EbmlLoc start;

            Ebml_StartSubElement(glob, &start, CuePoint);
            {
                EbmlLoc start;

                Ebml_SerializeUnsigned(glob, CueTime, cue->time);

                Ebml_StartSubElement(glob, &start, CueTrackPositions);
                Ebml_SerializeUnsigned(glob, CueTrack, 1);
                Ebml_SerializeUnsigned64(glob, CueClusterPosition,
                                         cue->loc - glob->position_reference);
                //Ebml_SerializeUnsigned(glob, CueBlockNumber, cue->blockNumber);
                Ebml_EndSubElement(glob, &start);
            }
            Ebml_EndSubElement(glob, &start);
        }
        Ebml_EndSubElement(glob, &start);
    }

    // Do not seek back.
    //Ebml_EndSubElement(glob, &glob->startSegment);
}


// return: output buffer
// output_size: the size of output buffer
unsigned char *webm_get_file_header(int stream_index, int width, int height, int *output_size) {
    struct stream_state *stream;
    if (stream_index < 0 || stream_index >= WEBM_MAX_NUM_STREAMS) {
        log_err("Invalid stream index: %d", stream_index);
        return NULL;
    }
    if (output_size == NULL) {
        log_err("Output size cannot be null!");
        return NULL;
    }

    streams[stream_index] = calloc(1, sizeof(struct stream_state));
    stream = streams[stream_index];
    if(!stream) {
        log_err("Failed to allocate new stream.");
        return NULL;
    }
    /* Setup default input stream settings */
    /* Change the default timebase to a high enough value so that the
     * encoder will always create strictly increasing timestamps.
     */
    stream->g_timebase_den = 1000;
    stream->g_timebase_num = 1;
    stream->frame_num = 0;
    stream->ebml.last_pts_ms = -1;
    stream->ebml.buf = (unsigned char *) malloc(MAX_OUTPUT);
    stream->ebml.buf_max_length = MAX_OUTPUT;
    stream->ebml.length = 0;
    stream->ebml.offset = 0;
    memset(stream->ebml.buf, 0, stream->ebml.buf_max_length);
    write_webm_file_header(&stream->ebml, width, height, 1, 30);

    *output_size = stream->ebml.offset;
    return stream->ebml.buf;
}

unsigned char *webm_get_block_header(int stream_index, int is_iframe,
                                     int buffer_size, int *output_size) {
    struct stream_state *stream;
    if (stream_index < 0 || stream_index >= WEBM_MAX_NUM_STREAMS) {
        log_err("Invalid stream index: %d", stream_index);
        return NULL;
    }
    if (output_size == NULL) {
        log_err("Output size cannot be null!");
        return NULL;
    }

    stream = streams[stream_index];
    stream->ebml.buf = (unsigned char *) malloc(MAX_OUTPUT);
    stream->ebml.buf_max_length = MAX_OUTPUT;
    stream->ebml.length = 0;
    stream->ebml.offset = 0;

    write_webm_block(&stream->ebml, stream->g_timebase_num,
                     stream->g_timebase_den, is_iframe, buffer_size,
                     stream->frame_num);
    stream->frame_num++;

    *output_size = stream->ebml.offset;
    return stream->ebml.buf;
}

unsigned char *webm_get_file_footer(int stream_index, int *output_size) {
    struct stream_state *stream;
    unsigned char *output_buffer;
    int malloc_size;
    if (stream_index < 0 || stream_index >= WEBM_MAX_NUM_STREAMS) {
        log_err("Invalid stream index: %d", stream_index);
        return NULL;
    }
    if (output_size == NULL) {
        log_err("Output size cannot be null!");
        return NULL;
    }

    stream = streams[stream_index];
    malloc_size = MAX_OUTPUT + stream->ebml.cues * MAX_CUE_SIZE;
    stream->ebml.buf = (unsigned char *) malloc(malloc_size);
    stream->ebml.buf_max_length = malloc_size;
    stream->ebml.length = 0;
    stream->ebml.offset = 0;
    write_webm_file_footer(&stream->ebml);
    free(stream->ebml.cue_list);
    stream->ebml.cue_list = NULL;

    *output_size = stream->ebml.offset;
    output_buffer = stream->ebml.buf;
    return output_buffer;
}
