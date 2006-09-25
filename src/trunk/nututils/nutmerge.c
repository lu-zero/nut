// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include "nutmerge.h"
#include <math.h>
#include <string.h>

FILE * stats = NULL;

extern demuxer_t avi_demuxer;
extern demuxer_t nut_demuxer;
extern demuxer_t ogg_demuxer;

demuxer_t * ndemuxers[] = {
	&avi_demuxer,
	//&nut_demuxer,
	&ogg_demuxer,
	NULL
};

extern framer_t vorbis_framer;
extern framer_t mpeg4_framer;
extern framer_t null_framer;

framer_t * nframers[] = {
	&vorbis_framer,
	&mpeg4_framer,
	&null_framer,
	NULL
};

frame_table_input_t ft_default[] = {
	// There must be atleast this safety net:
	//{ 4128,      3,   0,   1,      0,    0,     0 },
	//{ flag, fields, pts, mul, stream, size, count }
	  { 8192,      0,   0,   1,      0,    0,     0 }, // invalid 0x00
	  {   56,      0,   0,   1,      0,    0,     0 }, // safety net non key frame
	  {   56,      0,   0,   1,      0,    0,     0 }, // safety net key frame
	  { 4128,      0,   0,   1,      0,    0,     0 }, // one more safety net
	  {   27,      0,   0,   1,      0,    0,     0 }, // EOR frame
	  {    1,      4,   1, 337,      1,  336,     0 }, // used 82427 times
	  {    1,      4,   1, 385,      1,  384,     0 }, // used 56044 times
	  {    0,      4,   2,   7,      0,    6,     0 }, // used 20993 times
	  {    0,      4,   1,   7,      0,    6,     0 }, // used 10398 times
	  {    1,      4,   1, 481,      1,  480,     0 }, // used 3527 times
	  {    1,      4,   1, 289,      1,  288,     0 }, // used 2042 times
	  {    1,      4,   1, 577,      1,  576,     0 }, // used 1480 times
	  {    1,      4,   1, 673,      1,  672,     0 }, // used 862 times
	  {    1,      4,   1, 769,      1,  768,     0 }, // used 433 times
	  {    1,      4,   1, 961,      1,  960,     0 }, // used 191 times
	  {   32,      3,   2, 101,      0,    0,     0 }, // "1.2.0" => 14187
	  {   32,      3,  -1,  40,      0,    0,     0 }, // "1.-1.0" => 5707
	  {   32,      3,   1,  81,      0,    0,     0 }, // "1.1.0" => 11159
	  {   33,      3,   1,  11,      0,    0,     0 }, // "1.1.1" => 1409
	  {  105,      3,   0,   6,      0,    0,     0 }, // checksum for video
	  { 8192,      2,   0,   1,      0,    0,     0 }, // invalid 0xFF
	  {   -1,      0,   0,   0,      0,    0,     0 }, // end
};

void push_packet(stream_t * stream, packet_t * p) {
	if (stream->npackets == stream->packets_alloc) {
		stream->packets_alloc += 20;
		stream->packets = realloc(stream->packets, stream->packets_alloc * sizeof(packet_t));
	}
	stream->packets[stream->npackets++] = *p;
}

int peek_stream_packet(stream_t * stream, packet_t * p, int n) {
	while (stream->npackets <= n) {
		int err;
		if ((err = stream->demuxer.fill_buffer(stream->demuxer.priv))) return err;
	}
	*p = stream->packets[n];
	return 0;
}

int get_stream_packet(stream_t * stream, packet_t * p) {
	while (!stream->npackets) {
		int err;
		if ((err = stream->demuxer.fill_buffer(stream->demuxer.priv))) return err;
	}
	*p = stream->packets[0];
	stream->npackets--;
	memmove(&stream->packets[0], &stream->packets[1], stream->npackets * sizeof(packet_t));
	return 0;
}

void free_streams(stream_t * streams) {
	int i;
	if (!streams) return;
	for (i = 0; streams[i].stream_id >= 0; i++) {
		int j;
		for (j = 0; j < streams[i].npackets; j++) free(streams[i].packets[j].buf);
		free(streams[i].packets);
	}
}

