#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "nut.h"
#include "priv.h"

static size_t stream_read(void * priv, size_t len, uint8_t * buf) {
	return fread(buf, 1, len, priv);
}

static off_t stream_seek(void * priv, long pos, int whence) {
	fseek(priv, pos, whence);
	return ftello(priv);
}

static int stream_eof(void * priv) {
	return 1;
}

static void flush_buf(input_buffer_t *bc) {
	assert(!bc->is_mem);
	bc->file_pos += bc->buf_ptr - bc->buf;
	bc->read_len -= bc->buf_ptr - bc->buf;
	memmove(bc->buf, bc->buf_ptr, bc->read_len);
	bc->buf_ptr = bc->buf;
}

static int ready_read_buf(input_buffer_t * bc, int amount) {
	int pos = (bc->buf_ptr - bc->buf);
	if (bc->read_len - pos < amount && !bc->is_mem) {
		if (bc->write_len - pos < amount) {
			bc->write_len = amount + pos + PREALLOC_SIZE;
			bc->buf = realloc(bc->buf, bc->write_len);
			bc->buf_ptr = bc->buf + pos;
		}
		// ### + PREALLOC_SIZE ?
		bc->read_len +=
			bc->isc.read(bc->isc.priv, amount - (bc->read_len - pos) + 10, bc->buf + bc->read_len);
	}
	return bc->read_len - (bc->buf_ptr - bc->buf);
}

static void seek_buf(input_buffer_t * bc, long pos, int whence) {
	assert(!bc->is_mem);
	if (whence == SEEK_CUR) pos -= bc->read_len - (bc->buf_ptr - bc->buf);
	bc->file_pos = bc->isc.seek(bc->isc.priv, pos, whence);
	bc->buf_ptr = bc->buf;
	bc->read_len = 0;
	if (whence == SEEK_END) bc->filesize = bc->file_pos - pos;
}

static int buf_eof(input_buffer_t * bc) {
	if (bc->is_mem) return -ERR_BAD_EOF;
	if (!bc->isc.eof || bc->isc.eof(bc->isc.priv)) return 1;
	return 2;
}

static input_buffer_t * new_mem_buffer() {
	input_buffer_t * bc = malloc(sizeof(input_buffer_t));
	bc->read_len = 0;
	bc->write_len = PREALLOC_SIZE;
	bc->is_mem = 1;
	bc->file_pos = 0;
	bc->filesize = 0;
	bc->buf_ptr = bc->buf = malloc(bc->write_len);
	return bc;
}

static input_buffer_t * new_input_buffer(nut_input_stream_t isc) {
	input_buffer_t * bc = new_mem_buffer();
	bc->is_mem = 0;
	bc->isc = isc;
	if (!bc->isc.read) {
		bc->isc.read = stream_read;
		bc->isc.seek = stream_seek;
		bc->isc.eof = stream_eof;
	}
	return bc;
}

static void free_buffer(input_buffer_t * bc) {
	if (!bc) return;
	free(bc->buf);
	free(bc);
}

static int get_bytes(input_buffer_t * bc, int count, uint64_t * val) {
	int i;
	if (ready_read_buf(bc, count) < count) return buf_eof(bc);
	*val = 0;
	for(i = 0; i < count; i++){
		*val = (*val << 8) | *(bc->buf_ptr++);
	}
	return 0;
}

static int get_v(input_buffer_t * bc, uint64_t * val) {
	int i, len;
	*val = 0;
	while ((len = ready_read_buf(bc, 16))) {
		for(i = 0; i < len; i++){
			uint8_t tmp= *(bc->buf_ptr++);
			*val = (*val << 7) | (tmp & 0x7F);
			if (!(tmp & 0x80)) return 0;
		}
	}
	return buf_eof(bc);
}

static int get_s(input_buffer_t * bc, int64_t * val) {
	uint64_t tmp;
	if (get_v(bc, &tmp)) return buf_eof(bc);
	tmp++;
	if (tmp & 1) *val = -(tmp >> 1);
	else         *val =  (tmp >> 1);
	return 0;
}

