// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include <string.h>
#include "nutmerge.h"

struct framer_priv_s {
	int blocksize[2];
	stream_tt * stream;
	int mode_count;
	int * modes;
	uint8_t * codec_specific;
	int64_t pts;
};

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
} bit_packer_tt;

static int get_bits(bit_packer_tt * bp, int bits, uint64_t * res) {
	uint64_t val = 0;
	int pos = 0;
	bp->left -= bits;
	if (bp->left < 0) return err_vorbis_header;

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

#define CHECK(x) do{ if ((err = (x))) goto err_out; }while(0)

static int setup_headers(framer_priv_tt * vc, nut_stream_header_tt * s) {
	bit_packer_tt bp;
	uint64_t num;
	int i, err = 0, pd_read = 0;
	int channels, sample_rate, codec_specific_len;
	uint8_t * p;
	packet_tt pd[3];

	// need first 3 packets - TODO - support working directly from good codec_specific instead of Ogg crap
	CHECK(get_stream_packet(vc->stream, &pd[0])); pd_read++;
	CHECK(get_stream_packet(vc->stream, &pd[1])); pd_read++;
	CHECK(get_stream_packet(vc->stream, &pd[2])); pd_read++;

	codec_specific_len = 1 + pd[0].p.len/255 + 1 + pd[1].p.len/255 + 1 + pd[0].p.len + pd[1].p.len + pd[2].p.len;
	p = vc->codec_specific = malloc(codec_specific_len);
	*p++ = 2;
	for (i = 0; i < 2; i++) {
		int tmp = pd[i].p.len;
		while (tmp >= 255) { *p++ = 255; tmp -= 255; }
		*p++ = tmp;
	}
	for (i = 0; i < 3; i++) { memcpy(p, pd[i].buf, pd[i].p.len); p += pd[i].p.len; }

	if (pd[0].p.len < 30) { err = err_vorbis_header; goto err_out; }
	p = pd[0].buf;
	channels = p[11];
	sample_rate = (p[15] << 24) | (p[14] << 16) | (p[13] << 8) | p[12];
	vc->blocksize[0] = 1 << (p[28] & 0xF);
	vc->blocksize[1] = 1 << (p[28] >> 4);
	if (vc->blocksize[0] == vc->blocksize[1]) i = vc->blocksize[0]/2;
	else i = vc->blocksize[0]/4;
	vc->blocksize[0] /= i;
	vc->blocksize[1] /= i;

	s->type = NUT_AUDIO_CLASS;
	s->fourcc_len = 4;
	s->fourcc = (uint8_t*)"vrbs";
	s->time_base.den = sample_rate / gcd(sample_rate, i);
	s->time_base.num = i / gcd(sample_rate, i);
	s->fixed_fps = 0;
	s->decode_delay = 0;
	s->codec_specific_len = codec_specific_len;
	s->codec_specific = vc->codec_specific;
	s->samplerate_num = sample_rate;
	s->samplerate_denom = 1;
	s->channel_count = channels;

	bp.buf_ptr = pd[2].buf;
	bp.left = pd[2].p.len*8;
	bp.pos = 0;

	CHECK(get_bits(&bp, 8, &num)); if (num != 5) { err = err_vorbis_header; goto err_out; }
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
				err = err_vorbis_header;
				goto err_out;
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
		} else { err = err_vorbis_header; goto err_out; } // unknown floor
	}

	// residues
	CHECK(get_bits(&bp, 6, &num)); i = num + 1;
	for (; i > 0; i--) {
		int j, classifications;
		CHECK(get_bits(&bp, 16, &num));
		if ((int)num > 2) { err = err_vorbis_header; goto err_out; } // unkown residue
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
		if (num) { err = err_vorbis_header; goto err_out; } // bad mapping type
		CHECK(get_bits(&bp, 1, &num)); // is submaps
		if (num) {
			CHECK(get_bits(&bp, 4, &num));
			submaps = num + 1;
		}
		CHECK(get_bits(&bp, 1, &num)); // square polar
		if (num) {
			CHECK(get_bits(&bp, 8, &num));
			CHECK(get_bits(&bp, ilog(channels - 1) * 2 * (num + 1), NULL));
		}
		CHECK(get_bits(&bp, 2, &num)); // reserved
		if (num) { err = err_vorbis_header; goto err_out; }
		if (submaps > 1) CHECK(get_bits(&bp, 4 * channels, NULL));
		CHECK(get_bits(&bp, submaps*(8+8+8), NULL));
	}

	// finally! modes
	CHECK(get_bits(&bp, 6, &num)); vc->mode_count = num + 1;
	vc->modes = malloc(vc->mode_count * sizeof(int));
	for (i = 0; i < vc->mode_count; i++) {
		CHECK(get_bits(&bp, 1, &num)); // block flag
		vc->modes[i] = num;
		CHECK(get_bits(&bp, 16 + 16 + 8, NULL));
	}
	CHECK(get_bits(&bp, 1, &num)); // framing
	if (!num) { err = err_vorbis_header; goto err_out; }

err_out:
	for (i = 0; i < pd_read; i++) free(pd[i].buf);

	return err;
}

static int get_packet(framer_priv_tt * vc, packet_tt * p) {
	bit_packer_tt bp;
	uint64_t num;
	int64_t last_pts = MAX(vc->pts, 0); // -1 is not valid
	int mode, err = 0;
	int mybs, prevbs, nextbs;

	CHECK(get_stream_packet(vc->stream, p));

	bp.buf_ptr = p->buf;
	bp.left = p->p.len*8;
	bp.pos = 0;
	CHECK(get_bits(&bp, 1, NULL));
	CHECK(get_bits(&bp, ilog(vc->mode_count - 1), &num));
	if ((int)num >= vc->mode_count) return err_vorbis_packet; // ERROR

	mode = vc->modes[num];
	prevbs = nextbs = mybs = vc->blocksize[mode];
	if (mode) { // big window
		CHECK(get_bits(&bp, 1, &num)); prevbs = vc->blocksize[num];
		CHECK(get_bits(&bp, 1, &num)); nextbs = vc->blocksize[num];
	}

	if (vc->pts == -1) vc->pts = -MIN(prevbs, mybs)/2; // negative pts for first frame

	vc->pts += MIN(prevbs, mybs)/2; // overlapped with prev
	vc->pts += (mybs - prevbs)/4; // self-contained
	vc->pts += (mybs - nextbs)/4;

	p->p.pts = last_pts;
	p->p.next_pts = vc->pts;
	p->p.flags = NUT_FLAG_KEY;
err_out:
	return err == err_vorbis_header ? err_vorbis_packet : err;
}

static framer_priv_tt * init(stream_tt * s) {
	framer_priv_tt * vc = malloc(sizeof(framer_priv_tt));
	vc->stream = s;
	vc->modes = NULL;
	vc->codec_specific = NULL;
	vc->pts = -1;
	return vc;
}

static void uninit(framer_priv_tt * vc) {
	free(vc->modes);
	free(vc->codec_specific);
	free(vc);
}

framer_tt vorbis_framer = {
	e_vorbis,
	init,
	setup_headers,
	get_packet,
	uninit,
	NULL
};
