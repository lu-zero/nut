#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "nutmerge.h"
#define FREAD(file, len, var) do { if (fread((var), 1, (len), (file)) != (len)) return -1; }while(0)

struct ogg_s;
typedef struct ogg_s ogg_t;
struct ogg_stream_s;
typedef struct ogg_stream_s ogg_stream_t;

typedef struct ogg_codec_s {
	char * magic;
	int magic_len;
	char * fourcc;
	int fourcc_len;
	int type;
	int (*read_headers)(ogg_t * ogg, int stream);
	int (*get_pts)(ogg_stream_t * os);
	int (*is_key)(ogg_stream_t * os);
	void (*uninit)(ogg_stream_t * os);
} ogg_codec_t;

struct ogg_stream_s {
	int serial; // serial of stream
	uint8_t * buf; // buffer, always re-alloced
	int buf_pos; // position to read from
	int buf_end; // total data in buf
	int * sizes; // sizes of segments in buf
	int totpos; // amount of segments
	int pos; // pos in sizes

	ogg_codec_t * oc;
	void * oc_priv;

	// oc->read_headers must fill all of these.
	int time_base_denom;
	int time_base_nom;
	int fixed_fps;
	int codec_specific_len;
	uint8_t * codec_specific;
	int width;
	int height;
	int sample_width;
	int sample_height;
	int colorspace_type;
	int samplerate_nom;
	int samplerate_denom;
	int channel_count;
};

struct ogg_s {
	FILE * in;
	ogg_stream_t * streams;
	int nstreams;
	int last_stream;
};

typedef struct __attribute__((packed)) ogg_header_s {
	char magic[4];
	uint8_t version;
	uint8_t type;
	uint64_t gp; // discarded
	uint32_t serial;
	uint32_t page; // discarded
	uint32_t crc; // discarded
	uint8_t segments;
} ogg_header_t;

static int vorbis_read_headers(ogg_t * ogg, int stream);
static int vorbis_get_pts(ogg_stream_t * os);
static void vorbis_uninit(ogg_stream_t * os);

static ogg_codec_t vorbis_ogg_codec = {
	"\001vorbis", 7, // magic
	"VRBS", 4, // fourcc
	1, // type
	vorbis_read_headers,
	vorbis_get_pts,
	NULL, // is_key
	vorbis_uninit
};

static ogg_codec_t * ogg_codecs[] = {
	&vorbis_ogg_codec,
	NULL
};

static int find_stream(ogg_t * ogg, int serial) {
	ogg_stream_t * os;
	int i;
	for (i = 0; i < ogg->nstreams; i++) {
		if (ogg->streams[i].serial == serial) return i;
	}
	ogg->streams = realloc(ogg->streams, sizeof(ogg_stream_t) * ++ogg->nstreams);
	os = &ogg->streams[i];
	os->serial = serial;
	os->buf = NULL;
	os->buf_pos = 0;
	os->buf_end = 0;
	os->pos = 0;
	os->totpos = 0;
	os->sizes = NULL;
	os->oc = NULL;
	os->oc_priv = NULL;
	return i;
}

static int read_page(ogg_t * ogg, int * stream) {
	ogg_header_t tmp;
	ogg_stream_t * os;
	uint8_t seg[256];
	int i, tot = 0, totseg;

	FREAD(ogg->in, sizeof(ogg_header_t), &tmp);
	if (strncmp(tmp.magic, "OggS", 4)) return 2;
	if (tmp.version != 0) return 3;
	// FIXENDIAN32(tmp.serial); // endianess of serial doesn't matter, it's still unique

	*stream = find_stream(ogg, tmp.serial);
	os = &ogg->streams[*stream];

	FREAD(ogg->in, tmp.segments, seg);
	if (!(tmp.type & 0x01) && os->pos == os->totpos) {
		os->pos = 0;
		os->totpos = 0;
		os->buf_pos = 0;
		os->buf_end = 0;
	}
	totseg = os->buf_end;
	for (i = 0; i < os->totpos; i++) totseg -= os->sizes[i];
	for (i = 0; i < tmp.segments; i++) {
		tot += seg[i];
		totseg += seg[i];
		if (seg[i] < 255) {
			if (os->totpos > 255) return 4;
			os->sizes = realloc(os->sizes, sizeof(int) * ++os->totpos);
			os->sizes[os->totpos - 1] = totseg;
			totseg = 0;
		}
	}
	os->buf = realloc(os->buf, os->buf_end + tot);
	FREAD(ogg->in, tot, os->buf + os->buf_end);
	os->buf_end += tot;
	if (seg[i-1] == 255) // this page is incomplete, move on to next page
		return read_page(ogg, stream);

	return 0;
}