#ifdef TRACE
static int get_v_trace(input_buffer_t * bc, uint64_t * val, char * var, char * file, int line, char * func) {
	int a = get_v(bc, val);
	printf("GET_V %llu to var `%s' at %s:%d, %s() (ret: %d)\n", *val, var, file, line, func, a);
	return a;
}

static int get_s_trace(input_buffer_t * bc, int64_t * val, char * var, char * file, int line, char * func) {
	int a = get_s(bc, val);
	printf("GET_S %lld to var `%s' at %s:%d, %s() (ret: %d)\n", *val, var, file, line, func, a);
	return a;
}

#define get_v_(bc, var, name) get_v_trace(bc, var, name, __FILE__, __LINE__, __PRETTY_FUNCTION__)
#define get_s_(bc, var, name) get_s_trace(bc, var, name, __FILE__, __LINE__, __PRETTY_FUNCTION__)
#define get_v(bc, var) get_v_(bc, var, #var)
#define get_s(bc, var) get_s_(bc, var, #var)
#else
#define get_v_(bc, var, name) get_v(bc, var)
#define get_s_(bc, var, name) get_s(bc, var)
#endif

#define CHECK(expr) do { int _a; if ((_a = (expr))) { err = _a; goto err_out; } } while(0)
#define ERROR(expr, code) do { if (expr) { err = code; goto err_out; } } while(0)
#define GET_V(bc, v) do { uint64_t _tmp; CHECK(get_v_((bc), &_tmp, #v)); (v) = _tmp; } while(0)
#define GET_S(bc, v) do {  int64_t _tmp; CHECK(get_s_((bc), &_tmp, #v)); (v) = _tmp; } while(0)

static int get_data(input_buffer_t * bc, int len, uint8_t * buf) {
	int tmp;

	if (!len) return 0;
	assert(buf);

	tmp = ready_read_buf(bc, len);

	len = MIN(len, tmp);
	memcpy(buf, bc->buf_ptr, len);
	bc->buf_ptr += len;

	return len;
}

static int get_vb(input_buffer_t * in, int * len, uint8_t ** buf) {
	uint64_t tmp;
	if (get_v(in, &tmp)) return buf_eof(in);
	*len = tmp;
	*buf = malloc(tmp);
	if (get_data(in, tmp, *buf) != tmp) { free(*buf); return buf_eof(in); }
	return 0;
}

static int get_frame(nut_context_t * nut, nut_packet_t * fd, int n) {
	int err = 0, coded_pts, size_lsb = 0, scrap, i;
	stream_context_t * sc;

	if (!nut->ft[n].stream_plus1) GET_V(nut->i, fd->stream);
	else fd->stream = nut->ft[n].stream_plus1 - 1;

	sc = &nut->sc[fd->stream];

	if (!nut->ft[n].pts_delta) {
		GET_V(nut->i, coded_pts);
		if (coded_pts >= (1 << sc->msb_pts_shift))
			fd->pts = coded_pts - (1 << sc->msb_pts_shift);
		else {
			int mask, delta;
			mask = (1 << sc->msb_pts_shift)-1;
			delta = sc->last_pts - mask/2;
			fd->pts = ((coded_pts - delta) & mask) + delta;
		}
	} else {
		fd->pts = sc->last_pts + nut->ft[n].pts_delta;
	}

	if (nut->ft[n].flags & 1) GET_V(nut->i, size_lsb);
	fd->len = size_lsb * nut->ft[n].mul + nut->ft[n].lsb;
	fd->is_key = (nut->ft[n].flags & 2) ? 1 : 0;

	for (i = 0; i < nut->ft[n].reserved; i++) GET_V(nut->i, scrap);

	sc->last_pts = fd->pts;
err_out:
	return err;
}

