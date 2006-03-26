#include "nutmerge.h"
#define ABS(x) MAX(x,-x)

struct demuxer_priv_s {
	nut_context_t * nut;
	uint8_t * buf;
};

static void * init(FILE * in) {
	demuxer_priv_t * nut = malloc(sizeof(demuxer_priv_t));
	nut_demuxer_opts_t dopts = { { in, NULL, NULL, NULL, 0 } , 0};
	nut->nut = nut_demuxer_init(&dopts);
	nut->buf = NULL;
	return nut;
}

static void uninit(demuxer_priv_t * nut) {
	nut_demuxer_uninit(nut->nut);
	free(nut->buf);
	free(nut);
}

static int read_headers(demuxer_priv_t * nut, nut_stream_header_t ** nut_streams) {
	int err = nut_read_headers(nut->nut, nut_streams);
	return err == 1 ? -1 : ABS(err);
}

static int get_packet(demuxer_priv_t * nut, nut_packet_t * p, uint8_t ** buf) {
	int err;
	int len;
	do {
		if ((err = nut_read_next_packet(nut->nut, p))) goto err_out;
		if (p->type != e_frame) if ((err = nut_skip_packet(nut->nut, &p->len))) goto err_out;
	} while (p->type != e_frame);
	*buf = nut->buf = realloc(nut->buf, len = p->len);
	err = nut_read_frame(nut->nut, &len, *buf);
err_out:
	return err == 1 ? -1 : ABS(err);
}

struct demuxer_t nut_demuxer = {
	"nut",
	init,
	read_headers,
	get_packet,
	uninit
};
