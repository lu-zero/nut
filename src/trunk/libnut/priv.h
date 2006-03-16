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

#define NUT_API_FLAGS    3

#define FLAG_SIZE_MSB   32
#define FLAG_CHECKSUM   64
#define FLAG_CODED    4096
#define FLAG_INVALID  8192

#define STREAM_CODED_FLAG 2
#define INVALID_FLAG 4

#define PREALLOC_SIZE 4096

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) > 0 ? (a) : -(a))

enum errors {
	ERR_GENERAL_ERROR = 1,
	ERR_BAD_VERSION,
	ERR_NOT_FRAME_NOT_N,
	ERR_BAD_CHECKSUM,
	ERR_MAX_SYNCPOINT_DISTANCE,
	ERR_MAX_DISTANCE,
	ERR_NO_HEADERS,
	ERR_NOT_SEEKABLE,
	ERR_OUT_OF_ORDER,
	ERR_MAX_PTS_DISTANCE,
	ERR_VLC_TOO_LONG,
	ERR_BAD_STREAM_ORDER,
	ERR_NOSTREAM_STARTCODE,
	ERR_BAD_EOF,
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
	uint16_t flags;
	uint16_t mul;
	uint16_t lsb;
	int16_t pts_delta;
	uint8_t reserved;
	uint8_t stream_plus1;
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
	int timebase_id;
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

struct nut_context_s {
	nut_muxer_opts_t mopts;
	nut_demuxer_opts_t dopts;
	input_buffer_t * i;
	output_buffer_t * o;
	output_buffer_t * tmp_buffer;
	output_buffer_t * tmp_buffer2;

	int timebase_count;
	nut_timebase_t * tb;

	int stream_count;
	stream_context_t * sc;

	int info_count;
	nut_info_packet_t * info;

	int max_distance;
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

static inline uint32_t crc32(uint8_t * buf, int len){
	int32_t crc = 0;
	int i;
	while (len--) {
		crc ^= *buf++ << 24;
		for(i = 0; i < 8; i++) crc = (crc<<1) ^ (0x04C11DB7 & (crc>>31));
	}
	return crc;
}

static inline uint64_t convert_ts(nut_context_t * nut, uint64_t sn, nut_timebase_t from, nut_timebase_t to) {
	uint64_t ln, d1, d2;
	ln = (uint64_t)from.nom * to.den;
	d1 = from.den;
	d2 = to.nom;
	return (ln / d1 * sn + (ln%d1) * sn / d1) / d2;
}

static inline int compare_ts(nut_context_t * nut, uint64_t a, nut_timebase_t at, uint64_t b, nut_timebase_t bt) {
	if (convert_ts(nut, a, at, bt) < b) return -1;
	if (convert_ts(nut, b, bt, at) < a) return  1;
	return 0;
}

static inline int64_t get_dts(int d, int64_t * pts_cache, int64_t pts) {
	while (d--) {
		int64_t t = pts_cache[d];
		if (t < pts) {
			pts_cache[d] = pts;
			pts = t;
		}
	}
	return pts;
}

static inline int64_t peek_dts(int d, int64_t * pts_cache, int64_t pts) {
	while (d--) if (pts_cache[d] < pts) pts = pts_cache[d];
	return pts;
}

#define bctello(bc) ((bc)->file_pos + ((bc)->buf_ptr - (bc)->buf))

#define TO_PTS(prefix, pts) \
	int prefix##_t = (pts) % nut->timebase_count; \
	uint64_t prefix##_p = (pts) / nut->timebase_count;

#define TO_DOUBLE(t, pts) ((double)(pts) / nut->tb[t].den * nut->tb[t].nom)

#define TO_DOUBLE_PTS(pts) ((double)((pts) / nut->timebase_count) / nut->tb[(pts) % nut->timebase_count].den * nut->tb[(pts) % nut->timebase_count].nom)

#define TO_TB(i) nut->tb[nut->sc[i].timebase_id]

#endif // _NUT_PRIV_H
