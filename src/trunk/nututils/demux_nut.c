#include "nutmerge.h"
#define ABS(x) MAX(x,-x)

static void * init(FILE * in) {
	return nut_demuxer_init(&(nut_demuxer_opts_t){ { in, NULL, NULL, NULL } , 0});
}

static void uninit(void * priv) {
	nut_demuxer_uninit(priv);
}

static int read_headers(void * priv, nut_stream_header_t ** nut_streams) {
	nut_packet_t tmp;
	int err;
	if ((err = nut_read_next_packet(priv, &tmp))) goto err_out;
	if (tmp.type != e_headers) return 2;
	err = nut_read_headers(priv, &tmp, nut_streams);
err_out:
	return err == 1 ? -1 : ABS(err);
}

static int get_packet(void * priv, nut_packet_t * p, uint8_t ** buf) {
	int err;
	int len;
	do {
		if ((err = nut_read_next_packet(priv, p))) goto err_out;
		if (p->type != e_frame) if ((err = nut_skip_packet(priv, &p->len))) goto err_out;
	} while (p->type != e_frame);
	*buf = malloc(len = p->len);
	err = nut_read_frame(priv, &len, *buf);
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
