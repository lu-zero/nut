#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "nut.h"
#include "priv.h"

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

static int stream_write(void * priv, size_t len, const uint8_t * buf) {
	return fwrite(buf, 1, len, priv);
}

static void flush_buf(output_buffer_t * bc) {
	assert(!bc->is_mem);
	bc->file_pos += bc->osc.write(bc->osc.priv, bc->buf_ptr - bc->buf, bc->buf);
	bc->buf_ptr = bc->buf;
}

static void ready_write_buf(output_buffer_t * bc, int amount) {
	if (bc->write_len - (bc->buf_ptr - bc->buf) > amount) return;

        if (bc->is_mem) {
		int tmp = bc->buf_ptr - bc->buf;
		bc->write_len = tmp + amount + PREALLOC_SIZE;
		bc->buf = realloc(bc->buf, bc->write_len);
		bc->buf_ptr = bc->buf + tmp;
	} else {
		flush_buf(bc);
		if (bc->write_len < amount) {
			free(bc->buf);
			bc->write_len = amount + PREALLOC_SIZE;
			bc->buf_ptr = bc->buf = malloc(bc->write_len);
		}
	}
}

static output_buffer_t * new_mem_buffer() {
	output_buffer_t *bc = malloc(sizeof(output_buffer_t));
	bc->write_len = PREALLOC_SIZE;
	bc->is_mem = 1;
	bc->file_pos = 0;
	bc->buf_ptr = bc->buf = malloc(bc->write_len);
	return bc;
}

static output_buffer_t * new_output_buffer(nut_output_stream_t osc) {
	output_buffer_t *bc = new_mem_buffer();
	bc->is_mem = 0;
	bc->osc = osc;
	if (!bc->osc.write) bc->osc.write = stream_write;
	return bc;
}

static void free_buffer(output_buffer_t * bc) {
	if (!bc) return;
	if (!bc->is_mem) flush_buf(bc);
	free(bc->buf);
	free(bc);
}

static output_buffer_t * clear_buffer(output_buffer_t * bc) {
	bc->buf_ptr = bc->buf;
	return bc;
}

static void put_bytes(output_buffer_t * bc, int count, uint64_t val) {
	ready_write_buf(bc, count);
	for(count--; count >= 0; count--){
		*(bc->buf_ptr++) = val >> (8 * count);
	}
}

static int v_len(uint64_t val) {
	int i;
	val &= 0x7FFFFFFFFFFFFFFFULL;
	for(i=1; val>>(i*7); i++);
	return i;
}

static void put_v(output_buffer_t * bc, uint64_t val) {
	int i = v_len(val);
	ready_write_buf(bc, i);
	for(i = (i-1)*7; i; i-=7) {
		*(bc->buf_ptr++) = 0x80 | (val>>i);
	}
	*(bc->buf_ptr++)= val & 0x7F;
}

static void put_s(output_buffer_t * bc, int64_t val) {
	if (val<=0) put_v(bc, -2*val  );
	else        put_v(bc,  2*val-1);
}

static void put_data(output_buffer_t * bc, int len, const uint8_t * data) {
	if (!len) return;
	assert(data);
	if (len < PREALLOC_SIZE || bc->is_mem) {
		ready_write_buf(bc, len);
		memcpy(bc->buf_ptr, data, len);
		bc->buf_ptr += len;
	} else {
		flush_buf(bc);
		bc->file_pos += bc->osc.write(bc->osc.priv, len, data);
	}
}

static void put_vb(output_buffer_t * bc, int len, uint8_t * data) {
	put_v(bc, len);
	put_data(bc, len, data);
}

