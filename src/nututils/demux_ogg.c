// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "nutmerge.h"
#define FREAD(file, len, var) do { if (fread((var), 1, (len), (file)) != (len)) return -1; }while(0)

typedef struct ogg_stream_s ogg_stream_tt;

struct ogg_stream_s {
	int serial;
	int leftover; // buffer for "next page" nonsense. always reallocated
	uint8_t * leftover_buf;
};

struct demuxer_priv_s {
	FILE * in;
	ogg_stream_tt * os;
	stream_tt * s;
	int nstreams;
	int stream_count; // when nutmerge_streams was handed to the API ...
};

static struct { enum nutmerge_codecs id; uint8_t * magic; int magic_len; } codecs[] = {
       { e_vorbis, (uint8_t*)"\001vorbis", 7 },
       { 0, NULL, 0 }
};

static int find_stream(demuxer_priv_tt * ogg, int serial) {
	extern demuxer_tt ogg_demuxer;
	int i;
	for (i = 0; i < ogg->nstreams; i++) {
		if (ogg->os[i].serial == serial) return i;
	}
	ogg->os = realloc(ogg->os, sizeof(ogg_stream_tt) * ++ogg->nstreams);
	ogg->s = realloc(ogg->s, sizeof(stream_tt) * (ogg->nstreams+1));
	ogg->os[i].serial = serial;
	ogg->os[i].leftover_buf = NULL;
	ogg->os[i].leftover = 0;
	ogg->s[i].stream_id = i;
	ogg->s[i].demuxer = ogg_demuxer;
	ogg->s[i].demuxer.priv = ogg;
	ogg->s[i].packets_alloc = ogg->s[i].npackets = 0;
	ogg->s[i].packets = NULL;
	ogg->s[i+1].stream_id = -1;
	return i;
}

static int read_page(demuxer_priv_tt * ogg, int * stream) {
	ogg_stream_tt * os;
	int i, serial, segments;
	char tmp_header[27];
	uint8_t sizes[256];
	int len, tot = 0;

	FREAD(ogg->in, 27, tmp_header);
	if (strncmp(tmp_header, "OggS", 5)) return err_bad_oggs_magic; // including version

	serial = (tmp_header[14] << 24) |
		 (tmp_header[15] << 16) |
		 (tmp_header[16] <<  8) |
		 (tmp_header[17]      );
	*stream = find_stream(ogg, serial);
	os = &ogg->os[*stream];

	segments = tmp_header[26];
	FREAD(ogg->in, segments, sizes);

	len = os->leftover;
	for (i = 0; i < segments; i++) {
		len += sizes[i];
		if (sizes[i] != 255) {
			packet_tt p;
			p.buf = malloc(len);
			p.p.len = len;
			p.p.stream = *stream;
			p.p.flags = p.p.pts = p.p.next_pts = 0; // Ogg sucks
			memcpy(p.buf, os->leftover_buf, os->leftover);
			FREAD(ogg->in, len - os->leftover, p.buf + os->leftover);
			push_packet(&ogg->s[*stream], &p);
			tot++;
			len = 0;
			os->leftover = 0;
		}
	}

	if (len) {
		os->leftover_buf = realloc(os->leftover_buf, len);
		FREAD(ogg->in, len - os->leftover, os->leftover_buf + os->leftover);
		os->leftover = len;
	}
	if (!tot) return read_page(ogg, stream); // this page gave nothing, move on to next page

	return 0;
}

static int read_headers(demuxer_priv_tt * ogg, stream_tt ** streams) {
	int i;
	int err;

	if ((err = read_page(ogg, &i))) return err;
	do { if ((err = read_page(ogg, &i))) return err; } while (i != 0);
	ogg->stream_count = ogg->nstreams;

	for (i = 0; i < ogg->stream_count; i++) {
		int j;
		for (j = 0; codecs[j].magic; j++) {
			if (codecs[j].magic_len > ogg->s[i].packets[0].p.len) continue;
			if (!memcmp(codecs[j].magic, ogg->s[i].packets[0].buf, codecs[j].magic_len)) break;
		}
		if (!codecs[j].magic) return err_ogg_no_codec;
		ogg->s[i].codec_id = codecs[j].id;
	}

	*streams = ogg->s;

	return 0;
}

static int fill_buffer(demuxer_priv_tt * ogg) {
	int err, dummy;

	if ((err = read_page(ogg, &dummy))) return err;
	if (ogg->stream_count != ogg->nstreams) return err_ogg_non_interleaved;

	return 0;
}

static demuxer_priv_tt * init(FILE * in) {
	demuxer_priv_tt * ogg = malloc(sizeof(demuxer_priv_tt));
	ogg->in = in;
	ogg->os = NULL;
	ogg->s = NULL;
	ogg->stream_count = ogg->nstreams = 0;
	return ogg;
}

static void uninit(demuxer_priv_tt * ogg) {
	int i;
	for (i = 0; i < ogg->nstreams; i++) free(ogg->os[i].leftover_buf);
	free(ogg->os);
	free_streams(ogg->s);
	free(ogg->s);
	free(ogg);
}

demuxer_tt ogg_demuxer = {
	"ogg",
	init,
	read_headers,
	fill_buffer,
	uninit,
	NULL
};
