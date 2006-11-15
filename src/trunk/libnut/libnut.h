// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#ifndef _NUT_H
#define _NUT_H

#define NUT_VERSION 2

#define NUT_VIDEO_CLASS    0
#define NUT_AUDIO_CLASS    1
#define NUT_SUBTITLE_CLASS 2
#define NUT_USERDATA_CLASS 3

#define NUT_FLAG_KEY 1
#define NUT_FLAG_EOR 2

typedef struct {
	void * priv;
	size_t (*read)(void * priv, size_t len, uint8_t * buf);
	off_t (*seek)(void * priv, long long pos, int whence); // can be NULL, but implies no read_index and no cache_syncpoints
	int (*eof)(void * priv); // can be NULL, implies any read error is caused by EOF
	off_t file_pos;
} nut_input_stream_t;

typedef struct {
	void * priv;
	int (*write)(void * priv, size_t len, const uint8_t * buf);
} nut_output_stream_t;

typedef struct { // for example 23.976 (24000/1001)
	int nom; // is 1001
	int den; // 24000
} nut_timebase_t;

typedef struct {
	int type; // -1 means end
	int fourcc_len;
	uint8_t * fourcc;
	nut_timebase_t time_base;
	int fixed_fps;
	int decode_delay;
	int codec_specific_len;
	uint8_t * codec_specific;
	// video
	int width;
	int height;
	int sample_width;
	int sample_height;
	int colorspace_type;
	// audio
	int samplerate_nom;
	int samplerate_denom;
	int channel_count;
	// does not need to be set, only read
	int max_pts;
} nut_stream_header_t;

typedef struct {
	int tmp_flag;      // -1 => end
	int tmp_fields;
	int tmp_pts;       // tmp_fields = 1
	int tmp_mul;       // tmp_fields = 2
	int tmp_stream;    // tmp_fields = 3
	int tmp_size;      // tmp_fields = 4
	int count;         // tmp_fields = 6 (5 is reserved)
} frame_table_input_t;

typedef struct {
	void * (*malloc)(size_t size);
	void * (*realloc)(void *ptr, size_t size);
	void (*free)(void *ptr);
} nut_alloc_t;

typedef struct {
	nut_output_stream_t output;
	nut_alloc_t alloc;
	int write_index;
	int realtime_stream; // implies no write_index
	int max_distance;
	frame_table_input_t * fti;
} nut_muxer_opts_t;

typedef struct {
	nut_input_stream_t input;
	nut_alloc_t alloc;
	int read_index; // implies cache_syncpoints
	int cache_syncpoints;
} nut_demuxer_opts_t;

typedef struct {
	char type[7];
	char name[65];
	int64_t val; // used in all types, is the length of data if there is data
	int den; // used in type=="r"
	nut_timebase_t tb; // used in type=="t"
	uint8_t * data; // must be NULL if carries no data
} nut_info_field_t;

typedef struct {
	int count; // count=-1 terminates the nut_info_packet_t array
	int stream_id_plus1;
	int chapter_id;
	nut_timebase_t chapter_tb;
	uint64_t chapter_start;
	uint64_t chapter_len;
	nut_info_field_t * fields;
} nut_info_packet_t;

typedef struct {
	enum { e_headers, e_unknown, e_frame, e_info } type; // unused for muxer
	// always used
	int len;
	// only used if type is e_frame
	int stream;
	uint64_t pts;
	int flags; // 1 - keyframe, 2 - EOR
	// not manditory, for reorderer
	int64_t next_pts;
} nut_packet_t;

struct nut_context_s;
typedef struct nut_context_s nut_context_t;

// Muxer

/** allocs nut context, writes headers to file */
nut_context_t * nut_muxer_init(const nut_muxer_opts_t * mopts, const nut_stream_header_t s[], const nut_info_packet_t info[]);
/** writes index (optionally), frees alloced ram */
void nut_muxer_uninit(nut_context_t * nut);

/** nut_write_frame does magic, it writes headers */
void nut_write_frame(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf);
/** the use of this function is illegal in non realtime streams!! */
void nut_write_info(nut_context_t * nut, const nut_info_packet_t * info);

/** do the same as the above function, but deal with frame reordering */
void nut_write_frame_reorder(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf);
void nut_muxer_uninit_reorder(nut_context_t * nut);

// Demuxer

/** Just inits stuff, can never fail, except memory error... */
nut_context_t * nut_demuxer_init(nut_demuxer_opts_t * dopts);
/** Just frees stuff. */
void nut_demuxer_uninit(nut_context_t * nut);

/**
All of the following functions return:
0 success
1 unexpected or expected EOF.
2 EAGAIN.
negative number: error, detailed error in nut_error(-1 * ret);

All errors except EAGAIN or ERR_NOT_SEEKABLE from nut_seek are fatal.
The only legal operation after a fatal error is nut_demuxer_uninit().

After an EAGAIN, whichever function returned it should always be
re-called with same params when data is available.
*/

/** Read headers, must be called at begginning
Both `s' and `info' are handeled by context, and are illegal pointers
after nut_demuxer_uninit() . If `info' is NULL, no info is read.
*/
int nut_read_headers(nut_context_t * nut, nut_stream_header_t * s [], nut_info_packet_t * info []);

/** Gives information on next frame, must be called before each packet. */
int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd);

int nut_skip_packet(nut_context_t * nut, int * len);

/** Just reads the frame DATA. all it's header has already been
read by nut_read_next_packet. buf must be allocated and big enough.
len will be non-zero in the case of EAGAIN or EOF
len is the amount of bytes that are LEFT, not the ones read. */
int nut_read_frame(nut_context_t * nut, int * len, uint8_t * buf);

/** "Can't find start code" "invalid frame code" "bad checksum" "bleh" */
const char * nut_error(int error);

/** Seeks to requested position in seconds
if (flags & 1), time_pos is relative to current position, otherwise absoloute.
if (flags & 2), the seek should go forwards when seeking, and find
                the nearest keyframe after target pts.
                No percise seeking is preformed.
if it returns (non fatal) error, no seek is preformed.
After nut_seek, nut_read_next_packet should be called to get the next frame.
active_streams is a -1 terminated list of all streams that are active...
if NULL, all streams are active.
*/
int nut_seek(nut_context_t * nut, double time_pos, int flags, const int * active_streams);

#endif // _NUT_H