static void put_syncpoint(nut_context_t * nut, output_buffer_t * bc) {
	int i;
	uint64_t pts = 0;
	int stream = 0;
	int back_ptr = 0;
	int keys[nut->stream_count];
	syncpoint_list_t * s = &nut->syncpoints;

	nut->last_syncpoint = bctello(nut->o);

	for (i = 0; i < nut->stream_count; i++) {
		if (nut->sc[i].last_dts > 0 && compare_ts(nut, nut->sc[i].last_dts, i, pts, stream) > 0) {
			pts = nut->sc[i].last_dts;
			stream = i;
		}
	}

	if (s->alloc_len <= s->len) {
		s->alloc_len += PREALLOC_SIZE;
		s->s = realloc(s->s, s->alloc_len * sizeof(syncpoint_t));
		s->pts = realloc(s->pts, s->alloc_len * nut->stream_count * sizeof(uint64_t));
	}

	for (i = 0; i < nut->stream_count; i++) s->pts[s->len * nut->stream_count + i] = nut->sc[i].last_key;
	s->s[s->len].pos = nut->last_syncpoint;
	s->len++;

	for (i = 0; i < nut->stream_count; i++) keys[i] = !!nut->sc[i].eor; // FIXME for eor in index
	for (i = s->len; --i; ) {
		int j;
		int n = 1;
		for (j = 0; j < nut->stream_count; j++) {
			if (keys[j]) continue;
			if (!s->pts[i * nut->stream_count + j]) continue;
			if (compare_ts(nut, s->pts[i * nut->stream_count + j] - 1, j, pts, stream) <= 0) keys[j] = 1;
		}
		for (j = 0; j < nut->stream_count; j++) n &= keys[j];
		if (n) { i--; break; }
	}
	back_ptr = (nut->last_syncpoint - s->s[i].pos) / 8;

	for (i = 0; i < nut->stream_count; i++) {
		nut->sc[i].last_pts = convert_ts(nut, pts, stream, i);
		nut->sc[i].last_key = 0;
	}

	put_bytes(bc, 8, SYNCPOINT_STARTCODE);
	put_v(bc, pts * nut->stream_count + stream);
	put_v(bc, back_ptr);

	nut->sync_overhead += bctello(bc) + 4/*checksum*/;
}

static int frame_header(nut_context_t * nut, const nut_packet_t * fd, int * rftnum) {
	int i, ftnum = -1, size = 0, coded_pts, pts_delta;
	stream_context_t * sc = &nut->sc[fd->stream];
	pts_delta = fd->pts - sc->last_pts;
	// ### check lsb pts
	if (MAX(pts_delta, -pts_delta) < (1 << (sc->msb_pts_shift - 1)) - 1)
		coded_pts = fd->pts & ((1 << sc->msb_pts_shift) - 1);
	else
		coded_pts = fd->pts + (1 << sc->msb_pts_shift);
	for (i = 0; i < 256; i++) {
		int len = 1;
		if (nut->ft[i].flags & INVALID_FLAG) continue;
		if (nut->ft[i].stream_plus1 && nut->ft[i].stream_plus1 - 1 != fd->stream) continue;
		if (nut->ft[i].pts_delta && nut->ft[i].pts_delta != pts_delta) continue;
		if (nut->ft[i].flags & MSB_CODED_FLAG) {
			if ((fd->len - nut->ft[i].lsb) % nut->ft[i].mul) continue;
		} else {
			if (nut->ft[i].lsb != fd->len) continue;
		}
		if (!(nut->ft[i].flags & STREAM_CODED_FLAG) && (fd->flags & 3) != nut->ft[i].stream_flags) continue;
		len += nut->ft[i].stream_plus1 ? 0 : v_len(fd->stream);
		len += nut->ft[i].pts_delta ? 0 : v_len(coded_pts);
		len += nut->ft[i].flags & MSB_CODED_FLAG ? v_len((fd->len - nut->ft[i].lsb) / nut->ft[i].mul) : 0;
		len += nut->ft[i].flags & STREAM_CODED_FLAG ? v_len((fd->flags & 3) ^ nut->ft[i].stream_flags) : 0;
		if (!size || len < size) { ftnum = i; size = len; }
	}
	assert(ftnum != -1);
	if (rftnum) *rftnum = ftnum;
	return size;
}

