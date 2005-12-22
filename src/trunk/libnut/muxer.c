#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "nut.h"
#include "priv.h"

/*frame_table_input_t ft_default[] = {
	//{ flag, fields, pts, mul, stream, size, count }
	  {    1,      6,   1,   1,      2,    0,     1 },
	  {    3,      4,   1,  74,      2,    0,     0 },
	  {    3,      6,   0,   1,      2,    0,     1 },
	  {    3,      6,   1,   1,      1,    0,     1 },
	  {    3,      6,   0,   1,      1,    0,     1 },
	  {    4,      2,   0,   1,      0,    0,     0 }, // 'N', invalid
	  {    1,      4,   1, 175,      1,    0,     1 },
	  {    1,      6,   0,   1,      0,    0,     1 },
	  {    3,      6,   0,   1,      0,    0,     1 },
	  {   -1,      0,   0,   0,      0,    0,     0 } // end

	// There must be atleast 2 safety nets, one for keyframe and one for non keyframe
	// { 1, 6, 0, 1, 0, 0, 1 },
	// { 3, 6, 0, 1, 0, 0, 1 },
};*/

frame_table_input_t ft_default[] = {
	//{ flag, fields, pts, mul, stream, size, count }
	  {    1,      6,   1,   1,      2,    0,     1 },
	  {    2,      4,   1, 418,      2,  417,     0 },
	  {    2,      4,   1, 366,      2,  365,     0 },
	  {    2,      4,   1, 523,      2,  522,     0 },
	  {    2,      4,   1, 314,      2,  313,     0 },
	  {    2,      4,   1, 627,      2,  626,     0 },
	  {    2,      4,   1, 105,      2,  104,     0 },
	  {    2,      4,   2,  68,      1,    0,     0 },
//	  {    2,      4,   1, 385,      2,  384,     0 },
//	  {    1,      4,   1,  73,      1,    0,     0 },
	  {    3,      6,   0,   1,      2,    0,     1 },
	  {    3,      6,   1,   1,      1,    0,     1 },
	  {    3,      6,   0,   1,      1,    0,     1 },
	  {    4,      2,   0,   1,      0,    0,     0 }, // 'N', invalid
	  {    2,      4,  16, 275,      1,  100,     1 },
	  {    1,      6,   0,   1,      0,    0,     1 },
	  {    3,      6,   0,   1,      0,    0,     1 },
	  {   -1,      0,   0,   0,      0,    0,     0 } // end

	// There must be atleast 2 safety nets, one for keyframe and one for non keyframe
	// { 1, 6, 0, 1, 0, 0, 1 },
	// { 3, 6, 0, 1, 0, 0, 1 },
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

static void put_frame(nut_context_t * nut, const nut_packet_t * fd, const uint8_t * data) {
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
		if (nut->ft[i].flags & 4) continue;
		if (nut->ft[i].stream_plus1 && nut->ft[i].stream_plus1 - 1 != fd->stream) continue;
		if (nut->ft[i].pts_delta && nut->ft[i].pts_delta != pts_delta) continue;
		if ((nut->ft[i].flags & 2) != (fd->is_key ? 2 : 0)) continue;
		if (nut->ft[i].flags & 1) {
			if ((fd->len - nut->ft[i].lsb) % nut->ft[i].mul) continue;
		} else {
			if (nut->ft[i].lsb != fd->len) continue;
		}
		len += nut->ft[i].stream_plus1 ? 0 : v_len(fd->stream);
		len += nut->ft[i].pts_delta ? 0 : v_len(coded_pts);
		len += nut->ft[i].flags & 1 ? v_len((fd->len - nut->ft[i].lsb) / nut->ft[i].mul) : 0;
		if (!size || len < size) { ftnum = i; size = len; }
	}
	assert(ftnum != -1);
	sc->total_frames++;
	sc->overhead += size;
	sc->tot_size += fd->len;
	put_bytes(nut->o, 1, ftnum); // frame_code
	if (!nut->ft[ftnum].stream_plus1) put_v(nut->o, fd->stream);
	if (!nut->ft[ftnum].pts_delta)   put_v(nut->o, coded_pts);
	if (nut->ft[ftnum].flags & 1)
		put_v(nut->o, (fd->len - nut->ft[ftnum].lsb) / nut->ft[ftnum].mul);
	put_data(nut->o, fd->len, data);
	sc->last_pts = fd->pts;
	sc->last_dts = get_dts(sc->decode_delay, sc->pts_cache, fd->pts);
	sc->sh.max_pts = MAX(sc->sh.max_pts, fd->pts);
}

