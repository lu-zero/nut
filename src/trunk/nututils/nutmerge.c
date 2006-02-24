#include "nutmerge.h"
#include <math.h>
#include <string.h>

FILE * stats = NULL;

extern struct demuxer_t avi_demuxer;
extern struct demuxer_t nut_demuxer;
extern struct demuxer_t ogg_demuxer;

struct demuxer_t * demuxers[] = {
	&avi_demuxer,
	&nut_demuxer,
	&ogg_demuxer,
	NULL
};

int main(int argc, char * argv []) {
	FILE * in = NULL, * out = NULL;
	struct demuxer_t * demuxer = NULL;
	void * demuxer_priv = NULL;
	nut_context_t * nut = NULL;
	nut_stream_header_t * nut_stream = NULL;
	nut_muxer_opts_t mopts;
	uint8_t * buf;
	int err = 0;
	int i;
	nut_packet_t p;
	const char * extention;
	fprintf(stderr, "==============================================================\n");
	fprintf(stderr, "PLEASE NOTE THAT NUTMERGE FOR NOW CREATES _INVALID_ NUT FILES.\n");
	fprintf(stderr, "DO NOT USE THESE FILES FOR ANYTHING BUT TESTING LIBNUT.\n");
	fprintf(stderr, "==============================================================\n");
	if (argc > 4 && !strcmp(argv[1], "-v")) {
		stats = fopen(argv[2], "w");
		argv += 2;
		argc -= 2;
	}
	if (argc < 3) { printf("bleh, more params you fool...\n"); return 1; }
	extention = argv[1];
	for (i = 0; argv[1][i]; i++) if (argv[1][i] == '.') extention = &argv[1][i+1];
	for (i = 0; demuxers[i]; i++)
		if (!strcmp(demuxers[i]->extention, extention)) demuxer = demuxers[i];

	if (!demuxer) {
		printf("unsupported file format\n");
		err = 1;
		goto err_out;
	}

	in = fopen(argv[1], "r");
	out = fopen(argv[2], "w");

	demuxer_priv = demuxer->init(in);

	if ((err = demuxer->read_headers(demuxer_priv, &nut_stream))) goto err_out;
	mopts.output = (nut_output_stream_t){ .priv = out, .write = NULL };
	mopts.write_index = 1;
	nut = nut_muxer_init(&mopts, nut_stream, NULL);

	for (i = 0; nut_stream[i].type >= 0; i++);

	{
	int pts[i];
	int * frames[i];
	int frames_pos[i];
	int frames_alloc[i];

	for (i = 0; nut_stream[i].type >= 0; i++) {
		frames_alloc[i] = frames_pos[i] = pts[i] = 0;
		frames[i] = NULL;
	}

	if (stats) fprintf(stats, "%10s%10s%10s%10s\n", "stream", "len", "pts_diff", "flags");
	while (!(err = demuxer->get_packet(demuxer_priv, &p, &buf))) {
		int s = p.stream;
		nut_write_frame_reorder(nut, &p, buf);
		if (++frames_pos[s] > frames_alloc[s]) {
			frames_alloc[s] = frames_pos[s] + 4096;
			frames[s] = realloc(frames[s], sizeof(int) * frames_alloc[s]);
		}
		frames[s][frames_pos[s] - 1] = p.len;
		if (stats) fprintf(stats, "%10d%10d%10d%10d\n", p.stream, p.len, (int)p.pts - pts[s], p.flags);
		pts[s] = p.pts;
		if (!(p.pts % 5000)) {
			for (i = 0; nut_stream[i].type >= 0; i++) {
				fprintf(stderr, "%d: %5d ", i, frames_pos[i]);
			}
			fprintf(stderr, "\r");
		}
	}
	if (err == -1) err = 0;
	nut_muxer_uninit_reorder(nut);
	nut = NULL;
	for (i = 0; nut_stream[i].type >= 0; i++) {
		int j;
		uint64_t total = 0;
		double avg;
		double std = 0;
		for (j = 0; j < frames_pos[i]; j++) total += frames[i][j];
		avg = (double)total / frames_pos[i];
		for (j = 0; j < frames_pos[i]; j++) {
			std += (frames[i][j] - avg)*(frames[i][j] - avg);
		}
		std /= frames_pos[i];
		std = sqrt(std);
		fprintf(stderr, "Stream %d: Standard Deviation %.2lf\n", i, std);
		free(frames[i]);
	}
	}
err_out:
	nut_muxer_uninit_reorder(nut);
	free(nut_stream);
	if (demuxer) demuxer->uninit(demuxer_priv);
	if (in) fclose(in);
	if (out) fclose(out);
	if (stats) fclose(stats);
	return err;
}
