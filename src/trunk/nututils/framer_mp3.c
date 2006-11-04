// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include "nutmerge.h"

struct framer_priv_s {
	stream_t * stream;
};

static int get_packet(framer_priv_t * mp, packet_t * p) {
	return get_stream_packet(mp->stream, p);
}

static int setup_headers(framer_priv_t * mp, nut_stream_header_t * s) {
	*s = mp->stream->sh;
	return 0; // nothing to do
}

static framer_priv_t * init(stream_t * s) {
	framer_priv_t * mp = malloc(sizeof(framer_priv_t));
	mp->stream = s;
	return mp;
}

static void uninit(framer_priv_t * mp) {
	free(mp);
}

framer_t mp3_framer = {
	e_mp3,
	init,
	setup_headers,
	get_packet,
	uninit,
	NULL
};
