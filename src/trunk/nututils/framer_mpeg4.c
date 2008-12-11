// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include "nutmerge.h"

struct framer_priv_s {
	stream_tt * stream;
	int64_t cur_pts;
};

static int find_frame_type(int len, uint8_t * buf, int * type) {
	if (!len) return err_mpeg4_no_frame_type;
	while (--len) { // not including last byte
		if (*buf++ != 0xB6) continue;
		*type = *buf >> 6;
		return 0;
	}
	return err_mpeg4_no_frame_type;
}

#define CHECK(x) do{ if ((err = (x))) return err; }while(0)

static int get_packet(framer_priv_tt * mc, packet_tt * p) {
	packet_tt tmp_p;
	int n = 0;
	int type, err = 0;

	CHECK(get_stream_packet(mc->stream, p));

	p->p.pts = mc->cur_pts++;
	p->p.next_pts = 0;

	CHECK(find_frame_type(p->p.len, p->buf, &type));

	if (stats) fprintf(stats, "%c", type==0?'I':type==1?'P':type==2?'B':'S');

	if (!(p->p.flags & NUT_FLAG_KEY) ^ (type != 0)) printf("Error detected stream %d frame %d\n", p->p.stream, (int)p->p.pts);
	p->p.flags |= (type == 0 ? NUT_FLAG_KEY : 0);

	if (type == 2) { p->p.pts--; return 0; } // B-frame, simple
	if (type == 3) printf("S-Frame %d\n", (int)p->p.pts);

	// I, P or S, needs forward B-frame check

	while ((err = peek_stream_packet(mc->stream, &tmp_p, n++)) != -1) {
		if (err) return err;
		CHECK(find_frame_type(tmp_p.p.len, tmp_p.buf, &type));
		if (type != 2) break; // Not a B-frame, we're done.
		p->p.pts++;
	}

	return 0;
}

static int setup_headers(framer_priv_tt * mc, nut_stream_header_tt * s) {
	*s = mc->stream->sh;
	return 0; // nothing to do
}

static framer_priv_tt * init(stream_tt * s) {
	framer_priv_tt * mc = malloc(sizeof(framer_priv_tt));
	mc->stream = s;
	mc->cur_pts = 0;
	return mc;
}

static void uninit(framer_priv_tt * mc) {
	free(mc);
}

framer_tt mpeg4_framer = {
	e_mpeg4,
	init,
	setup_headers,
	get_packet,
	uninit,
	NULL
};
