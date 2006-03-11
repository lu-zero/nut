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

frame_table_input_t ft_default[] = {
	// There must be atleast this safety net:
	//{    3,      3,     0,   0,   1,      0,    0,     0 },
	//{ flag, fields, sflag, pts, mul, stream, size, count }
	  {    4,      0,     0,   0,   1,      0,    0,     0 }, // invalid 0x00
	  {    1,      1,     1,   0,   1,      0,    0,     0 }, // safety net non key frame
	  {    1,      0,     0,   0,   1,      0,    0,     0 }, // safety net key frame
	  {    3,      0,     0,   0,   1,      0,    0,     0 }, // one more safety net
	  {    0,      5,     1,   1, 337,      2,  336,     0 }, // used 82427 times
	  {    0,      5,     1,   1, 385,      2,  384,     0 }, // used 56044 times
	  {    0,      5,     0,   2,   7,      1,    6,     0 }, // used 20993 times
	  {    0,      5,     0,   1,   7,      1,    6,     0 }, // used 10398 times
	  {    0,      5,     1,   1, 481,      2,  480,     0 }, // used 3527 times
	  {    0,      5,     1,   1, 289,      2,  288,     0 }, // used 2042 times
	  {    0,      5,     1,   1, 577,      2,  576,     0 }, // used 1480 times
	  {    0,      5,     1,   1, 673,      2,  672,     0 }, // used 862 times
	  {    0,      5,     1,   1, 769,      2,  768,     0 }, // used 433 times
	  {    0,      5,     1,   1, 961,      2,  960,     0 }, // used 191 times
	  {    1,      4,     0,   2, 104,      1,    0,     0 }, // "1.2.0" => 14187
	  {    1,      4,     0,  -1,  42,      1,    0,     0 }, // "1.-1.0" => 5707
	  {    1,      4,     0,   1,  83,      1,    0,     0 }, // "1.1.0" => 11159
	  {    1,      4,     1,   1,  11,      1,    0,     0 }, // "1.1.1" => 1409
	  {    4,      3,     0,   0,   1,      0,    0,     0 }, // invalid 0xFF
	  {   -1,      0,     0,   0,   0,      0,    0,     0 }, // end
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

	in = fopen(argv[1], "rb");
	out = fopen(argv[2], "wb");

	demuxer_priv = demuxer->init(in);

	if ((err = demuxer->read_headers(demuxer_priv, &nut_stream))) goto err_out;
	mopts.output = (nut_output_stream_t){ .priv = out, .write = NULL };
	mopts.write_index = 1;
	mopts.fti = ft_default;
	mopts.max_distance = 32768;
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
