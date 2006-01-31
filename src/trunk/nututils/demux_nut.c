#include "nutmerge.h"
#define ABS(x) MAX(x,-x)

typedef struct nutmerge_nut_s {
	nut_context_t * nut;
	uint8_t * buf;
} nutmerge_nut_t;

static void * init(FILE * in) {
	nutmerge_nut_t * nut = malloc(sizeof(nutmerge_nut_t));
	nut_demuxer_opts_t dopts = { { in, NULL, NULL, NULL, 0 } , 0};
	nut->nut = nut_demuxer_init(&dopts);
	nut->buf = NULL;
	return nut;
}

static void uninit(void * priv) {
	nutmerge_nut_t * nut = priv;
	nut_demuxer_uninit(nut->nut);
	free(nut->buf);
	free(nut);
}

static int read_headers(void * priv, nut_stream_header_t ** nut_streams) {
	nutmerge_nut_t * nut = priv;
	nut_packet_t tmp;
	int err;
	if ((err = nut_read_next_packet(nut->nut, &tmp))) goto err_out;
	if (tmp.type != e_headers) return 2;
	err = nut_read_headers(nut->nut, &tmp, nut_streams);
err_out:
	return err == 1 ? -1 : ABS(err);
}

static int get_packet(void * priv, nut_packet_t * p, uint8_t ** buf) {
	nutmerge_nut_t * nut = priv;
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