static int put_frame_header(nut_context_t * nut, output_buffer_t * bc, const nut_packet_t * fd) {
	stream_context_t * sc = &nut->sc[fd->stream];
	int ftnum = -1, coded_pts, pts_delta = fd->pts - sc->last_pts;
	int size;

	if (ABS(pts_delta) < (1 << (sc->msb_pts_shift - 1)) - 1)
		coded_pts = fd->pts & ((1 << sc->msb_pts_shift) - 1);
	else
		coded_pts = fd->pts + (1 << sc->msb_pts_shift);

	size = frame_header(nut, fd, &ftnum);

	put_bytes(bc, 1, ftnum); // frame_code

	if (!nut->ft[ftnum].stream_plus1) put_v(bc, fd->stream);
	if (!nut->ft[ftnum].pts_delta)    put_v(bc, coded_pts);
	if (nut->ft[ftnum].flags & MSB_CODED_FLAG)
		put_v(bc, (fd->len - nut->ft[ftnum].lsb) / nut->ft[ftnum].mul);
	if (nut->ft[ftnum].flags & STREAM_CODED_FLAG)
		put_v(bc, (fd->flags & 3) ^ nut->ft[ftnum].stream_flags);

	return size;
}

static void put_frame(nut_context_t * nut, const nut_packet_t * fd, const uint8_t * data, int write_syncpoint) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	stream_context_t * sc = &nut->sc[fd->stream];
	int i;

	if (write_syncpoint) put_syncpoint(nut, tmp);

	sc->overhead += put_frame_header(nut, tmp, fd);

	put_data(nut->o, tmp->buf_ptr - tmp->buf, tmp->buf);

	if (write_syncpoint) put_bytes(nut->o, 4, crc32(tmp->buf + 8, tmp->buf_ptr - tmp->buf - 8)); // not including startcode

	sc->total_frames++;
	sc->tot_size += fd->len;

        for (i = 0; i < nut->stream_count; i++) {
		if (nut->sc[i].last_dts == -1) continue;
		if (compare_ts(nut, fd->pts, fd->stream, nut->sc[i].last_dts, i) < 0)
			fprintf(stderr, "%lld %d (%f) %lld %d (%f) \n",
				fd->pts, fd->stream, TO_DOUBLE(fd->stream, fd->pts),
				nut->sc[i].last_dts, i, TO_DOUBLE(i, nut->sc[i].last_dts));
		assert(compare_ts(nut, fd->pts, fd->stream, nut->sc[i].last_dts, i) >= 0);
	}

	put_data(nut->o, fd->len, data);
	sc->last_pts = fd->pts;
	sc->last_dts = get_dts(sc->decode_delay, sc->pts_cache, fd->pts);
	sc->sh.max_pts = MAX(sc->sh.max_pts, fd->pts);
}

static void put_header(output_buffer_t * bc, output_buffer_t * tmp, uint64_t startcode, int index_ptr) {
	int forward_ptr;
	assert(tmp->is_mem);

	forward_ptr = tmp->buf_ptr - tmp->buf;
	forward_ptr += 4; // checksum
	if (index_ptr) forward_ptr += 8;
	fprintf(stderr, "header/index size: %d\n", forward_ptr + 8 + v_len(forward_ptr));

	if (index_ptr) put_bytes(tmp, 8, 8 + v_len(forward_ptr) + forward_ptr);

	put_bytes(bc, 8, startcode);
	put_v(bc, forward_ptr);
	put_data(bc, tmp->buf_ptr - tmp->buf, tmp->buf);
	put_bytes(bc, 4, crc32(tmp->buf, tmp->buf_ptr - tmp->buf));
}

