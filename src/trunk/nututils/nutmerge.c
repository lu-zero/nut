#include "nutmerge.h"
#include <math.h>
#include <string.h>

FILE * stats = NULL;

extern struct demuxer_t avi_demuxer;
extern struct demuxer_t nut_demuxer;

struct demuxer_t * demuxers[] = {
	&avi_demuxer,
	&nut_demuxer,
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
	int * pts = NULL;
	int ** frames = NULL;
	int * frames_pos = NULL;
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
	// mopts.write_index = 0; // LETS SEE IF THIS WORKS!!! ### ### ###
	mopts.write_index = 1;
	nut = nut_muxer_init(&mopts, nut_stream);

	for (i = 0; nut_stream[i].type >= 0; i++);
	pts = calloc(i, sizeof(int));
	frames = calloc(i, sizeof(int*));
	frames_pos = calloc(i, sizeof(int));

	if (stats) fprintf(stats, "%10s%10s%10s%10s\n", "stream", "len", "pts_diff", "is_key");
	while (!(err = demuxer->get_packet(demuxer_priv, &p, &buf))) {
		nut_write_frame_reorder(nut, &p, buf);
		frames[p.stream] = realloc(frames[p.stream], sizeof(int) * ++frames_pos[p.stream]);
		frames[p.stream][frames_pos[p.stream] - 1] = p.len;
		if (stats) fprintf(stats, "%10d%10d%10d%10d\n", p.stream, p.len, p.pts - pts[p.stream], p.is_key);
		pts[p.stream] = p.pts;
	}
	if (err == -1) err = 0;
err_out:
	nut_muxer_uninit_reorder(nut);
	if (frames) for (i = 0; nut_stream[i].type >= 0; i++) {
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
	free(nut_stream);
	free(pts);
	free(frames);
	free(frames_pos);
	if (demuxer) demuxer->uninit(demuxer_priv);
	if (in) fclose(in);
	if (out) fclose(out);
	if (stats) fclose(stats);
	return err;
}
