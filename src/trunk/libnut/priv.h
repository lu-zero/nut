#ifndef _NUT_PRIV_H
#define _NUT_PRIV_H

//#define NDEBUG
//#define TRACE
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ID_STRING "nut/multimedia container"

#define      MAIN_STARTCODE (0x7A561F5F04ADULL + (((uint64_t)('N'<<8) + 'M')<<48))
#define    STREAM_STARTCODE (0x11405BF2F9DBULL + (((uint64_t)('N'<<8) + 'S')<<48))
#define SYNCPOINT_STARTCODE (0xE4ADEECA4569ULL + (((uint64_t)('N'<<8) + 'K')<<48))
#define     INDEX_STARTCODE (0xDD672F23E64EULL + (((uint64_t)('N'<<8) + 'X')<<48))
#define      INFO_STARTCODE (0xAB68B596BA78ULL + (((uint64_t)('N'<<8) + 'I')<<48))

#define MSB_CODED_FLAG 1
#define STREAM_CODED_FLAG 2
#define INVALID_FLAG 4

#define PREALLOC_SIZE 4096

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) > 0 ? (a) : -(a))

enum errors {
	ERR_GENERAL_ERROR = 1,
	ERR_BAD_VERSION = 2,
	ERR_NOT_FRAME_NOT_N = 3,
	ERR_BAD_CHECKSUM = 4,
	ERR_MAX_DISTANCE = 5,
	ERR_NO_HEADERS = 6,
	ERR_NOT_SEEKABLE = 7,
	ERR_OUT_OF_ORDER = 8,
	ERR_MAX_PTS_DISTANCE = 9,
	ERR_BAD_STREAM_ORDER = 11,
	ERR_NOSTREAM_STARTCODE = 12,
	ERR_BAD_EOF = 13,
};

typedef struct {
	nut_input_stream_t isc;
	int is_mem;
	uint8_t * buf;
	uint8_t * buf_ptr;
	int write_len; // memory allocated
	int read_len;  // data in memory
	off_t file_pos;
	off_t filesize;
} input_buffer_t;

typedef struct {
	nut_output_stream_t osc;
        int is_mem;
	uint8_t * buf;
	uint8_t * buf_ptr;
	int write_len; // memory allocated
	off_t file_pos;
} output_buffer_t;

typedef struct {
	int flags;
	int stream_flags;
	int stream_plus1;
	int pts_delta;
	int lsb;
	int mul;
	int reserved;
} frame_table_t;

typedef struct {
	off_t pos; // << 1, flag is "is pts correct" (in cache)
	uint64_t pts; // coded in '% stream_count'
	int back_ptr; // << 1, flag says if there is another syncpoint between this and next
} syncpoint_t;

typedef struct {
	int len;
	int alloc_len;
	syncpoint_t * s;
	uint64_t * pts; // each elem is stream_count items, +1 to real pts, 0 means there is no key
	uint64_t * eor; // same as pts, is the pts of last eor in syncpoint region _IF_ eor is set by syncpoint.
} syncpoint_list_t;

typedef struct {
	nut_packet_t p;
	uint8_t * buf;
	int64_t dts;
} reorder_packet_t;

typedef struct {
	uint64_t last_key; // muxer.c, re-set to 0 on every keyframe
	uint64_t last_pts;
	int64_t last_dts;
	int msb_pts_shift;
	int max_pts_distance;
	int decode_delay;
	nut_stream_header_t sh;
	int64_t * pts_cache;
	int64_t eor;
	// reorder.c
	int64_t next_pts;
	reorder_packet_t * packets;
	int num_packets;
	int64_t * reorder_pts_cache;
	// debug stuff
	int overhead;
	int tot_size;
	int total_frames;
} stream_context_t;