static int get_header(input_buffer_t * in, input_buffer_t * out, int len, int checksum) {
	uint64_t code;

	assert(out->is_mem);
	assert(out->buf == out->buf_ptr);

	if (checksum) len -= 4;

	out->write_len = len;
	free(out->buf);
	out->buf_ptr = out->buf = malloc(out->write_len);
	if (get_data(in, len, out->buf) != len) return buf_eof(in);
	out->read_len = len;

	if (checksum) {
		if (get_bytes(in, 4, &code)) return buf_eof(in); // checksum
		if (code != adler32(out->buf, len)) return -ERR_BAD_CHECKSUM;
	}
	return 0;
}

static int get_main_header(nut_context_t *nut, int len) {
	input_buffer_t * tmp = new_mem_buffer();
	int i, j, err = 0;
	int flag, fields, timestamp = -1, mul = -1, stream = -1, size, count, reserved;

	CHECK(get_header(nut->i, tmp, len, 1));

	GET_V(tmp, i);
	ERROR(i != NUT_VERSION, -ERR_BAD_VERSION);
	GET_V(tmp, nut->stream_count);
	GET_V(tmp, nut->max_distance);
	GET_V(tmp, nut->max_index_distance);
	GET_V(tmp, nut->global_time_base_nom);
	GET_V(tmp, nut->global_time_base_denom);

	for(i = 0; i < 256; ) {
		int scrap;
		GET_V(tmp, flag);
		GET_V(tmp, fields);
		if (fields > 0) GET_S(tmp, timestamp);
		if (fields > 1) GET_V(tmp, mul);
		if (fields > 2) GET_V(tmp, stream);
		if (fields > 3) GET_V(tmp, size);
		else size = 0;
		if (fields > 4) GET_V(tmp, reserved);
		else reserved = 0;
		if (fields > 5) GET_V(tmp, count);
		else count = mul - size;

		for (j = 6; j < fields; j++) {
			GET_V(tmp, scrap);
		}

		for(j = 0; j < count && i < 256; j++, i++) {
			assert(i != 'N' || flag == 4);
			nut->ft[i].flags = flag;
			nut->ft[i].stream_plus1 = stream;
			nut->ft[i].mul = mul;
			nut->ft[i].lsb = size + j;
			nut->ft[i].pts_delta = timestamp;
			nut->ft[i].reserved = reserved;
		}
	}
err_out:
	free_buffer(tmp);
	return err;
}

static int get_stream_header(nut_context_t * nut, int id) {
	input_buffer_t * tmp = new_mem_buffer();
	stream_context_t * sc = &nut->sc[id];
	int i, err = 0, len;
	uint64_t a;
	sc->sh.fourcc = NULL;
	sc->sh.codec_specific = NULL;

	GET_V(nut->i, len);
	CHECK(get_header(nut->i, tmp, len, 1));

	GET_V(tmp, i);
	ERROR(i != id, -ERR_BAD_STREAM_ORDER);

	GET_V(tmp, sc->sh.type);
	CHECK(get_vb(tmp, &sc->sh.fourcc_len, &sc->sh.fourcc));
	GET_V(tmp, sc->sh.time_base_nom);
	GET_V(tmp, sc->sh.time_base_denom);
	GET_V(tmp, sc->msb_pts_shift);
	GET_V(tmp, sc->decode_delay);
	CHECK(get_bytes(tmp, 1, &a));
	sc->sh.fixed_fps = a & 1;
	CHECK(get_vb(tmp, &sc->sh.codec_specific_len, &sc->sh.codec_specific));

	switch (sc->sh.type) {
		case NUT_VIDEO_CLASS:
			GET_V(tmp, sc->sh.width);
			GET_V(tmp, sc->sh.height);
			GET_V(tmp, sc->sh.sample_width);
			GET_V(tmp, sc->sh.sample_height);
			GET_V(tmp, sc->sh.colorspace_type); // TODO understand this
			break;
		case NUT_AUDIO_CLASS:
			GET_V(tmp, sc->sh.samplerate_nom);
			GET_V(tmp, sc->sh.samplerate_denom);
			GET_V(tmp, sc->sh.channel_count); // ### is channel count staying in spec
			break;
	}
err_out:
	free_buffer(tmp);
	if (err) {
		free(sc->sh.fourcc);
		free(sc->sh.codec_specific);
	}
	return err;
}