static void put_main_header(nut_context_t * nut) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	int i, j, n;
	int flag, fields, sflag, timestamp = 0, mul = 1, stream = 0, size, count;

	put_v(tmp, NUT_VERSION);
	put_v(tmp, nut->stream_count);
	put_v(tmp, nut->max_distance);
	for(n=i=0; i < 256; n++) {
		assert(nut->fti[n].tmp_flag != -1);
		put_v(tmp, flag = nut->fti[n].tmp_flag);
		put_v(tmp, fields = nut->fti[n].tmp_fields);
		if (fields > 0) put_v(tmp, sflag = nut->fti[n].tmp_sflag);
		else sflag = 0;
		if (fields > 1) put_s(tmp, timestamp = nut->fti[n].tmp_pts);
		if (fields > 2) put_v(tmp, mul = nut->fti[n].tmp_mul);
		if (fields > 3) put_v(tmp, stream = nut->fti[n].tmp_stream);
		if (fields > 4) put_v(tmp, size = nut->fti[n].tmp_size);
		else size = 0;
		if (fields > 5) put_v(tmp, 0); // reserved
		if (fields > 6) put_v(tmp, count = nut->fti[n].count);
		else count = mul - size;

		for(j = 0; j < count && i < 256; j++, i++) {
			if (i == 'N') {
				nut->ft[i].flags = INVALID_FLAG;
				j--;
				continue;
			}
			nut->ft[i].flags = flag;
			nut->ft[i].stream_flags = sflag;
			nut->ft[i].stream_plus1 = stream;
			nut->ft[i].mul = mul;
			nut->ft[i].lsb = size + j;
			nut->ft[i].pts_delta = timestamp;
		}
	}

	assert(nut->fti[n].tmp_flag == -1);
	put_header(nut->o, tmp, MAIN_STARTCODE, 0);
}

static void put_stream_header(nut_context_t * nut, int id) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	stream_context_t * sc = &nut->sc[id];

	put_v(tmp, id); // ### is stream_id staying in spec
	put_v(tmp, sc->sh.type);
	put_vb(tmp, sc->sh.fourcc_len, sc->sh.fourcc);
	put_v(tmp, sc->sh.timebase.nom);
	put_v(tmp, sc->sh.timebase.den);
	put_v(tmp, sc->msb_pts_shift);
	put_v(tmp, sc->max_pts_distance);
	put_v(tmp, sc->decode_delay);
	put_bytes(tmp, 1, sc->sh.fixed_fps ? 1 : 0);
	put_vb(tmp, sc->sh.codec_specific_len, sc->sh.codec_specific);

	switch (sc->sh.type) {
		case NUT_VIDEO_CLASS:
			put_v(tmp, sc->sh.width);
			put_v(tmp, sc->sh.height);
			put_v(tmp, sc->sh.sample_width);
			put_v(tmp, sc->sh.sample_height);
			put_v(tmp, sc->sh.colorspace_type); // TODO understand this
			break;
		case NUT_AUDIO_CLASS:
			put_v(tmp, sc->sh.samplerate_nom);
			put_v(tmp, sc->sh.samplerate_denom);
			put_v(tmp, sc->sh.channel_count); // ### is channel count staying in spec
			break;
	}

	put_header(nut->o, tmp, STREAM_STARTCODE, 0);
}

static void put_headers(nut_context_t * nut) {
	int i;
	nut->last_headers = bctello(nut->o);

	put_main_header(nut);
	for (i = 0; i < nut->stream_count; i++) put_stream_header(nut, i);
}

static void put_index(nut_context_t * nut) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	syncpoint_list_t * s = &nut->syncpoints;
	int i;
	uint64_t max_pts = 0;
	int stream = 0;

	for (i = 0; i < nut->stream_count; i++) {
		if (compare_ts(nut, nut->sc[i].sh.max_pts, i, max_pts, stream) > 0) {
			max_pts = nut->sc[i].sh.max_pts;
			stream = i;
		}
	}
	put_v(tmp, max_pts * nut->stream_count + stream);

	put_v(tmp, s->len);
	for (i = 0; i < s->len; i++) {
		off_t pos = s->s[i].pos / 8;
		off_t last_pos = i ? s->s[i-1].pos / 8 : 0;
		put_v(tmp, pos - last_pos);
	}

	for (i = 0; i < nut->stream_count; i++) {
		uint64_t a, last = 0;
		int j;
		for (j = 0; j < s->len; ) {
			int k;
			a = 0;
			for (k = 0; k < 5 && j+k < s->len; k++) a |= !!s->pts[(j + k) * nut->stream_count + i] << k;
			if (a == 0 || a == ((1 << k) - 1)) {
				int flag = a & 2;
				for (k = 0; j+k < s->len; k++) if (!s->pts[(j+k) * nut->stream_count + i] != !flag) break;
				put_v(tmp, k << 2 | flag | 1);
				if (j+k < s->len) k++;
			} else {
				for (; k+7 < 62 && j+k < s->len; ) {
					uint64_t b = 0;
					int tmp2;
					for (tmp2 = 0; tmp2 < 7 && j+k+tmp2 < s->len; tmp2++) {
						b |= !!s->pts[(j + k + tmp2) * nut->stream_count + i] << tmp2;
					}
					if (b == 0 || b == ((1 << tmp2) - 1)) break;
					a |= b << k;
					k += tmp2;
				}
				put_v(tmp, ((1LL << k) | a) << 1);
			}
			assert(k > 4 || j+k == s->len);
			j += k;
			for (k = j - k; k < j; k++) {
				if (!s->pts[k * nut->stream_count + i]) continue;
				put_v(tmp, s->pts[k * nut->stream_count + i] - last - 1);
				last = s->pts[k * nut->stream_count + i] - 1;
			}
		}
	}

	put_header(nut->o, tmp, INDEX_STARTCODE, 1);
}

