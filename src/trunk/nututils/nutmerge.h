// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see LICENSE

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
//#define NDEBUG
#include <assert.h>
#include <nut.h>

#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX(a,b) ((a) < (b) ? (b) : (a))

extern FILE * stats;

typedef struct demuxer_priv_s demuxer_priv_t;
typedef struct framer_priv_s framer_priv_t;
typedef struct stream_s stream_t;

enum nutmerge_codecs {
	e_vorbis,
	e_mpeg4,
	e_null,
};

typedef struct {
	nut_packet_t p;
	uint8_t * buf; // the demuxer mallocs this, nutmerge (or framer) evantually frees it
} packet_t;

typedef struct {
	char * extension;
	demuxer_priv_t * (*init)(FILE * in);
	/// streams is -1 terminated, handled and freed by demuxer
	int (*read_headers)(demuxer_priv_t * priv, stream_t ** streams);
	int (*fill_buffer)(demuxer_priv_t * priv);
	void (*uninit)(demuxer_priv_t * priv);
	demuxer_priv_t * priv;
} demuxer_t;

typedef struct {
	enum nutmerge_codecs codec_id;
	framer_priv_t * (*init)(stream_t * stream);
	int (*setup_headers)(framer_priv_t * priv, nut_stream_header_t * s); // fill 's'
	int (*get_packet)(framer_priv_t * priv, packet_t * p); // 'p->buf' is now controlled by caller
	void (*uninit)(framer_priv_t * priv);
	framer_priv_t * priv;
} framer_t;

struct stream_s {
	int stream_id; // -1 terminated
	demuxer_t demuxer;
	enum nutmerge_codecs codec_id;

	nut_stream_header_t sh;

	int npackets;
	int packets_alloc;
	packet_t * packets;
};

void ready_stream(stream_t * streams); // setup default stream info

void push_packet(stream_t * stream, packet_t * p);

int peek_stream_packet(stream_t * stream, packet_t * p, int n); // n = 0 means next packet, n = 1 means 1 after that
int get_stream_packet(stream_t * stream, packet_t * p);

void free_streams(stream_t * streams); // all the way to -1 terminated list, not the actual 'streams' though