static int get_headers(ogg_t * ogg) {
	int i;
	int err;
	int stream;

	if ((err = read_page(ogg, &stream))) return err;
	do {
		if ((err = read_page(ogg, &stream))) return err;
	} while (stream != 0);
	stream = ogg->nstreams;

	for (i = 0; i < stream; i++) {
		ogg_stream_t * os = &ogg->streams[i];
		int j;
		for (j = 0; ogg_codecs[j]; j++) {
			if (ogg_codecs[j]->magic_len > os->buf_end) continue;
			if (!memcmp(ogg_codecs[j]->magic, os->buf, ogg_codecs[j]->magic_len))
				break;
		}
		if (!ogg_codecs[j]) return 5;
		os->oc = ogg_codecs[j];
		if ((err = os->oc->read_headers(ogg, i))) return err;
	}
	if (stream != ogg->nstreams) return 6; // non-perfect-interleaved!
	return 0;
}

// BEGIN vorbis

static int gcd(int a, int b) {
	while (b != 0) {
		int t = b;
		b = a % b;
		a = t;
	}
	return a;
}
static int ilog(int a) {
	int i;
	for (i = 0; (a >> i) > 0; i++);
	return i;
}

typedef struct bit_packer_s {
	int pos;
	int left;
	uint8_t * buf_ptr;
} bit_packer_t;

static int get_bits(bit_packer_t * bp, int bits, uint64_t * res) {
	uint64_t val = 0;
	int pos = 0;
	bp->left -= bits;
	if (bp->left < 0) return 1;

	if (!bits) return 0;
	if (bp->pos) {
		if (bp->pos > bits) {
			val = *bp->buf_ptr >> (8 - bp->pos);
			val &= (1ULL << bits) - 1;
			bp->pos -= bits;
			pos = bits;
		} else {
			val = *bp->buf_ptr >> (8 - bp->pos);
			pos = bp->pos;
			bp->pos = 0;
			bp->buf_ptr++;
		}
	}
	for (; bits - pos >= 8; pos += 8) val |= *bp->buf_ptr++ << pos;
	if (bits - pos) {
		val |= (*bp->buf_ptr & ((1ULL << (bits - pos)) - 1)) << pos;
		bp->pos = 8 - (bits - pos);
	}
	if (res) *res = val;
	//printf("read %d bits: %d\n", bits, (int)val);
	return 0;
}

#define CHECK(x) do{ if ((err = (x))) return err; }while(0)