void nut_write_info(nut_context_t * nut, const nut_info_packet_t info []) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	int i;

	for(i = 0; ; i++){
		const char * type;
		put_v(tmp, info[i].id);
		if (!info[i].id) break;
		type = info_table[info[i].id].type;
		if (!type) {
			type = info[i].type;
			put_vb(tmp, strlen(info[i].type) + 1, info[i].type);
		}
		if (!info_table[info[i].id].name)
			put_vb(tmp, strlen(info[i].name) + 1, info[i].name);
		if (!strcmp(type, "v"))
			put_v(tmp, info[i].val);
		else if (!strcmp(type, "s"))
			put_s(tmp, info[i].val);
		else
			put_vb(tmp, info[i].val, info[i].data);
	}

	put_header(nut->o, tmp, INFO_STARTCODE, 0);
}

void nut_write_frame(nut_context_t * nut, const nut_packet_t * fd, const uint8_t * buf) {
	stream_context_t * sc = &nut->sc[fd->stream];
	int write_syncpoint = 0;

	if (bctello(nut->o) > (1 << 23)) { // main header repetition
		int i = 23; // ### magic value for header repetition
		if (bctello(nut->o) > (1 << 25)) {
			for (i = 26; bctello(nut->o) > (1 << i); i++);
			i--;
		}
		if (nut->last_headers < (1 << i)) {
			put_headers(nut);
		}
	}
	// distance syncpoints
	if (ABS((int64_t)fd->pts - (int64_t)sc->last_pts) > sc->max_pts_distance)
		fprintf(stderr, "%d - %d > %d   \n", (int)fd->pts, (int)sc->last_pts, sc->max_pts_distance);
	if (nut->last_syncpoint < nut->last_headers || ABS((int64_t)fd->pts - (int64_t)sc->last_pts) > sc->max_pts_distance ||
		bctello(nut->o) - nut->last_syncpoint + fd->len + frame_header(nut, fd, NULL) > nut->max_distance) write_syncpoint = 1;

	put_frame(nut, fd, buf, write_syncpoint);

	if ((fd->flags & NUT_KEY_STREAM_FLAG) && !sc->last_key) sc->last_key = fd->pts + 1;
	if (fd->flags & NUT_EOR_STREAM_FLAG) sc->eor = fd->pts + 1;
	else sc->eor = 0;
}

