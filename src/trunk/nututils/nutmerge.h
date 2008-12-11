// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#ifndef NUTUTILS_NUTMERGE_H
#define NUTUTILS_NUTMERGE_H

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
//#define NDEBUG
#include <assert.h>
#include <libnut.h>

#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX(a,b) ((a) < (b) ? (b) : (a))

extern FILE * stats;

typedef struct demuxer_priv_s demuxer_priv_tt;
typedef struct framer_priv_s framer_priv_tt;
typedef struct stream_s stream_tt;

enum nutmerge_codecs {
	e_vorbis,
	e_mpeg4,
	e_mp3,
};

typedef struct {
	nut_packet_tt p;
	uint8_t * buf; // the demuxer mallocs this, nutmerge (or framer) eventually frees it
} packet_tt;

typedef struct {
	char * extension;
	demuxer_priv_tt * (*init)(FILE * in);
	/// streams is -1 terminated, handled and freed by demuxer
	int (*read_headers)(demuxer_priv_tt * priv, stream_tt ** streams);
	int (*fill_buffer)(demuxer_priv_tt * priv);
	void (*uninit)(demuxer_priv_tt * priv);
	demuxer_priv_tt * priv;
} demuxer_tt;

typedef struct {
	enum nutmerge_codecs codec_id;
	framer_priv_tt * (*init)(stream_tt * stream);
	int (*setup_headers)(framer_priv_tt * priv, nut_stream_header_tt * s); // fill 's'
	int (*get_packet)(framer_priv_tt * priv, packet_tt * p); // 'p->buf' is now controlled by caller
	void (*uninit)(framer_priv_tt * priv);
	framer_priv_tt * priv;
} framer_tt;

struct stream_s {
	int stream_id; // -1 terminated
	demuxer_tt demuxer;
	enum nutmerge_codecs codec_id;

	nut_stream_header_tt sh;

	int npackets;
	int packets_alloc;
	packet_tt * packets;
};

void ready_stream(stream_tt * streams); // setup default stream info

void push_packet(stream_tt * stream, packet_tt * p);

int peek_stream_packet(stream_tt * stream, packet_tt * p, int n); // n = 0 means next packet, n = 1 means 1 after that
int get_stream_packet(stream_tt * stream, packet_tt * p);

void free_streams(stream_tt * streams); // all the way to -1 terminated list, not the actual 'streams' though

enum nutmerge_errors {
	err_unexpected_eof = 1,
	err_avi_bad_riff,
	err_avi_bad_avi,
	err_avi_no_avih,
	err_avi_no_strl,
	err_avi_no_strh,
	err_avi_no_strf,
	err_avi_no_hdrl,
	err_avi_no_idx,
	err_avi_no_movi,
	err_avi_bad_avih_len,
	err_avi_bad_strh_len,
	err_avi_bad_vids_len,
	err_avi_bad_auds_len,
	err_avi_bad_strf_type,
	err_avi_stream_overflow,
	err_avi_no_video_codec,
	err_avi_no_audio_codec,
	err_avi_bad_packet,
	err_mpeg4_no_frame_type,
	err_mp3_bad_packet,
	err_bad_oggs_magic,
	err_ogg_no_codec,
	err_ogg_non_interleaved,
	err_vorbis_header,
	err_vorbis_packet,
};

#endif // NUTUTILS_NUTMERGE_H