static int vorbis_read_headers(ogg_t * ogg, int stream) {
	ogg_stream_t * os = &ogg->streams[stream];
	bit_packer_t bp;
	uint64_t num;
	int err;
	int i, tmp;
	int channels;
	int sample_rate;
	int blocksize0, blocksize1;
	int * priv;
	os->codec_specific = NULL;

	while (os->totpos < 3) { // read more pages
		CHECK(read_page(ogg, &i));
	}

	if (os->sizes[0] < 30) return 1;
	channels = os->buf[11];
	sample_rate = (os->buf[15] << 24) | (os->buf[14] << 16) | (os->buf[13] << 8) | os->buf[12];
	blocksize0 = 1 << (os->buf[28] & 0xF);
	blocksize1 = 1 << (os->buf[28] >> 4);
	i = gcd(blocksize0, blocksize1);
	blocksize0 /= i;
	blocksize1 /= i;

	os->time_base_denom = sample_rate / gcd(sample_rate, i);
	os->time_base_nom = i / gcd(sample_rate, i);
	os->fixed_fps = 1;
	os->codec_specific_len = 0;
	os->codec_specific = NULL;
	os->samplerate_nom = sample_rate;
	os->samplerate_denom = 1;
	os->channel_count = channels;

	os->codec_specific_len = 1 + os->sizes[0]/255 + 1 + os->sizes[1]/255 + 1 +
				os->sizes[0] + os->sizes[1] + os->sizes[2];
	os->codec_specific = malloc(os->codec_specific_len);
	os->codec_specific[0] = 2;
	tmp = 1;
	i = os->sizes[0];
	while (i >= 255) { os->codec_specific[tmp++] = 255; i -= 255; }
	os->codec_specific[tmp++] = i;
	i = os->sizes[1];
	while (i >= 255) { os->codec_specific[tmp++] = 255; i -= 255; }
	os->codec_specific[tmp++] = i;
	memcpy(os->codec_specific + tmp, os->buf, os->sizes[0] + os->sizes[1] + os->sizes[2]);

	bp.buf_ptr = os->buf + os->sizes[0] + os->sizes[1];
	bp.left = os->sizes[2]*8;
	bp.pos = 0;

	CHECK(get_bits(&bp, 8, &num)); if (num != 5) return 2;
	CHECK(get_bits(&bp, 8*6, NULL)); // "vorbis"

	// codebook
	CHECK(get_bits(&bp, 8, &num)); i = num + 1;
	for (; i > 0; i--) {
		int dimentions, entries;
		int j;
		CHECK(get_bits(&bp, 24, &num)); // magic
		CHECK(get_bits(&bp, 16, &num)); dimentions = num;
		CHECK(get_bits(&bp, 24, &num)); entries = num;
		CHECK(get_bits(&bp, 1, &num));
		if (num) { // ordered
			CHECK(get_bits(&bp, 5, NULL)); // len
			j = 0;
			while (j < entries) {
				CHECK(get_bits(&bp, ilog(entries - j), &num));
				j += num;
			}
		} else { // not ordered
			CHECK(get_bits(&bp, 1, &num));
			if (num) { // sparse
				for (j = 0; j < entries; j++) {
					CHECK(get_bits(&bp, 1, &num)); // flag
					if (num) CHECK(get_bits(&bp, 5, NULL));
				}
			} else { // not sparse
				CHECK(get_bits(&bp, 5 * entries, NULL));
			}
		}
		CHECK(get_bits(&bp, 4, &num)); // lookup
		switch (num) {
			case 0: j = -1; break;
			case 1: for (j = 0; ; j++) {
					int n = 1, i;
					for (i = 0; i < dimentions; i++) n*= j;
					if (n > entries) break;
				}
				j--;
				break;
			case 2: j = dimentions * entries; break;
			default:
				return 3;
		}
		if (j >= 0) {
			int bits;
			CHECK(get_bits(&bp, 32, NULL)); // float minimum
			CHECK(get_bits(&bp, 32, NULL)); // float delta
			CHECK(get_bits(&bp, 4, &num)); bits = num + 1;
			CHECK(get_bits(&bp, 1, NULL)); // sequence_p
			CHECK(get_bits(&bp, j*bits, NULL));
		}
	}

	// time domain
	CHECK(get_bits(&bp, 6, &num)); i = num + 1;
	CHECK(get_bits(&bp, i*16, NULL));

	// floors
	CHECK(get_bits(&bp, 6, &num)); i = num + 1;
	for (; i > 0; i--) {
		CHECK(get_bits(&bp, 16, &num));
		if (num == 0) { // floor type 0
			CHECK(get_bits(&bp, 16, NULL)); // floor0_order
			CHECK(get_bits(&bp, 16, NULL)); // floor0_rate
			CHECK(get_bits(&bp, 16, NULL)); // floor0_bark_map_size
			CHECK(get_bits(&bp, 6, NULL)); // floor0_amplitude_bits
			CHECK(get_bits(&bp, 8, NULL)); // floor0_amplitude_offset
			CHECK(get_bits(&bp, 4, &num)); // floor0_number_of_books
			CHECK(get_bits(&bp, 8 * (num+1), NULL)); // floor0_book_list
		} else if (num == 1) { // floor type 1
			int partitions, j, max = -1, rangebits;
			CHECK(get_bits(&bp, 5, &num)); partitions = num;
			{ int class_list[partitions];
			for (j = 0; j < partitions; j++) {
				CHECK(get_bits(&bp, 4, &num));
				class_list[j] = num;
				max = MAX(max, (int)num);
			}
			{ int classes[max + 1];
			for (j = 0; j <= max; j++) {
				int n;
				CHECK(get_bits(&bp, 3, &num));
				classes[j] = num + 1;
				CHECK(get_bits(&bp, 2, &num));
				if (num) CHECK(get_bits(&bp, 8, NULL));
				for (n = 0; n <= ((1 << num) - 1); n++) CHECK(get_bits(&bp, 8, NULL));
			}
			CHECK(get_bits(&bp, 2, NULL)); // multiplier
			CHECK(get_bits(&bp, 4, &num)); rangebits = num;

			for (j = 0; j < partitions; j++) {
				CHECK(get_bits(&bp, classes[class_list[j]]*rangebits, NULL));
			}
			}}
		} else return 5; // unknown floor
	}

	// residues
	CHECK(get_bits(&bp, 6, &num)); i = num + 1;
	for (; i > 0; i--) {
		int j, classifications;
		CHECK(get_bits(&bp, 16, &num));
		if ((int)num > 2) return 6; // unkown residue
		CHECK(get_bits(&bp, 24, NULL)); // residue_begin
		CHECK(get_bits(&bp, 24, NULL)); // residue_end
		CHECK(get_bits(&bp, 24, NULL)); // residue_partition_size
		CHECK(get_bits(&bp, 6, &num)); classifications = num + 1;
		CHECK(get_bits(&bp, 8, NULL)); // residue_classbook
		{int bits[classifications];
		for (j = 0; j < classifications; j++) {
			CHECK(get_bits(&bp, 3, &num)); bits[j] = num;
			CHECK(get_bits(&bp, 1, &num));
			if (num) {
				CHECK(get_bits(&bp, 5, &num));
				bits[j] |= num << 3;
			}
		}
		for (j = 0; j < classifications; j++){
			int bit;
			for (bit = 0; bit < 8; bit++) {
				if (bits[j] & (1 << bit)) CHECK(get_bits(&bp, 8, NULL));
			}
		}
		}
	}

	// mappings
	CHECK(get_bits(&bp, 6, &num)); i = num + 1;
	for (; i > 0; i--) {
		int submaps = 1;
		CHECK(get_bits(&bp, 16, &num)); // type
		if (num) return 7; // bad mapping type
		CHECK(get_bits(&bp, 1, &num)); // is submaps
		if (num) {
			CHECK(get_bits(&bp, 4, &num));
			submaps = num + 1;
		}
		CHECK(get_bits(&bp, 1, &num));
		if (num) {
			CHECK(get_bits(&bp, 8, &num));
			CHECK(get_bits(&bp, ilog(channels - 1) * 2 * num, NULL));
		}
		CHECK(get_bits(&bp, 2, &num)); // reserved
		if (num) return 8;
		if (submaps > 1) CHECK(get_bits(&bp, 4 * channels, NULL));
		CHECK(get_bits(&bp, submaps*(8+8+8), NULL));
	}

	// modes
	CHECK(get_bits(&bp, 6, &num)); i = num + 1;
	priv = os->oc_priv = malloc(sizeof(int) * (i + 4));
	priv[0] = i;
	priv[1] = blocksize0;
	priv[2] = blocksize1;
	priv[3] = 0;
	for (i = 0; i < priv[0]; i++) {
		CHECK(get_bits(&bp, 1, &num)); // block flag
		priv[i+4] = num + 1;
		CHECK(get_bits(&bp, 16 + 16 + 8, NULL));
	}
	CHECK(get_bits(&bp, 1, &num)); // framing
	if (!num) { free(os->oc_priv); return 9; }

	os->buf_pos = os->sizes[0] + os->sizes[1] + os->sizes[2];
	os->pos = 3;
	return 0;
}