typedef struct {
	int tmp_flag;      // 1 => use msb, 2 => coded sflags, 4 => invalid
	int tmp_fields;
	int tmp_sflag;     // tmp_fields = 1
	int tmp_pts;       // tmp_fields = 2
	int tmp_mul;       // tmp_fields = 3
	int tmp_stream;    // tmp_fields = 4
	int tmp_size;      // tmp_fields = 5
	int count;         // tmp_fields = 7 (6 is reserved)
} frame_table_input_t;

struct nut_context_s {
	nut_muxer_opts_t mopts;
	nut_demuxer_opts_t dopts;
	input_buffer_t * i;
	output_buffer_t * o;
	output_buffer_t * tmp_buffer;

	int stream_count;
	stream_context_t * sc;

	int info_count;
	nut_info_packet_t * info;

	int max_distance;
	frame_table_input_t * fti;
	frame_table_t ft[256];

	off_t last_syncpoint; // for checking corruption and putting syncpoints, also for back_ptr
	off_t last_headers; // for header repetition and state for demuxer

	off_t before_seek; // position before any seek mess
	off_t seek_status;
	struct {
		off_t good_key;
		uint64_t old_last_pts;
	} * seek_state; // for linear search, so we can go back as if nothing happenned

	syncpoint_list_t syncpoints;

	// debug
	int sync_overhead;
};

static const struct { char * name, * type; } info_table [] = {
        {NULL                   ,  NULL }, // end
        {NULL                   ,  NULL },
        {NULL                   , "UTF8"},
        {NULL                   , "v"},
        {NULL                   , "s"},
        {"StreamId"             , "v"},
        {"Author"               , "UTF8"},
        {"Title"                , "UTF8"},
        {"Language"             , "UTF8"},
        {"Description"          , "UTF8"},
        {"Copyright"            , "UTF8"},
        {"Encoder"              , "UTF8"},
        {"Keyword"              , "UTF8"},
        {"Cover"                , "JPEG"},
        {"Cover"                , "PNG"},
        {"Disposition"          , "UTF8"},
};

static inline uint32_t crc32(uint8_t * buf, int len){
	uint32_t crc = 0;
	int i;
	while (len--) {
		crc ^= *buf++ << 24;
		for(i = 0; i < 8; i++) crc = (crc<<1) ^ (0x04C11DB7 & (crc>>31));
	}
	return crc;
}

static inline uint64_t convert_ts(nut_context_t * nut, uint64_t sn, int from, int to) {
	uint64_t ln, d1, d2;
	ln = (uint64_t)nut->sc[from].sh.timebase.nom * nut->sc[to].sh.timebase.den;
	d1 = nut->sc[from].sh.timebase.den;
	d2 = nut->sc[to].sh.timebase.nom;
	return (ln / d1 * sn + (ln%d1) * sn / d1) / d2;
}

static inline int compare_ts(nut_context_t * nut, uint64_t a, int at, uint64_t b, int bt) {
	if (convert_ts(nut, a, at, bt) < b) return -1;
	if (convert_ts(nut, b, bt, at) < a) return  1;
	return 0;
}

static inline int get_dts(int d, int64_t * pts_cache, int pts) {
	while (d--) {
		int64_t t = pts_cache[d];
		if (t < pts) {
			pts_cache[d] = pts;
			pts = t;
		}
	}
	return pts;
}

#define bctello(bc) ((bc)->file_pos + ((bc)->buf_ptr - (bc)->buf))

#define TO_PTS(prefix, pts) \
	int prefix##_s = (pts) % nut->stream_count; \
	uint64_t prefix##_p = (pts) / nut->stream_count;

#define TO_DOUBLE(stream, pts) ((double)(pts) / nut->sc[(stream)].sh.timebase.den * nut->sc[(stream)].sh.timebase.nom)

#define TO_DOUBLE_PTS(pts) ((double)((pts) / nut->stream_count) / nut->sc[(pts) % nut->stream_count].sh.timebase.den * nut->sc[(pts) % nut->stream_count].sh.timebase.nom)

#endif // _NUT_PRIV_H