static int get_syncpoint(nut_context_t *nut) {
	int i;
	int err = 0;
	uint64_t ln, d1, d2;
	uint64_t timestamp;
	uint64_t back_ptr;

	GET_V(nut->i, timestamp);
	GET_V(nut->i, back_ptr);

	d1 = nut->global_time_base_denom;
	for (i = 0; i < nut->stream_count; i++) {
		ln = (uint64_t)nut->global_time_base_nom * nut->sc[i].sh.time_base_denom;
		d2 = nut->sc[i].sh.time_base_nom;
		nut->sc[i].last_pts = (ln / d1 * timestamp + (ln%d1) * timestamp / d1) / d2;
	}
	nut->last_syncpoint = bctello(nut->i);
	nut->last_synctime = timestamp;
	if (!nut->first_syncpoint) {
		nut->first_syncpoint = nut->last_syncpoint;
		nut->first_synctime = nut->last_synctime;
	}
err_out:
	return err;
}

static int get_index(nut_context_t * nut) {
	input_buffer_t * tmp = new_mem_buffer();
	index_context_t * idx;
	int err = 0;
	uint64_t startcode;
	CHECK(get_bytes(nut->i, 8, &startcode));
	while (startcode == INDEX_STARTCODE) {
		int id, j, len, cur_pts = 0;
		off_t cur_pos = 0;

		GET_V(nut->i, len);
		CHECK(get_header(nut->i, tmp, len, 1));
		GET_V(tmp, id);
		ERROR(id > nut->stream_count, -1);
		idx = &nut->sc[id].index;

		GET_V(tmp, nut->sc[id].sh.max_pts);
		GET_V(tmp, idx->len);
		idx->alloc_len = idx->len;
		idx->ii = realloc(idx->ii, sizeof(index_item_t) * idx->len);

		for (j = 0; j < idx->len; j++) {
			uint64_t tmps;
			GET_V(tmp, tmps); cur_pts += tmps;
			GET_V(tmp, tmps); cur_pos += tmps;
			idx->ii[j].pts = cur_pts;
			idx->ii[j].pos = cur_pos;
		}

		tmp->buf_ptr = tmp->buf;
		tmp->read_len = 0;
		printf("NUT index read successfully, %d chunks\n", idx->len);

		CHECK(get_bytes(nut->i, 8, &startcode));
	}
err_out:
	free_buffer(tmp);
	// if (err == 1) err = 0; // an EOF at index reading is non fatal
	// FIXME no, wait, yes it is
	return err;
}