static int vorbis_get_pts(ogg_stream_t * os) {
	bit_packer_t bp;
	uint64_t num;
	int * priv = os->oc_priv;
	int pts = priv[3];
	bp.buf_ptr = os->buf + os->buf_pos;
	bp.left = os->sizes[os->pos]*8;
	bp.pos = 0;
	get_bits(&bp, 1, NULL);
	get_bits(&bp, ilog(priv[0] - 1), &num);
	priv[3] += priv[priv[num+4]];
	return pts;
}

static void vorbis_uninit(ogg_stream_t * os) {
	free(os->oc_priv);
	free(os->codec_specific);
}

// END

static void * init(FILE * in) {
	ogg_t * ogg = malloc(sizeof(ogg_t));
	ogg->streams = NULL;
	ogg->nstreams = 0;
	ogg->in = in;
	ogg->last_stream = 0;
	return ogg;
}

static void uninit(void * priv) {
	ogg_t * ogg = priv;
	int i;
	for (i = 0; i < ogg->nstreams; i++) {
		if (ogg->streams[i].oc && ogg->streams[i].oc->uninit)
			ogg->streams[i].oc->uninit(&ogg->streams[i]);
		free(ogg->streams[i].buf);
		free(ogg->streams[i].sizes);
	}
	free(ogg->streams);
	free(ogg);
}

