// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include "nutmerge.h"
#include <string.h>

FILE * stats = NULL;

extern demuxer_tt avi_demuxer;
extern demuxer_tt nut_demuxer;
extern demuxer_tt ogg_demuxer;

demuxer_tt * ndemuxers[] = {
	&avi_demuxer,
	//&nut_demuxer,
	&ogg_demuxer,
	NULL
};

extern framer_tt vorbis_framer;
extern framer_tt mpeg4_framer;
extern framer_tt mp3_framer;

framer_tt * nframers[] = {
	&vorbis_framer,
	&mpeg4_framer,
	&mp3_framer,
	NULL
};

void push_packet(stream_tt * stream, packet_tt * p) {
	if (stream->npackets == stream->packets_alloc) {
		stream->packets_alloc += 20;
		stream->packets = realloc(stream->packets, stream->packets_alloc * sizeof(packet_tt));
	}
	stream->packets[stream->npackets++] = *p;
}

int peek_stream_packet(stream_tt * stream, packet_tt * p, int n) {
	while (stream->npackets <= n) {
		int err;
		if ((err = stream->demuxer.fill_buffer(stream->demuxer.priv))) return err;
	}
	*p = stream->packets[n];
	return 0;
}

int get_stream_packet(stream_tt * stream, packet_tt * p) {
	while (!stream->npackets) {
		int err;
		if ((err = stream->demuxer.fill_buffer(stream->demuxer.priv))) return err;
	}
	*p = stream->packets[0];
	stream->npackets--;
	memmove(&stream->packets[0], &stream->packets[1], stream->npackets * sizeof(packet_tt));
	return 0;
}

void free_streams(stream_tt * streams) {
	int i;
	if (!streams) return;
	for (i = 0; streams[i].stream_id >= 0; i++) {
		int j;
		for (j = 0; j < streams[i].npackets; j++) free(streams[i].packets[j].buf);
		free(streams[i].packets);
	}
}

static int pick(stream_tt * streams, int stream_count) {
	int i, n = 0;
	for (i = 1; i < stream_count; i++) if (streams[i].npackets > streams[n].npackets) n = i;
	return n;
}

#define fget_packet(framer, p) (framer).get_packet((framer).priv, p)

static int convert(FILE * out, demuxer_tt * demuxer, stream_tt * streams, int stream_count) {
	nut_context_tt * nut = NULL;
	nut_stream_header_tt nut_stream[stream_count+1];
	nut_muxer_opts_tt mopts;
	framer_tt framers[stream_count];
	int i, err = 0;
	packet_tt p;
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

	mopts.output = (nut_output_stream_tt){ .priv = out, .write = NULL };
	mopts.write_index = 1;
	mopts.realtime_stream = 0;
	mopts.fti = NULL;
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
	demuxer_tt demuxer = { .priv = NULL };
	stream_tt * streams;
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
	if (argc < 3) { fprintf(stderr, "usage: %s source.avi output.nut\n", argv[0]); return 1; }
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
	if (err) switch ((enum nutmerge_errors)err) {
#define ERR_CASE(a) case a: fprintf(stderr, "%s\n", #a); break;
		ERR_CASE(err_unexpected_eof)
		ERR_CASE(err_avi_bad_riff)
		ERR_CASE(err_avi_bad_avi)
		ERR_CASE(err_avi_no_avih)
		ERR_CASE(err_avi_no_strl)
		ERR_CASE(err_avi_no_strh)
		ERR_CASE(err_avi_no_strf)
		ERR_CASE(err_avi_no_hdrl)
		ERR_CASE(err_avi_no_idx)
		ERR_CASE(err_avi_no_movi)
		ERR_CASE(err_avi_bad_avih_len)
		ERR_CASE(err_avi_bad_strh_len)
		ERR_CASE(err_avi_bad_vids_len)
		ERR_CASE(err_avi_bad_auds_len)
		ERR_CASE(err_avi_bad_strf_type)
		ERR_CASE(err_avi_stream_overflow)
		ERR_CASE(err_avi_no_video_codec)
		ERR_CASE(err_avi_no_audio_codec)
		ERR_CASE(err_avi_bad_packet)
		ERR_CASE(err_mpeg4_no_frame_type)
		ERR_CASE(err_mp3_bad_packet)
		ERR_CASE(err_bad_oggs_magic)
		ERR_CASE(err_ogg_no_codec)
		ERR_CASE(err_ogg_non_interleaved)
		ERR_CASE(err_vorbis_header)
		ERR_CASE(err_vorbis_packet)
#undef ERR_CASE
	}
	return err;
}