static void put_header(output_buffer_t * bc, output_buffer_t * tmp, uint64_t startcode, int checksum) {
	int forward_ptr;
	assert(tmp->is_mem);

	forward_ptr = tmp->buf_ptr - tmp->buf;
	if (checksum) forward_ptr += 4;
	fprintf(stderr, "header/index size: %d\n", forward_ptr + 8 + v_len(forward_ptr));

	put_bytes(bc, 8, startcode);
	put_v(bc, forward_ptr);
	put_data(bc, tmp->buf_ptr - tmp->buf, tmp->buf);
	if (checksum) put_bytes(bc, 4, adler32(tmp->buf, tmp->buf_ptr - tmp->buf));
}

static void put_main_header(nut_context_t * nut) {
	output_buffer_t *tmp = new_mem_buffer();
	int i, j, n;
	int flag, fields, timestamp = -1, mul = -1, stream = -1, size, count;

	put_v(tmp, NUT_VERSION);
	put_v(tmp, nut->stream_count);
	put_v(tmp, nut->max_distance);
	put_v(tmp, nut->max_index_distance);
	put_v(tmp, nut->global_timebase.nom);
	put_v(tmp, nut->global_timebase.den);
	for(n=i=0; i < 256; n++) {
		assert(nut->fti[n].tmp_flag != -1);
		put_v(tmp, flag = nut->fti[n].tmp_flag);
		put_v(tmp, fields = nut->fti[n].tmp_fields);
		if (fields > 0) put_s(tmp, timestamp = nut->fti[n].tmp_pts);
		if (fields > 1) put_v(tmp, mul = nut->fti[n].tmp_mul);
		if (fields > 2) put_v(tmp, stream = nut->fti[n].tmp_stream);
		if (fields > 3) put_v(tmp, size = nut->fti[n].tmp_size);
		else size = 0;
		if (fields > 4) put_v(tmp, 0); // reserved
		if (fields > 5) put_v(tmp, count = nut->fti[n].count);
		else count = mul - size;

		for(j = 0; j < count && i < 256; j++, i++) {
			assert(i != 'N' || flag == 4);
			nut->ft[i].flags = flag;
			nut->ft[i].stream_plus1 = stream;
			nut->ft[i].mul = mul;
			nut->ft[i].lsb = size + j;
			nut->ft[i].pts_delta = timestamp;
		}
	}

	assert(nut->fti[n].tmp_flag == -1);
	put_header(nut->o, tmp, MAIN_STARTCODE, 1);
	free_buffer(tmp);
}

static void put_stream_header(nut_context_t * nut, int id) {
	output_buffer_t * tmp = new_mem_buffer();
	stream_context_t * sc = &nut->sc[id];

	put_v(tmp, id); // ### is stream_id staying in spec
	put_v(tmp, sc->sh.type);
	put_vb(tmp, sc->sh.fourcc_len, sc->sh.fourcc);
	put_v(tmp, sc->sh.timebase.nom);
	put_v(tmp, sc->sh.timebase.den);
	put_v(tmp, sc->msb_pts_shift);
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

	put_header(nut->o, tmp, STREAM_STARTCODE, 1);
	free_buffer(tmp);
}