static int read_headers(void * priv, nut_stream_header_t ** nut_streams) {
	ogg_t * ogg = priv;
	nut_stream_header_t * s;
	int i;
	int err;

	if ((err = get_headers(ogg))) return err;

	*nut_streams = s = malloc(sizeof(nut_stream_header_t) * (ogg->nstreams+1));

	for (i = 0; i < ogg->nstreams; i++) {
		ogg_stream_t * os = &ogg->streams[i];
		s[i].type = os->oc->type;
		s[i].fourcc = os->oc->fourcc;
		s[i].fourcc_len = os->oc->fourcc_len;
		s[i].time_base_denom = os->time_base_denom;
		s[i].time_base_nom = os->time_base_nom;
		s[i].fixed_fps = os->fixed_fps;
		s[i].codec_specific = os->codec_specific;
		s[i].codec_specific_len = os->codec_specific_len;
		switch (os->oc->type) {
			case 0: // video
				s[i].width = os->width;
				s[i].height = os->height;
				s[i].sample_width = os->sample_width;
				s[i].sample_height = os->sample_height;
				s[i].colorspace_type = os->colorspace_type;
				break;
			case 1: // audio
				s[i].samplerate_nom = os->samplerate_nom;
				s[i].samplerate_denom = os->samplerate_denom;
				s[i].channel_count = os->channel_count;
		}
	}
	s[i].type = -1;
	return 0;
}

static int get_packet(void * priv, nut_packet_t * p, uint8_t ** buf) {
	ogg_t * ogg = priv;
	int stream = ogg->last_stream;
	ogg_stream_t * os = &ogg->streams[stream];
	int err;
	int size;

	if (os->pos == os->totpos) {
		if ((err = read_page(ogg, &stream))) return err;
		ogg->last_stream = stream;
		os = &ogg->streams[stream];
	}

	size = os->sizes[os->pos];
	p->len = size;

	p->next_pts = 0;
	p->stream = stream;
	p->is_key = os->oc->is_key ? os->oc->is_key(os) : 1;
	p->pts = os->oc->get_pts(os);

	*buf = os->buf + os->buf_pos;

	os->buf_pos += size;
	os->pos++;

	return 0;
}

struct demuxer_t ogg_demuxer = {
	"ogg",
	init,
	read_headers,
	get_packet,
	uninit
};

#ifdef OGG_PROG
int main(int argc, char *argv[]) {
	FILE * in;
	ogg_t * ogg = NULL;
	int err;
	int i;

	if (argc < 2) {
		printf("bleh, more params you fool...\n");
		return 1;
	}

	in = fopen(argv[1], "r");
	ogg = init(in);
	if ((err = get_headers(ogg))) return err;
	printf("Streams: %d\n", ogg->nstreams);

	for (i = 0; i < ogg->nstreams; i++) {
		ogg_stream_t * os = &ogg->streams[i];
		printf("\n");
		printf("Stream: %d\n", i);
		printf(" serial: %d\n", os->serial);
		printf(" codec: %s\n", os->oc->fourcc);
		printf(" timebase: %d / %d\n", os->time_base_nom, os->time_base_denom);
		printf(" type: %d\n", os->oc->type);
		if (!os->oc->type) { // video
			printf("  res: %dx%d\n", os->width, os->height);
		} else {
			printf("  samplerate: %d / %d\n", os->samplerate_nom, os->samplerate_denom);
			printf("  channels: %d\n", os->channel_count);
		}
	}
	uninit(ogg);
	return 0;
}
#endif
