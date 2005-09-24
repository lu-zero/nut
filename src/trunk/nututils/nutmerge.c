#include <math.h>
#include "nutmerge.h"
#include "avireader.h"

FILE * stats = NULL;

int main(int argc, char * argv []) {
	FILE * in = NULL, * out = NULL;
	AVIContext * avi = NULL;
	nut_context_t * nut = NULL;
	nut_stream_header_t * nut_stream = NULL;
	nut_muxer_opts_t mopts;
	int err = 0;
	int i;
	int * pts = NULL;
	int ** frames = NULL;
	int * frames_pos = NULL;
	nut_packet_t p;
	if (argc < 3) { printf("bleh, more params you fool...\n"); return 1; }
	fprintf(stderr, "==============================================================\n");
	fprintf(stderr, "PLEASE NOTE THAT NUTMERGE FOR NOW CREATES _INVALID_ NUT FILES.\n");
	fprintf(stderr, "DO NOT USE THESE FILES FOR ANYTHING BUT TESTING LIBNUT.\n");
	fprintf(stderr, "==============================================================\n");
	if (!strcmp(argv[1], "-v") && argc > 4) {
		stats = fopen(argv[2], "w");
		argv += 2;
	}
	in = fopen(argv[1], "r");
	out = fopen(argv[2], "w");

	avi = init_avi(in);
	if ((err = avi_read_headers(avi))) goto err_out;
	nut_stream = nut_create_stream_context(avi);
	mopts.output = (nut_output_stream_t){ .priv = out, .write = NULL };
	// mopts.write_index = 0; // LETS SEE IF THIS WORKS!!! ### ### ###
	mopts.write_index = 1;
	nut = nut_muxer_init(&mopts, nut_stream);

	for (i = 0; nut_stream[i].type >= 0; i++);
	pts = calloc(i, sizeof(int));
	frames = calloc(i, sizeof(int*));
	frames_pos = calloc(i, sizeof(int));

	if (stats) fprintf(stats, "%10s%10s%10s%10s\n", "stream", "len", "pts_diff", "is_key");
	while (!(err = get_avi_packet(avi, &p))) {
		uint8_t buf[p.len];
		FREAD(in, p.len, buf);
		nut_write_frame_reorder(nut, &p, buf);
		frames[p.stream] = realloc(frames[p.stream], sizeof(int) * ++frames_pos[p.stream]);
		frames[p.stream][frames_pos[p.stream] - 1] = p.len;
		if (stats) fprintf(stats, "%10d%10d%10d%10d\n", p.stream, p.len, p.pts - pts[p.stream], p.is_key);
		pts[p.stream] = p.pts;
	}
	if (err == -1) err = 0;
err_out:
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
	free(pts);
	free(frames);
	free(frames_pos);
	nut_muxer_uninit_reorder(nut);
	uninit_avi(avi);
	free(nut_stream);
	fclose(in);
	fclose(out);
	if (stats) fclose(stats);
	return err;
}