static void put_syncpoint(nut_context_t *nut) {
	int i;
	uint64_t pts = 0;
	off_t back_ptr = 0;

	nut->last_syncpoint = bctello(nut->o);

	for (i = 0; i < nut->stream_count; i++) {
		pts = MAX(pts, convert_ts(nut->sc[i].last_dts, nut->sc[i].sh.timebase, nut->global_timebase));
		if (nut->sc[i].back_ptr) {
			if (back_ptr) back_ptr = MIN(back_ptr, nut->sc[i].back_ptr);
			else back_ptr = nut->sc[i].back_ptr;
		}
	}
	if (back_ptr) back_ptr = (nut->last_syncpoint - back_ptr) / 8;

	put_bytes(nut->o, 8, KEYFRAME_STARTCODE);
	put_v(nut->o, pts);
	put_v(nut->o, back_ptr);

	for (i = 0; i < nut->stream_count; i++) {
		nut->sc[i].last_pts = convert_ts(pts, nut->global_timebase, nut->sc[i].sh.timebase);
	}

	nut->sync_overhead += bctello(nut->o) - nut->last_syncpoint;
	nut->syncpoints.len++;
}

static void put_headers(nut_context_t * nut) {
	int i;
	nut->last_headers = bctello(nut->o);
	put_main_header(nut);
	for (i = 0; i < nut->stream_count; i++) {
		put_stream_header(nut, i);
	}
	put_syncpoint(nut);
}

static void put_index(nut_context_t * nut) {
	// ### check index writing
	off_t start = bctello(nut->o);
	int i;
	for (i = 0; i < nut->stream_count; i++) {
		output_buffer_t * tmp = new_mem_buffer();
		index_context_t * idx = &nut->sc[i].index;
		int j, count;
		int max = nut->max_index_distance;
		int last_pos, last_pts;
		int ipos[idx->len];

		put_v(tmp, i); // stream_id
		put_v(tmp, nut->sc[i].sh.max_pts);

		last_pos = 0;
		count = 0;
		for (j = 0; j < idx->len; j++) {
			if (j == 0 || j == idx->len - 1 || idx->ii[j+1].pos - last_pos > max) {
				ipos[count++] = j;
				last_pos = idx->ii[j].pos;
			}
		}

		put_v(tmp, count);

		last_pos = 0;
		last_pts = 0;
		for (j = 0; j < count; j++) {
			put_v(tmp, idx->ii[ipos[j]].pts - last_pts);
			put_v(tmp, idx->ii[ipos[j]].pos - last_pos);
			last_pts = idx->ii[ipos[j]].pts;
			last_pos = idx->ii[ipos[j]].pos;
		}

		put_header(nut->o, tmp, INDEX_STARTCODE, 1);
		free_buffer(tmp);
	}
	put_bytes(nut->o, 8, bctello(nut->o) - start);
}

void nut_write_info(nut_context_t * nut, const nut_info_packet_t info []) {
	output_buffer_t *tmp = new_mem_buffer();
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
		else
			put_vb(tmp, info[i].val, info[i].data);
	}

	put_header(nut->o, tmp, INFO_STARTCODE, 1);
	free_buffer(tmp);
}

void nut_write_frame(nut_context_t * nut, const nut_packet_t * fd, const uint8_t * buf) {
	int wrote_syncpoint = 0;
	stream_context_t * sc = &nut->sc[fd->stream];

	if (fd->is_key) sc->back_ptr = 0; // hint for put_syncpoint

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
	if (bctello(nut->o) - nut->last_syncpoint + fd->len > nut->max_distance) {
		// distance syncpoints
		put_syncpoint(nut);
		wrote_syncpoint = 1;
	}
	if (fd->is_key) { // syncpoints after keyframes
		index_context_t * idx = &sc->index;

		if (sc->last_key > 5) { // ### magic value for keyframes
			sc->last_key = -1;
			if (!wrote_syncpoint) put_syncpoint(nut);
		} else if (sc->last_key > 0)
			sc->last_key++;
			// for an all-keyframe stream last_key will never grow posotive.
		else sc->last_key--;

		// index handling
		if (idx->len == idx->alloc_len) {
			idx->alloc_len += PREALLOC_SIZE;
			idx->ii = realloc(idx->ii, sizeof(index_item_t) * idx->alloc_len);
		}
		idx->ii[idx->len].pts = fd->pts;
		idx->ii[idx->len].pos = bctello(nut->o);
		idx->len++;

		// reset back_ptr
		sc->back_ptr = nut->last_syncpoint;
	} else if (sc->last_key > 0)
		sc->last_key++;
	else
		sc->last_key *= -1;

	put_frame(nut, fd, buf);
}

