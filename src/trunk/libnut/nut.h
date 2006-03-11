#ifndef _NUT_H
#define _NUT_H

#define NUT_VERSION 2

#define NUT_VIDEO_CLASS 0
#define NUT_AUDIO_CLASS 1

#define NUT_KEY_STREAM_FLAG 1
#define NUT_EOR_STREAM_FLAG 2

typedef struct {
	void * priv;
	size_t (*read)(void * priv, size_t len, uint8_t * buf);
	off_t (*seek)(void * priv, long long pos, int whence);
	int (*eof)(void * priv);
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
	nut_timebase_t timebase;
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
	int tmp_flag;      // 1 => use msb, 2 => coded sflags, 4 => invalid, -1 => end
	int tmp_fields;
	int tmp_sflag;     // tmp_fields = 1
	int tmp_pts;       // tmp_fields = 2
	int tmp_mul;       // tmp_fields = 3
	int tmp_stream;    // tmp_fields = 4
	int tmp_size;      // tmp_fields = 5
	int count;         // tmp_fields = 7 (6 is reserved)
} frame_table_input_t;

typedef struct {
	nut_output_stream_t output;
	int write_index;
	int max_distance;
	frame_table_input_t * fti;
} nut_muxer_opts_t;

typedef struct {
	nut_input_stream_t input;
	int read_index;
	//int cache_syncpoints;
} nut_demuxer_opts_t;

typedef struct {
	char type[7];
	char name[65];
	int64_t val;
	int den;
	uint8_t * data; // must be NULL if carries no data
} nut_info_field_t;

typedef struct {
	int count; // count=-1 terminates the nut_info_packet_t array
	int stream_id_plus1;
	int chapter_id;
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
void nut_write_info(nut_context_t * nut, const nut_info_packet_t info []);

/** do the same as the above function, but deal with frame reordering */
void nut_muxer_uninit_reorder(nut_context_t * nut);
void nut_write_frame_reorder(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf);

// Demuxer

/** Just inits stuff, can never fail. */
nut_context_t * nut_demuxer_init(nut_demuxer_opts_t * dopts);
/** Just frees stuff. */
void nut_demuxer_uninit(nut_context_t * nut);

/** All these functions return:
0 success
1 unexpected or expected EOF.
2 EAGAIN.
negative number: error, detailed error in nut_error(-1 * ret);

fatality:
EAGAIN is never fatal
EOF is always fatal
negative error is NOT fatal, UNLESS it was given by nut_read_next_packet.

free-ing data malloced by the funtions is only ever needed after a
successful call. - to protect this, things which should've been malloced will
turn to NULL.

After an EAGAIN, whichever function returned it should always be
re-called with same params when data is available.

nut_read_next_packet should always be called after a negative error.

*/

/**
@return information about next packet. pd must be alloced.
If not skipped, the _appropriate_ function must be called. */
int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd);

/** Unless read_next_packet gives e_eof, skip_packet always works.
MUST be called after an e_unknown
len will be non-zero in the case of EAGAIN or EOF
len is the amount of bytes that are LEFT, not the ones read. */
int nut_skip_packet(nut_context_t * nut, int * len);

/** Read headers, must be called ONLY after read_next_packet
gave e_headers.
@brief "s" is malloced and needs to be free'd.
*/
int nut_read_headers(nut_context_t * nut, nut_packet_t * pd, nut_stream_header_t * s []);

/** Just reads the frame DATA. all it's header has already been
read by nut_read_next_packet. buf must be allocated and big enough.
len will be non-zero in the case of EAGAIN or EOF
len is the amount of bytes that are LEFT, not the ones read. */
int nut_read_frame(nut_context_t * nut, int * len, uint8_t * buf);

/** Allocates everything necessary for info. Last info ends just as
described by spec - 'id = 0'. */
int nut_read_info(nut_context_t * nut, nut_info_packet_t * info []);

/** must be called for infodata to be freed... */
void nut_free_info(nut_info_packet_t info []);

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


/**
Internal secrets:
nut_read_next_packet:
Just about ALL error recovery will be put in nut_read_next_packet.
It should be called after an error, it should never return an error.
Only time nut_read_next_packet can return an error is if it can't
find main headers...

nut_context remembers if headers have been read or not.

If headers have not yet been read:
1. if the stream starts with "file_id_string", it will be skipped.
2. if the main headers cannot be found, nut_read_next_packet will
   start hunting for them.

Once they have been read, nut_read_next_packet will always return
e_unknown if it sees any header again. (so they will be skipped)

nut_read_next_packet will return EOF if it reachs index_startcode,
end_startcode or stream EOF.

nut_read_next_packet does handling necessary for new frames
regarding last_pts, not read_frame.

nut_read_next_packet also does syncpoint handling.
It'll start hunting syncpoints if no syncpoint has appeared for a
long time or one of the functions just return a negative error.

Other black magic:
When nut_read_headers is called, and the stream is seekable, it
searches for an index

nut_skip_packet works differently according to packet type:
it either reads forward_ptr and skips by that, or it skips by
frame data. It also mallocs and frees memory to discard the
packet if non-seekable or method 0 was chosen.

nut_read_frame is an incredibly trivial function, it just reads
the actual codec data, that's all. Only errors it could ever give
is EOF or EAGAIN.

For an example see example.c
*/

#endif // _NUT_H