int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd) {
	int err = 0;
	uint64_t n, tmp;
	if (!nut->last_headers) {
		const int len = strlen(ID_STRING) + 1;
		char str[len];
		ERROR(get_data(nut->i, len, str) != len, buf_eof(nut->i));
		if (memcmp(str, ID_STRING, len)) nut->i->buf_ptr = nut->i->buf; // rewind
		CHECK(get_bytes(nut->i, 8, &tmp));
		do {
			if (tmp == MAIN_STARTCODE) {
				GET_V(nut->i, pd->len);
				pd->type = e_headers;
				return 0;
			}
			ERROR(ready_read_buf(nut->i, 1) < 1, buf_eof(nut->i));
			tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		} while (bctello(nut->i) < 4096);
	}
	ERROR(!nut->last_headers, -ERR_NO_HEADERS);
	CHECK(get_bytes(nut->i, 1, &n)); // frame_code or 'N'
	if (n != 'N' && !(nut->ft[n].flags & 4)) { // frame
		pd->type = e_frame;
		CHECK(get_frame(nut, pd, n));
		goto err_out;
	}

	ERROR(n != 'N', -ERR_NOT_FRAME_NOT_N); // ###
	CHECK(get_bytes(nut->i, 7, &tmp));
	switch ((n << 56) | tmp) {
		case KEYFRAME_STARTCODE:
			CHECK(get_syncpoint(nut));
			flush_buf(nut->i);
			return nut_read_next_packet(nut, pd);
		case INDEX_STARTCODE:
			err = 1; // EOF
			goto err_out;
		case MAIN_STARTCODE:
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
		case STREAM_STARTCODE:
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
		case INFO_STARTCODE: // FIXME
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
		default:
			GET_V(nut->i, pd->len);
			pd->type = e_unknown;
			break;
	}
err_out:
	if (err != 2) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

int nut_read_headers(nut_context_t * nut, nut_packet_t * pd, nut_stream_header_t * s []) {
	int i, err = 0;
	off_t headers_pos = bctello(nut->i);
	*s = NULL;
	if (!nut->last_headers) { // we already have headers, we were called just for index
		nut->sc = NULL;

		CHECK(get_main_header(nut, pd->len));

		nut->sc = malloc(sizeof(stream_context_t) * nut->stream_count);

		for (i = 0; i < nut->stream_count; i++) {
			uint64_t tmp;
			nut->sc[i].index.ii = NULL;
			CHECK(get_bytes(nut->i, 8, &tmp));
			if (tmp != STREAM_STARTCODE) return -1;

			nut->sc[i].index.len = 0;
			nut->sc[i].index.alloc_len = 0;
			nut->sc[i].index.ii = NULL;
			nut->sc[i].last_pts = 0;
			nut->sc[i].sh.max_pts = 0;

			CHECK(get_stream_header(nut, i));
		}
		nut->last_headers = headers_pos;
	}

	if (nut->dopts.read_index && nut->i->isc.seek) {
		uint64_t idx_ptr;
		if (!nut->index_pos) {
			if (!nut->prev_pos) {
				off_t cur_pos = bctello(nut->i);
				seek_buf(nut->i, -8, SEEK_END);
				nut->prev_pos = cur_pos;
			}
			CHECK(get_bytes(nut->i, 8, &idx_ptr));
			idx_ptr = nut->i->filesize - 8 - idx_ptr;
			if (idx_ptr < nut->i->filesize) { // valid ptr
				nut->index_pos = idx_ptr;
				seek_buf(nut->i, nut->index_pos, SEEK_SET);
			} else {
				seek_buf(nut->i, nut->prev_pos, SEEK_SET);
				nut->prev_pos = 0;
			}
		}
		if (nut->index_pos) {
			CHECK(get_index(nut));
			seek_buf(nut->i, nut->prev_pos, SEEK_SET);
			nut->prev_pos = 0;
		}
	}
	*s = malloc(sizeof(nut_stream_header_t) * (nut->stream_count + 1));
	for (i = 0; i < nut->stream_count; i++) (*s)[i] = nut->sc[i].sh;
	(*s)[i].type = -1;
err_out:
	if (err) {
		if (!nut->last_headers) {
			// impossible for nut->sc->index.ii to be alloced
			// because that means headers were read successfully..
			free(nut->sc);
			nut->sc = NULL;
			nut->stream_count = 0;
		}
	}
	if (err != 2) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

int nut_read_frame(nut_context_t * nut, int * len, uint8_t * buf) {
	int tmp = MIN(*len, nut->i->read_len - (nut->i->buf_ptr - nut->i->buf));
	if (tmp) {
		memcpy(buf, nut->i->buf_ptr, tmp);
		nut->i->buf_ptr += tmp;
		*len -= tmp;
	}
	if (*len) {
		int read = nut->i->isc.read(nut->i->isc.priv, *len, buf + tmp);
		nut->i->file_pos += read;
		*len -= read;
	}

	flush_buf(nut->i); // ALWAYS flush...

	if (*len) return buf_eof(nut->i);
	return 0;
}

int nut_skip_packet(nut_context_t * nut, int * len) {
	// FIXME or fseek
#ifdef __TCC__
	uint8_t * tmp = malloc(*len);
	int a = nut_read_frame(nut, len, tmp);
	free(tmp);
	return a;
#else
	uint8_t tmp[*len];
	return nut_read_frame(nut, len, tmp);
#endif
}

int nut_read_info(nut_context_t * nut, nut_info_packet_t * info []) {
#if 0
	BufferContext *tmp = new_mem_buffer();

	for(;;){
		char * type;
		int id = info[0].id;
		put_v(tmp, id);
		if (!id) break;
		type = info_table[id].type;
		if (!type) {
			type = info[0].type.data;
			put_vb(tmp, info[0].type.len, info[0].type.data);
		}
		if (!info_table[id].name)
			put_vb(tmp, info[0].name.len, info[0].name.data);
		if (!strcmp(type, "v"))
			put_v(tmp, info[0].val.i);
		else
			put_vb(tmp, info[0].val.s.len, info[0].val.s.data);
		id++;
	}

	put_header(nut->i, tmp, INFO_STARTCODE, 1);
	free_buffer(tmp);
#endif
	return 0;
}

void nut_free_info(nut_info_packet_t info []) {
}

static int find_syncpoint(nut_context_t * nut, int backwards, uint64_t * time, uint64_t * back_ptr) {
	int len = nut->max_distance + 7;
	off_t last = bctello(nut->i) + 1;
	uint64_t tmp = 0;
	// TODO, add caching, make smarter
retry:
	if (backwards) seek_buf(nut->i, -nut->max_distance, SEEK_CUR);
	if (last == bctello(nut->i)) return -ERR_NOT_SEEKABLE;
	last = bctello(nut->i);

	flush_buf(nut->i);
	len = ready_read_buf(nut->i, len);

	if (len == 0) return buf_eof(nut->i); // FIXME

	while (nut->i->buf_ptr - nut->i->buf < len) {
		tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		if (tmp != KEYFRAME_STARTCODE) continue;
		last = bctello(nut->i) - 8;
		// FIXME EOF
		get_v(nut->i, &tmp); if (time) *time = tmp;
		get_v(nut->i, &tmp); if (back_ptr) *back_ptr = last - 8 * tmp - 7;
		return 0;
	}
	goto retry;
}

static int search_syncpoint(nut_context_t * nut, uint64_t trequested) {
	off_t p1, p2, p3;
	off_t plast = 0;
	uint64_t t1, t2, t3;
	uint64_t back_ptr;
	int i;
	t3 = trequested;
	if (t3 > nut->last_synctime) { // future
		p1 = nut->last_syncpoint;
		t1 = nut->last_synctime;
		seek_buf(nut->i, 0, SEEK_END);
		find_syncpoint(nut, 1, &t2, NULL);
		p2 = bctello(nut->i);
	} else { // past
		p2 = nut->last_syncpoint;
		t2 = nut->last_synctime;
		if (!nut->first_syncpoint) {
			seek_buf(nut->i, 0, SEEK_SET);
			find_syncpoint(nut, 0, &nut->first_synctime, NULL);
			nut->first_syncpoint = bctello(nut->i);
		}
		p1 = nut->first_syncpoint;
		t1 = nut->first_synctime;
	}
	p3 = t3 * (double)(p2 - p1) / (t2 - t1); // wild guess by avg
	while (1) {
		seek_buf(nut->i, p3, SEEK_SET);
		find_syncpoint(nut, 0, &t3, &back_ptr); // FIXME check for EOF
		p3 = bctello(nut->i);
		if (p3 == plast) break; // we landed exactly where we were...
		plast = p3;
		if (t3 > trequested) { // we landed too forward
			p2 = p3;
			t2 = t3;
		} else { // we landed too backwards
			p1 = p3;
			t1 = t3;
		}
		p3 = (p1 + p2) / 2; // middle of results
	}
	assert(t3 >= trequested);
	while (t3 > trequested) find_syncpoint(nut, 1, &t3, &back_ptr);
	//printf("wanted time: %lld, found time: %lld at pos %d\n", trequested, t3, bctello(nut->i));
	seek_buf(nut->i, back_ptr, SEEK_SET);
	ready_read_buf(nut->i, 8); // sigh, FIXME
	for (i = 0; i < 8; i++) if (*(nut->i->buf_ptr++) == 'N') break;
	nut->i->buf_ptr--;
	return 0;
}

int nut_seek(nut_context_t * nut, double time_pos, int type, int * active_streams) {
	if (!nut->i->isc.seek) return -ERR_NOT_SEEKABLE;

	if (!nut->sc[0].index.ii) {
		int64_t tmp;
		double time_base = (double)nut->global_time_base_nom / nut->global_time_base_denom;
		tmp = time_pos / time_base;
		if (type & 1) tmp += nut->last_synctime;
		//printf("something: %lf / %lf = %llu\n", time_pos, time_base, tmp);
		search_syncpoint(nut, tmp); // FIXME, return code.
		get_bytes(nut->i, 8, &tmp);
		if (tmp != KEYFRAME_STARTCODE) return -ERR_NOT_SEEKABLE; // FIXME
		nut->i->buf_ptr -= 8;
		flush_buf(nut->i);
		return 0;
	} else { // have index
		// FIXME nut->last_syncpoint;
		int i;
		int64_t pts;
		pts = time_pos / ((double)nut->sc[0].sh.time_base_nom / nut->sc[0].sh.time_base_denom);
		if (type & 1) pts += nut->sc[0].last_pts;

		for (i = 0; i < nut->sc[0].index.len; i++) {
			if (nut->sc[0].index.ii[i].pts > pts) break;
		}
		if ((type & 2) && i > 0) i--;

		if (i == nut->sc[0].index.len) seek_buf(nut->i, 0, SEEK_END);
		else seek_buf(nut->i, nut->sc[0].index.ii[i].pos, SEEK_SET);
	}

	return 0;
}

nut_context_t * nut_demuxer_init(nut_demuxer_opts_t * dopts) {
	nut_context_t * nut = malloc(sizeof(nut_context_t));

	nut->i = new_input_buffer(dopts->input);
	nut->fti = NULL;
	nut->sc = NULL;
	nut->first_syncpoint = 0;
	nut->first_synctime = 0;
	nut->last_syncpoint = 0;
	nut->last_synctime = 0;
	nut->last_headers = 0;
	nut->index_pos = 0;
	nut->prev_pos = 0;
	nut->stream_count = 0;
	nut->dopts = *dopts;
	return nut;
}

void nut_demuxer_uninit(nut_context_t * nut) {
	int i;
	if (!nut) return;
	for (i = 0; i < nut->stream_count; i++) {
		free(nut->sc[i].index.ii);
		free(nut->sc[i].sh.fourcc);
		free(nut->sc[i].sh.codec_specific);
	}

	free(nut->sc);
	free_buffer(nut->i);
	free(nut);
}

const char * nut_error(int error) {
	switch((enum errors)error) {
		case ERR_GENERAL_ERROR: return "General Error.";
		case ERR_BAD_VERSION: return "Bad NUT Version.";
		case ERR_NOT_FRAME_NOT_N: return "Invalid Framecode.";
		case ERR_BAD_CHECKSUM: return "Bad Checksum.";
		case ERR_NO_HEADERS: return "No headers found!";
		case ERR_NOT_SEEKABLE: return "Cannot seek to that position.";
		case ERR_BAD_STREAM_ORDER: return "Stream headers are stored in wrong order.";
		case ERR_CANT_SEEK: return "Managed to seek to index but cannot seek back!";
		case ERR_BAD_EOF: return "Invalid forward_ptr!";
	}
	return NULL;
}