nut_context_t * nut_muxer_init(const nut_muxer_opts_t * mopts, const nut_stream_header_t s[]) {
	int i;
	nut_context_t * nut = malloc(sizeof(nut_context_t));
	// TODO check that all input is valid

	nut->o = new_output_buffer(mopts->output);
	nut->max_distance = 32768; // TODO
	//nut->max_index_distance = 32768; // TODO
	nut->max_index_distance = 524288; // TODO
	nut->global_timebase.nom = 1; // TODO
	nut->global_timebase.den = 1000; // TODO
	nut->fti = ft_default; // TODO
	nut->mopts = *mopts;

	nut->sync_overhead = 0;
	nut->syncpoints.len = 0;

	for (i = 0; s[i].type >= 0; i++);
	nut->stream_count = i;

	nut->sc = malloc(sizeof(stream_context_t) * nut->stream_count);

	for (i = 0; i < nut->stream_count; i++) {
		int j;
		nut->sc[i].index.len = 0;
		nut->sc[i].index.alloc_len = PREALLOC_SIZE;
		nut->sc[i].index.ii = malloc(sizeof(index_item_t) * nut->sc[i].index.alloc_len);

		nut->sc[i].last_key = -1;
		nut->sc[i].last_pts = 0;
		nut->sc[i].msb_pts_shift = 7; // TODO
		nut->sc[i].decode_delay = 0; // ### TODO
		nut->sc[i].back_ptr = 0;
		nut->sc[i].sh = s[i];
		nut->sc[i].sh.max_pts = 0;

		if (s[i].fourcc_len) {
			nut->sc[i].sh.fourcc = malloc(s[i].fourcc_len);
			memcpy(nut->sc[i].sh.fourcc, s[i].fourcc, s[i].fourcc_len);
		} else nut->sc[i].sh.fourcc = NULL;
		if (s[i].codec_specific_len) {
			nut->sc[i].sh.codec_specific = malloc(s[i].codec_specific_len);
			memcpy(nut->sc[i].sh.codec_specific, s[i].codec_specific, s[i].codec_specific_len);
		} else nut->sc[i].sh.codec_specific = NULL;

		nut->sc[i].next_pts = 0;
		nut->sc[i].packets = NULL;
		nut->sc[i].num_packets = 0;

		nut->sc[i].pts_cache = malloc(nut->sc[i].decode_delay * sizeof(int64_t));
		nut->sc[i].reorder_pts_cache = malloc(nut->sc[i].decode_delay * sizeof(int64_t));
		for (j = 0; j < nut->sc[i].decode_delay; j++)
			nut->sc[i].reorder_pts_cache[j] = nut->sc[i].pts_cache[j] = -1;

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

		free(nut->sc[i].index.ii);
		free(nut->sc[i].sh.fourcc);
		free(nut->sc[i].sh.codec_specific);
		free(nut->sc[i].pts_cache);
		free(nut->sc[i].reorder_pts_cache);
	}
	fprintf(stderr, "Syncpoints: %d size: %d\n", nut->syncpoints.len, nut->sync_overhead);

	free(nut->sc);
	free_buffer(nut->o); // flushes file
	fprintf(stderr, "TOTAL: %d bytes data, %d bytes overhead, %.2lf%% overhead\n", total,
		(int)ftell(nut->mopts.output.priv) - total, (double)(ftell(nut->mopts.output.priv) - total) / total*100);
	free(nut);
}