static int pick(stream_t * streams, int stream_count) {
	int i, n = 0;
	for (i = 1; i < stream_count; i++) if (streams[i].npackets > streams[n].npackets) n = i;
	return n;
}

#define fget_packet(framer, p) (framer).get_packet((framer).priv, p)

static int convert(FILE * out, demuxer_t * demuxer, stream_t * streams, int stream_count) {
	nut_context_t * nut = NULL;
	nut_stream_header_t nut_stream[stream_count+1];
	nut_muxer_opts_t mopts;
	framer_t framers[stream_count];
	int i, err = 0;
	packet_t p;
	int pts[stream_count];

	memset(framers, 0, sizeof framers);
	memset(pts, 0, sizeof pts);

	for (i = 0; i < stream_count; i++) {
		int j;
		for (j = 0; nframers[j]; j++) if (nframers[j]->codec_id == streams[i].codec_id) break;
		if (!nframers[j]) fprintf(stderr, "unsupported file format\n");
		framers[i] = *nframers[j];
		framers[i].priv = framers[i].init(&streams[i]);
		if ((err = framers[i].setup_headers(framers[i].priv, &nut_stream[i]))) goto err_out;
	}
	nut_stream[i].type = -1;

	mopts.output = (nut_output_stream_t){ .priv = out, .write = NULL };
	mopts.write_index = 1;
	mopts.fti = ft_default;
	mopts.max_distance = 32768;
	mopts.alloc.malloc = NULL;
	nut = nut_muxer_init(&mopts, nut_stream, NULL);

	if (stats) fprintf(stats, "%10s%10s%10s%10s\n", "stream", "len", "pts_diff", "flags");
	while (!(err = fget_packet(framers[pick(streams, stream_count)], &p))) {
		int s = p.p.stream;
		nut_write_frame_reorder(nut, &p.p, p.buf);
		if (stats) fprintf(stats, "%10d%10d%10d%10d\n", p.p.stream, p.p.len, (int)p.p.pts - pts[s], p.p.flags);
		pts[s] = p.p.pts;
		if (!(p.p.pts % 5000)) {
			for (i = 0; i < stream_count; i++) fprintf(stderr, "%d: %5d ", i, pts[i]);
			fprintf(stderr, "\r");
		}
		free(p.buf);
	}
	if (err == -1) err = 0;
err_out:
	nut_muxer_uninit_reorder(nut);
	for (i = 0; i < stream_count; i++) if (framers[i].priv) framers[i].uninit(framers[i].priv);
	return err;
}

int main(int argc, char * argv []) {
	FILE * in = NULL, * out = NULL;
	demuxer_t demuxer = { .priv = NULL };
	stream_t * streams;
	int i, err = 0;
	const char * extension;

	fprintf(stderr, "==============================================================\n");
	fprintf(stderr, "PLEASE NOTE THAT NUTMERGE FOR NOW CREATES _INVALID_ NUT FILES.\n");
	fprintf(stderr, "DO NOT USE THESE FILES FOR ANYTHING BUT TESTING LIBNUT.\n");
	fprintf(stderr, "==============================================================\n");

	if (argc > 4 && !strcmp(argv[1], "-v")) {
		stats = fopen(argv[2], "w");
		argv += 2;
		argc -= 2;
	}
	if (argc < 3) { fprintf(stderr, "bleh, more params you fool...\n"); return 1; }
	extension = argv[1];
	for (i = 0; argv[1][i]; i++) if (argv[1][i] == '.') extension = &argv[1][i+1];
	for (i = 0; ndemuxers[i]; i++) if (!strcmp(ndemuxers[i]->extension, extension)) break;
	if (!ndemuxers[i]) {
		fprintf(stderr, "unsupported file format\n");
		err = 1;
		goto err_out;
	}

	demuxer = *ndemuxers[i];

	in = fopen(argv[1], "rb");
	out = fopen(argv[2], "wb");

	demuxer.priv = demuxer.init(in);
	if ((err = demuxer.read_headers(demuxer.priv, &streams))) goto err_out;

	for (i = 0; streams[i].stream_id >= 0; i++);

	err = convert(out, &demuxer, streams, i);

err_out:
	if (demuxer.priv) demuxer.uninit(demuxer.priv);
	if (in) fclose(in);
	if (out) fclose(out);
	if (stats) fclose(stats);
	return err;
}