nut_context_t * nut_muxer_init(const nut_muxer_opts_t * mopts, const nut_stream_header_t s[]) {
	nut_context_t * nut = malloc(sizeof(nut_context_t));
	int i;
	// TODO check that all input is valid

	nut->o = new_output_buffer(mopts->output);
	nut->tmp_buffer = new_mem_buffer(); // general purpose buffer
	nut->max_distance = 32768; // TODO
	nut->fti = ft_default; // TODO
	nut->mopts = *mopts;

	nut->sync_overhead = 0;

	nut->syncpoints.len = 0;
	nut->syncpoints.alloc_len = 0;
	nut->syncpoints.s = NULL;
	nut->syncpoints.pts = NULL;
	nut->last_syncpoint = 0;

	for (nut->stream_count = 0; s[nut->stream_count].type >= 0; nut->stream_count++);

	nut->sc = malloc(sizeof(stream_context_t) * nut->stream_count);

	for (i = 0; i < nut->stream_count; i++) {
		int j;
		nut->sc[i].last_key = 0;
		nut->sc[i].last_pts = 0;
		nut->sc[i].last_dts = -1;
		nut->sc[i].msb_pts_shift = 7; // TODO
		nut->sc[i].max_pts_distance = (s[i].timebase.den + s[i].timebase.nom - 1) / s[i].timebase.nom; // TODO
		nut->sc[i].decode_delay = !i; // ### TODO
		nut->sc[i].eor = 0;
		nut->sc[i].sh = s[i];
		nut->sc[i].sh.max_pts = 0;

		nut->sc[i].sh.fourcc = malloc(s[i].fourcc_len);
		memcpy(nut->sc[i].sh.fourcc, s[i].fourcc, s[i].fourcc_len);

		nut->sc[i].sh.codec_specific = malloc(s[i].codec_specific_len);
		memcpy(nut->sc[i].sh.codec_specific, s[i].codec_specific, s[i].codec_specific_len);

		nut->sc[i].pts_cache = malloc(nut->sc[i].decode_delay * sizeof(int64_t));

		// reorder.c
		nut->sc[i].reorder_pts_cache = malloc(nut->sc[i].decode_delay * sizeof(int64_t));
		for (j = 0; j < nut->sc[i].decode_delay; j++) nut->sc[i].reorder_pts_cache[j] = nut->sc[i].pts_cache[j] = -1;
		nut->sc[i].next_pts = 0;
		nut->sc[i].packets = NULL;
		nut->sc[i].num_packets = 0;

		// debug
		nut->sc[i].total_frames = 0;
		nut->sc[i].overhead = 0;
		nut->sc[i].tot_size = 0;
	}

	put_data(nut->o, strlen(ID_STRING) + 1, ID_STRING);

	put_headers(nut);

	return nut;
}

void nut_muxer_uninit(nut_context_t * nut) {
	int i;
	int total = 0;
	if (!nut) return;

	put_headers(nut);
	if (nut->mopts.write_index) put_index(nut);

	for (i = 0; i < nut->stream_count; i++) {
		total += nut->sc[i].tot_size;
		fprintf(stderr, "Stream %d:\n", i);
		fprintf(stderr, "   frames: %d\n", nut->sc[i].total_frames);
		fprintf(stderr, "   TOT: ");
		fprintf(stderr, "packet size: %d ", nut->sc[i].tot_size);
		fprintf(stderr, "packet overhead: %d ", nut->sc[i].overhead);
		fprintf(stderr, "(%.2lf%%)\n", (double)nut->sc[i].overhead / nut->sc[i].tot_size * 100);
		fprintf(stderr, "   AVG: ");
		fprintf(stderr, "packet size: %.2lf ", (double)nut->sc[i].tot_size / nut->sc[i].total_frames);
		fprintf(stderr, "packet overhead: %.2lf\n", (double)nut->sc[i].overhead / nut->sc[i].total_frames);

		free(nut->sc[i].sh.fourcc);
		free(nut->sc[i].sh.codec_specific);
		free(nut->sc[i].pts_cache);
		free(nut->sc[i].reorder_pts_cache);
	}
	free(nut->sc);

	fprintf(stderr, "Syncpoints: %d size: %d\n", nut->syncpoints.len, nut->sync_overhead);

	free(nut->syncpoints.s);
	free(nut->syncpoints.pts);

	free_buffer(nut->tmp_buffer);
	free_buffer(nut->o); // flushes file
	fprintf(stderr, "TOTAL: %d bytes data, %d bytes overhead, %.2lf%% overhead\n", total,
		(int)ftell(nut->mopts.output.priv) - total, (double)(ftell(nut->mopts.output.priv) - total) / total*100);
	free(nut);
}
