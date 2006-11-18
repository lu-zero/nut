// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "libnut.h"
#include "priv.h"

static size_t stream_read(void * priv, size_t len, uint8_t * buf) {
	return fread(buf, 1, len, priv);
}

static off_t stream_seek(void * priv, long long pos, int whence) {
	fseek(priv, pos, whence);
	return ftello(priv);
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
		amount += 10; // ### + PREALLOC_SIZE ?
		if (!bc->alloc) return 0; // there was a previous memory error
		if (bc->write_len - pos < amount) {
			int new_len = amount + pos + PREALLOC_SIZE;
			uint8_t * buf = bc->alloc->realloc(bc->buf, new_len);
			if (!buf) { bc->alloc = NULL; return 0; }
			bc->write_len = new_len;
			bc->buf = buf;
			bc->buf_ptr = bc->buf + pos;
		}
		bc->read_len += bc->isc.read(bc->isc.priv, amount - (bc->read_len - pos), bc->buf + bc->read_len);
	}
	return bc->read_len - (bc->buf_ptr - bc->buf);
}

static void seek_buf(input_buffer_t * bc, long long pos, int whence) {
	assert(!bc->is_mem);
	if (whence != SEEK_END) {
		// don't do anything when already in seeked position. but still flush_buf
		off_t req = pos + (whence == SEEK_CUR ? bctello(bc) : 0);
		if (req >= bc->file_pos && req < bc->file_pos + bc->read_len) {
			bc->buf_ptr = bc->buf + (req - bc->file_pos);
			flush_buf(bc);
			return;
		}
	}
	if (whence == SEEK_CUR) pos -= bc->read_len - (bc->buf_ptr - bc->buf);
	fprintf(stderr, "seeking %d ", (int)pos);
	switch (whence) {
		case SEEK_SET: fprintf(stderr, "SEEK_SET   "); break;
		case SEEK_CUR: fprintf(stderr, "SEEK_CUR   "); break;
		case SEEK_END: fprintf(stderr, "SEEK_END   "); break;
	}
	bc->file_pos = bc->isc.seek(bc->isc.priv, pos, whence);
	bc->buf_ptr = bc->buf;
	bc->read_len = 0;
	if (whence == SEEK_END) bc->filesize = bc->file_pos - pos;
}

static int buf_eof(input_buffer_t * bc) {
	if (bc->is_mem) return NUT_ERR_BAD_EOF;
	if (!bc->alloc) return NUT_ERR_OUT_OF_MEM;
	if (!bc->isc.eof || bc->isc.eof(bc->isc.priv)) return NUT_ERR_EOF;
	return NUT_ERR_EAGAIN;
}

static int skip_buffer(input_buffer_t * bc, int len) {
	if (ready_read_buf(bc, len) < len) return buf_eof(bc);
	bc->buf_ptr += len;
	return 0;
}

static uint8_t * get_buf(input_buffer_t * bc, off_t start) {
	start -= bc->file_pos;
	assert((unsigned)start < bc->read_len);
	return bc->buf + start;
}

static input_buffer_t * new_mem_buffer(input_buffer_t * bc) {
	if (!bc) return NULL;
	bc->read_len = 0;
	bc->write_len = 0;
	bc->is_mem = 1;
	bc->file_pos = 0;
	bc->filesize = 0;
	bc->buf_ptr = bc->buf = NULL;
	bc->alloc = NULL;
	return bc;
}

static input_buffer_t * new_input_buffer(nut_alloc_t * alloc, nut_input_stream_t isc) {
	input_buffer_t * bc = new_mem_buffer(alloc->malloc(sizeof(input_buffer_t)));
	if (!bc) return NULL;
	bc->alloc = alloc;
	bc->is_mem = 0;
	bc->isc = isc;
	bc->file_pos = isc.file_pos;
	if (!bc->isc.read) {
		bc->isc.read = stream_read;
		bc->isc.seek = stream_seek;
		bc->isc.eof = NULL;
	}
	return bc;
}

static void free_buffer(input_buffer_t * bc) {
	if (!bc) return;
	assert(!bc->is_mem);
	bc->alloc->free(bc->buf);
	bc->alloc->free(bc);
}

static int get_bytes(input_buffer_t * bc, int count, uint64_t * val) {
	int i;
	if (ready_read_buf(bc, count) < count) return buf_eof(bc);
	*val = 0;
	for (i = 0; i < count; i++) {
		*val = (*val << 8) | *(bc->buf_ptr++);
	}
	return 0;
}

static int get_v(input_buffer_t * bc, uint64_t * val) {
	int i, len;
	*val = 0;

	do {
		len = ready_read_buf(bc, 16);
		for (i = 0; i < len; i++) {
			uint8_t tmp= *(bc->buf_ptr++);
			*val = (*val << 7) | (tmp & 0x7F);
			if (!(tmp & 0x80)) return 0;
		}
		if (len >= 16 && !bc->is_mem) return NUT_ERR_VLC_TOO_LONG;
	} while (len >= 16);
	return buf_eof(bc);
}

static int get_s(input_buffer_t * bc, int64_t * val) {
	uint64_t tmp;
	int err;
	if ((err = get_v(bc, &tmp))) return err;
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

#define CHECK(expr) do { if ((err = (expr))) goto err_out; } while(0)
#define ERROR(expr, code) do { if (expr) { err = code; goto err_out; } } while(0)
#define GET_V(bc, v) do { uint64_t _tmp; CHECK(get_v_((bc), &_tmp, #v)); (v) = _tmp; } while(0)
#define GET_S(bc, v) do {  int64_t _tmp; CHECK(get_s_((bc), &_tmp, #v)); (v) = _tmp; } while(0)
#define SAFE_CALLOC(alloc, var, a, b) do { \
	ERROR(SIZE_MAX/(a) < (b), NUT_ERR_OUT_OF_MEM); \
	ERROR(!((var) = (alloc)->malloc((a) * (b))), NUT_ERR_OUT_OF_MEM); \
	memset((var), 0, (a) * (b)); \
} while(0)
#define SAFE_REALLOC(alloc, var, a, b) do { \
	void * _tmp; \
	ERROR(SIZE_MAX/(a) < (b), NUT_ERR_OUT_OF_MEM); \
	ERROR(!((_tmp) = (alloc)->realloc((var), (a) * (b))), NUT_ERR_OUT_OF_MEM); \
	(var) = _tmp; \
} while(0)

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

static int get_vb(nut_alloc_t * alloc, input_buffer_t * in, int * len, uint8_t ** buf) {
	uint64_t tmp;
	int err;
	if ((err = get_v(in, &tmp))) return err;
	if (!*len) {
		*buf = alloc->malloc(tmp);
		if (!*buf) return NUT_ERR_OUT_OF_MEM;
	} else if (*len < tmp) return NUT_ERR_OUT_OF_MEM;
	*len = tmp;
	if (get_data(in, *len, *buf) != *len) return buf_eof(in);
	return 0;
}

static int get_header(input_buffer_t * in, input_buffer_t * out) {
	off_t start = bctello(in) - 8; // startcode
	int forward_ptr;
	int err = 0;

	GET_V(in, forward_ptr);
	if (forward_ptr > 4096) {
		CHECK(skip_buffer(in, 4)); // header_checksum
		ERROR(crc32(get_buf(in, start), bctello(in) - start), NUT_ERR_BAD_CHECKSUM);
	}

	CHECK(skip_buffer(in, forward_ptr));
	ERROR(crc32(in->buf_ptr - forward_ptr, forward_ptr), NUT_ERR_BAD_CHECKSUM);

	if (out) {
		assert(out->is_mem);
		assert(out->buf == out->buf_ptr);
		out->buf_ptr = out->buf = in->buf_ptr - forward_ptr;
		out->write_len = out->read_len = forward_ptr - 4; // not including checksum
	}
err_out:
	return err;
}

static int get_main_header(nut_context_t * nut) {
	input_buffer_t itmp, * tmp = new_mem_buffer(&itmp);
	int i, j, err = 0;
	int flag, fields, timestamp = 0, mul = 1, stream = 0, size, count, reserved;

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, i);
	ERROR(i != NUT_VERSION, NUT_ERR_BAD_VERSION);
	GET_V(tmp, nut->stream_count);
	GET_V(tmp, nut->max_distance);
	if (nut->max_distance > 65536) nut->max_distance = 65536;

	GET_V(tmp, nut->timebase_count);
	nut->alloc->free(nut->tb); nut->tb = NULL;
	ERROR(SIZE_MAX/sizeof(nut_timebase_t) < nut->timebase_count, NUT_ERR_OUT_OF_MEM);
	nut->tb = nut->alloc->malloc(nut->timebase_count * sizeof(nut_timebase_t));
	ERROR(!nut->tb, NUT_ERR_OUT_OF_MEM);
	for (i = 0; i < nut->timebase_count; i++) {
		GET_V(tmp, nut->tb[i].nom);
		GET_V(tmp, nut->tb[i].den);
	}

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

		for (j = 6; j < fields; j++) GET_V(tmp, scrap);

		for(j = 0; j < count && i < 256; j++, i++) {
			if (i == 'N') {
				nut->ft[i].flags = FLAG_INVALID;
				j--;
				continue;
			}
			nut->ft[i].flags = flag;
			nut->ft[i].stream = stream;
			nut->ft[i].mul = mul;
			nut->ft[i].lsb = size + j;
			nut->ft[i].pts_delta = timestamp;
			nut->ft[i].reserved = reserved;
		}
	}
err_out:
	return err;
}

static int get_stream_header(nut_context_t * nut, int id) {
	input_buffer_t itmp, * tmp = new_mem_buffer(&itmp);
	stream_context_t * sc = &nut->sc[id];
	int i, err = 0;

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, i);
	ERROR(i != id, NUT_ERR_BAD_STREAM_ORDER);

	GET_V(tmp, sc->sh.type);
	CHECK(get_vb(nut->alloc, tmp, &sc->sh.fourcc_len, &sc->sh.fourcc));
	GET_V(tmp, sc->timebase_id);
	sc->sh.time_base = nut->tb[sc->timebase_id];
	GET_V(tmp, sc->msb_pts_shift);
	GET_V(tmp, sc->max_pts_distance);
	GET_V(tmp, sc->sh.decode_delay);
	GET_V(tmp, i); // stream_flags
	sc->sh.fixed_fps = i & 1;
	CHECK(get_vb(nut->alloc, tmp, &sc->sh.codec_specific_len, &sc->sh.codec_specific));

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
	return err;
}

static void free_info_packet(nut_context_t * nut, nut_info_packet_t * info) {
	int i;
	for (i = 0; i < info->count; i++) nut->alloc->free(info->fields[i].data);
	nut->alloc->free(info->fields);
}

static int get_info_header(nut_context_t * nut, nut_info_packet_t * info) {
	input_buffer_t itmp, * tmp = new_mem_buffer(&itmp);
	int i, err = 0;
	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, info->stream_id_plus1);
	GET_V(tmp, info->chapter_id);
	GET_V(tmp, info->chapter_start);
	info->chapter_tb = nut->tb[info->chapter_start % nut->timebase_count];
	info->chapter_start /= nut->timebase_count;
	GET_V(tmp, info->chapter_len);

	GET_V(tmp, info->count);
	SAFE_CALLOC(nut->alloc, info->fields, sizeof(nut_info_field_t), info->count);

	for (i = 0; i < info->count; i++) {
		int len;
		nut_info_field_t * field = &info->fields[i];

		len = sizeof(field->name) - 1;
		CHECK(get_vb(nut->alloc, tmp, &len, (uint8_t**)&field->name));
		field->name[len] = 0;

		GET_S(tmp, field->val);

		if (field->val == -1) {
			strcpy(field->type, "UTF-8");
			CHECK(get_vb(nut->alloc, tmp, &field->den, &field->data));
			field->val = field->den;
		} else if (field->val == -2) {
			len = sizeof(field->type) - 1;
			CHECK(get_vb(nut->alloc, tmp, &len, (uint8_t**)&field->type));
			field->type[len] = 0;
			CHECK(get_vb(nut->alloc, tmp, &field->den, &field->data));
			field->val = field->den;
		} else if (field->val == -3) {
			strcpy(field->type, "s");
			GET_S(tmp, field->val);
		} else if (field->val == -4) {
			strcpy(field->type, "t");
			GET_V(tmp, field->val);
			field->tb = nut->tb[field->val % nut->timebase_count];
			field->val /= nut->timebase_count;
		} else if (field->val < -4) {
			strcpy(field->type, "r");
			field->den = -field->val - 4;
			GET_S(tmp, field->val);
		} else {
			strcpy(field->type, "v");
		}
	}

err_out:
	return err;
}

static int skip_reserved_headers(nut_context_t * nut, uint64_t stop_startcode) {
	int err;
	uint64_t tmp;
	CHECK(get_bytes(nut->i, 8, &tmp));
	while (tmp != SYNCPOINT_STARTCODE && tmp != stop_startcode) {
		ERROR(tmp >> 56 != 'N', NUT_ERR_NOT_FRAME_NOT_N);
		CHECK(get_header(nut->i, NULL));
		CHECK(get_bytes(nut->i, 8, &tmp));
	}
	nut->i->buf_ptr -= 8;
err_out:
	return err;
}

static int get_headers(nut_context_t * nut, int read_info) {
	int i, err;
	uint64_t tmp;

	CHECK(get_bytes(nut->i, 8, &tmp));
	assert(tmp == MAIN_STARTCODE); // sanity, get_headers should only be called in this situation
	CHECK(get_main_header(nut));

	SAFE_CALLOC(nut->alloc, nut->sc, sizeof(stream_context_t), nut->stream_count);

	for (i = 0; i < nut->stream_count; i++) {
		int j;
		CHECK(skip_reserved_headers(nut, STREAM_STARTCODE));
		CHECK(get_bytes(nut->i, 8, &tmp));
		ERROR(tmp != STREAM_STARTCODE, NUT_ERR_NOSTREAM_STARTCODE);
		CHECK(get_stream_header(nut, i));
		SAFE_CALLOC(nut->alloc, nut->sc[i].pts_cache, sizeof(int64_t), nut->sc[i].sh.decode_delay);
		for (j = 0; j < nut->sc[i].sh.decode_delay; j++) nut->sc[i].pts_cache[j] = -1;
	}
	if (read_info) {
		// FIXME this skip_reserved_headers() will skip INDEX_STARTCODE
		CHECK(skip_reserved_headers(nut, INFO_STARTCODE));
		CHECK(get_bytes(nut->i, 8, &tmp));
		while (tmp == INFO_STARTCODE) {
			nut->info_count++;
			SAFE_REALLOC(nut->alloc, nut->info, sizeof(nut_info_packet_t), nut->info_count + 1);
			memset(&nut->info[nut->info_count - 1], 0, sizeof(nut_info_packet_t));
			CHECK(get_info_header(nut, &nut->info[nut->info_count - 1]));
			CHECK(skip_reserved_headers(nut, INFO_STARTCODE));
			CHECK(get_bytes(nut->i, 8, &tmp));
		}
		nut->info[nut->info_count].count = -1;
		nut->i->buf_ptr -= 8;
	}
err_out:
	assert(err != NUT_ERR_EAGAIN); // EAGAIN is illegal here!!
	return err;
}

static int add_syncpoint(nut_context_t * nut, syncpoint_t sp, uint64_t * pts, uint64_t * eor, int * out) {
	syncpoint_list_t * sl = &nut->syncpoints;
	int i, j, err = 0;

	assert(nut->dopts.cache_syncpoints & 1 || !pts); // pts information is never stored with no syncpoint cache
	for (i = sl->len; i--; ) { // more often than not, we're adding at end of list
		if (sl->s[i].pos > sp.pos) continue;
		if (sp.pos < sl->s[i].pos + 16) { // syncpoint already in list
			sl->s[i].pos = sp.pos;
			assert(!sl->s[i].pts || sl->s[i].pts == sp.pts);
			sl->s[i].pts = sp.pts;
			if (pts) {
				for (j = 0; j < nut->stream_count; j++) {
					assert(!sl->s[i].pts_valid || sl->pts[i * nut->stream_count + j] == pts[j]);
					sl->pts[i * nut->stream_count + j] = pts[j];
					assert(!sl->s[i].pts_valid || sl->eor[i * nut->stream_count + j] == eor[j]);
					sl->eor[i * nut->stream_count + j] = eor[j];
				}
				sl->s[i].pts_valid = 1;
			}
			if (out) *out = i;
			return 0;
		}
		break;
	}
	i++;
	if (sl->len + 1 > sl->alloc_len) {
		sl->alloc_len += PREALLOC_SIZE/4;
		SAFE_REALLOC(nut->alloc, sl->s, sizeof(syncpoint_t), sl->alloc_len);
		if (nut->dopts.cache_syncpoints & 1) {
			SAFE_REALLOC(nut->alloc, sl->pts, nut->stream_count * sizeof(uint64_t), sl->alloc_len);
			SAFE_REALLOC(nut->alloc, sl->eor, nut->stream_count * sizeof(uint64_t), sl->alloc_len);
		}
	}
	memmove(sl->s + i + 1, sl->s + i, (sl->len - i) * sizeof(syncpoint_t));
	sl->s[i] = sp;
	assert(sl->s[i].pts_valid == !!pts);

	if (nut->dopts.cache_syncpoints & 1) {
		memmove(sl->pts + (i + 1) * nut->stream_count, sl->pts + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));
		memmove(sl->eor + (i + 1) * nut->stream_count, sl->eor + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));
		for (j = 0; j < nut->stream_count; j++) {
			sl->pts[i * nut->stream_count + j] = pts ? pts[j] : 0;
			sl->eor[i * nut->stream_count + j] = eor ? eor[j] : 0;
		}
	}

	sl->len++;
	if (out) *out = i;
err_out:
	return err;
}

static void set_global_pts(nut_context_t * nut, uint64_t pts) {
	int i;
	TO_PTS(timestamp, pts)

	for (i = 0; i < nut->stream_count; i++) {
		nut->sc[i].last_pts = convert_ts(timestamp_p, nut->tb[timestamp_t], TO_TB(i));
	}
}

static int get_syncpoint(nut_context_t * nut) {
	int err = 0;
	syncpoint_t s;
	int after_seek = nut->last_syncpoint ? 0 : 1;
	input_buffer_t itmp, * tmp = new_mem_buffer(&itmp);

	s.pos = bctello(nut->i) - 8;

	if (nut->last_syncpoint == s.pos) after_seek = 1; // don't go through the same syncpoint twice
	nut->last_syncpoint = s.pos;

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, s.pts);
	GET_V(tmp, s.back_ptr);
	s.back_ptr = s.back_ptr * 16 + 15;

	set_global_pts(nut, s.pts);

	s.seen_next = 0;
	s.pts_valid = 0;
	if (nut->dopts.cache_syncpoints & 1) {
		int i;
		uint64_t pts[nut->stream_count];
		uint64_t eor[nut->stream_count];
		for (i = 0; i < nut->stream_count; i++) {
			pts[i] = nut->sc[i].last_key;
			nut->sc[i].last_key = 0;
			eor[i] = nut->sc[i].eor;
			nut->sc[i].eor = 0;
		}
		if (after_seek) CHECK(add_syncpoint(nut, s, NULL, NULL, NULL));
		else {
			s.pts_valid = 1;
			CHECK(add_syncpoint(nut, s, pts, eor, &i));
			nut->syncpoints.s[i - 1].seen_next = 1;
		}
	} else if (nut->dopts.cache_syncpoints) { // we're seeking, we need syncpoint cache
		int i;
		CHECK(add_syncpoint(nut, s, NULL, NULL, &i));
		if (!after_seek) nut->syncpoints.s[i - 1].seen_next = 1;
	}
err_out:
	return err;
}

static int get_index(nut_context_t * nut) {
	input_buffer_t itmp, * tmp = new_mem_buffer(&itmp);
	int err = 0;
	syncpoint_list_t * sl = &nut->syncpoints;
	uint64_t x;
	int i;

	CHECK(get_bytes(nut->i, 8, &x));
	ERROR(x != INDEX_STARTCODE, NUT_ERR_GENERAL_ERROR);

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, x);
	for (i = 0; i < nut->stream_count; i++) {
		TO_PTS(max, x)
		nut->sc[i].sh.max_pts = convert_ts(max_p, nut->tb[max_t], TO_TB(i));
	}

	GET_V(tmp, x);
	sl->alloc_len = sl->len = x;
	SAFE_REALLOC(nut->alloc, sl->s, sizeof(syncpoint_t), sl->alloc_len);
	SAFE_REALLOC(nut->alloc, sl->pts, nut->stream_count * sizeof(uint64_t), sl->alloc_len);
	SAFE_REALLOC(nut->alloc, sl->eor, nut->stream_count * sizeof(uint64_t), sl->alloc_len);

	for (i = 0; i < sl->len; i++) {
		GET_V(tmp, sl->s[i].pos);
		sl->s[i].pos *= 16;
		if (i) sl->s[i].pos += sl->s[i-1].pos;
		sl->s[i].back_ptr = 0;
		sl->s[i].pts = 0;
		sl->s[i].seen_next = 1;
		sl->s[i].pts_valid = 1;
	}
	for (i = 0; i < nut->stream_count; i++) {
		int j;
		uint64_t last_pts = 0; // all of pts[] array is off by one. using 0 for last pts is equivalent to -1 in spec.
		for (j = 0; j < sl->len; ) {
			int type, n, flag;
			GET_V(tmp, x);
			type = x & 1;
			x >>= 1;
			n = j;
			if (type) {
				flag = x & 1;
				x >>= 1;
				while (x--) if (n < sl->len) sl->pts[n++ * nut->stream_count + i] = flag;
				if (n < sl->len) sl->pts[n++ * nut->stream_count + i] = !flag;
			} else {
				while (x != 1) {
					sl->pts[n++ * nut->stream_count + i] = x & 1;
					x >>= 1;
					if (n == sl->len) break;
				}
			}
			for(; j < n; j++) {
				int A, B = 0;
				if (!sl->pts[j * nut->stream_count + i]) continue;
				GET_V(tmp, A);
				if (!A) {
					GET_V(tmp, A);
					GET_V(tmp, B);
					sl->eor[j * nut->stream_count + i] = last_pts + A + B;
				} else sl->eor[j * nut->stream_count + i] = 0;
				sl->pts[j * nut->stream_count + i] = last_pts + A;
				last_pts += A + B;
			}
		}
	}

	fprintf(stderr, "NUT index read successfully, %d syncpoints\n", sl->len);

err_out:
	return err;
}

static void clear_dts_cache(nut_context_t * nut) {
	int i;
	for (i = 0; i < nut->stream_count; i++) {
		int j;
		for (j = 0; j < nut->sc[i].sh.decode_delay; j++) nut->sc[i].pts_cache[j] = -1;
		nut->sc[i].last_dts = -1;
	}
}

static int get_packet(nut_context_t * nut, nut_packet_t * pd, int * saw_syncpoint) {
	nut_info_packet_t info = { 0 };
	uint64_t tmp;
	int err = 0, after_sync = 0, checksum = 0, flags, i;
	off_t start;

	CHECK(get_bytes(nut->i, 1, &tmp)); // frame_code or 'N'
	if (tmp == 'N') {
		CHECK(get_bytes(nut->i, 7, &tmp));
		tmp |= (uint64_t)'N' << 56;
		switch (tmp) {
			case SYNCPOINT_STARTCODE:
				after_sync = 1;
				CHECK(get_syncpoint(nut));
				CHECK(get_bytes(nut->i, 1, &tmp));
				break;
			case MAIN_STARTCODE:
				nut->i->buf_ptr -= 8;
				CHECK(skip_reserved_headers(nut, SYNCPOINT_STARTCODE));
				return -1;
			case INFO_STARTCODE: if (nut->dopts.new_info && !nut->seek_status) {
				CHECK(get_info_header(nut, &info));
				nut->dopts.new_info(nut->dopts.priv, &info);
				return -1;
			} // else - fall through!
			default:
				CHECK(get_header(nut->i, NULL));
				return -1;
		}
	}

	start = bctello(nut->i) - 1;

	flags = nut->ft[tmp].flags;
	ERROR(flags & FLAG_INVALID, NUT_ERR_NOT_FRAME_NOT_N);

	if (flags & FLAG_CODED) {
		int coded_flags;
		GET_V(nut->i, coded_flags);
		flags ^= coded_flags;
	}
	pd->flags = flags & NUT_API_FLAGS;

	if (flags & FLAG_STREAM_ID) GET_V(nut->i, pd->stream);
	else pd->stream = nut->ft[tmp].stream;

	ERROR(pd->stream >= nut->stream_count, NUT_ERR_NOT_FRAME_NOT_N);

	if (flags & FLAG_CODED_PTS) {
		uint64_t coded_pts;
		GET_V(nut->i, coded_pts);
		if (coded_pts >= (1 << nut->sc[pd->stream].msb_pts_shift))
			pd->pts = coded_pts - (1 << nut->sc[pd->stream].msb_pts_shift);
		else {
			int mask, delta;
			mask = (1 << nut->sc[pd->stream].msb_pts_shift)-1;
			delta = nut->sc[pd->stream].last_pts - mask/2;
			pd->pts = ((coded_pts - delta) & mask) + delta;
		}
	} else {
		pd->pts = nut->sc[pd->stream].last_pts + nut->ft[tmp].pts_delta;
	}

	if (flags & FLAG_SIZE_MSB) {
		int size_msb;
		GET_V(nut->i, size_msb);
		pd->len = size_msb * nut->ft[tmp].mul + nut->ft[tmp].lsb;
	} else pd->len = nut->ft[tmp].lsb;

	i = nut->ft[tmp].reserved;
	if (flags & FLAG_RESERVED) GET_V(nut->i, i);
	while (i--) { int scrap; GET_V(nut->i, scrap); }

	if (flags & FLAG_CHECKSUM) {
		CHECK(skip_buffer(nut->i, 4)); // header_checksum
		ERROR(crc32(nut->i->buf_ptr - (bctello(nut->i) - start), bctello(nut->i) - start), NUT_ERR_BAD_CHECKSUM);
		checksum = 1;
	}

	// error checking - max distance
	ERROR(!after_sync && bctello(nut->i) + pd->len - nut->last_syncpoint > nut->max_distance, NUT_ERR_MAX_SYNCPOINT_DISTANCE);
	ERROR(!checksum && pd->len > 2*nut->max_distance, NUT_ERR_MAX_DISTANCE);
	// error checking - max pts distance
	ERROR(!checksum && ABS((int64_t)pd->pts - (int64_t)nut->sc[pd->stream].last_pts) > nut->sc[pd->stream].max_pts_distance, NUT_ERR_MAX_PTS_DISTANCE);
	// error checking - out of order dts
	for (i = 0; i < nut->stream_count; i++) {
		if (nut->sc[i].last_dts == -1) continue;
		if (compare_ts(pd->pts, TO_TB(pd->stream), nut->sc[i].last_dts, TO_TB(i)) < 0)
			fprintf(stderr, "%lld %d (%f) %lld %d (%f) \n",
				pd->pts, pd->stream, TO_DOUBLE(pd->stream, pd->pts),
				nut->sc[i].last_dts, i, TO_DOUBLE(i, nut->sc[i].last_dts));
		ERROR(compare_ts(pd->pts, TO_TB(pd->stream), nut->sc[i].last_dts, TO_TB(i)) < 0, NUT_ERR_OUT_OF_ORDER);
	}

	if (saw_syncpoint) *saw_syncpoint = !!after_sync;
err_out:
	free_info_packet(nut, &info);
	return err;
}

static void push_frame(nut_context_t * nut, nut_packet_t * pd) {
	stream_context_t * sc = &nut->sc[pd->stream];
	sc->last_pts = pd->pts;
	sc->last_dts = get_dts(sc->sh.decode_delay, sc->pts_cache, pd->pts);
	if (pd->flags & NUT_FLAG_KEY && !sc->last_key) sc->last_key = pd->pts + 1;
	if (pd->flags & NUT_FLAG_EOR) sc->eor = pd->pts + 1;
	else sc->eor = 0;
}

static int find_main_headers(nut_context_t * nut) {
	int err = 0;
	uint64_t tmp;
	int read_data = 512*1024;

	// don't waste cpu by running this check every damn time for EAGAIN
	// Except for the first time, to not waste memory
	if (!nut->seek_status && ready_read_buf(nut->i, read_data) < read_data && buf_eof(nut->i) == NUT_ERR_EAGAIN)
		return NUT_ERR_EAGAIN;

	CHECK(get_bytes(nut->i, 7, &tmp));
	read_data -= 7;
	while (read_data--) {
		ERROR(ready_read_buf(nut->i, 30) < 1, buf_eof(nut->i));
		tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		if (tmp == MAIN_STARTCODE) break;
		// give up if we reach a syncpoint, unless we're searching the file end
		if (tmp == SYNCPOINT_STARTCODE && nut->seek_status != 18) break;
	}
	if (tmp == MAIN_STARTCODE) {
		off_t pos = bctello(nut->i) - 8;
		// load all headers into memory so they can be cleanly decoded without EAGAIN issues
		// also check validity of the headers we just found
		do {
			if ((err = get_header(nut->i, NULL)) == NUT_ERR_EAGAIN) goto err_out;
			if (err) { tmp = err = 0; break; } // bad

			if ((err = get_bytes(nut->i, 8, &tmp)) == NUT_ERR_EAGAIN) goto err_out;
			assert(!err || err == NUT_ERR_EOF); // the only possibilities
			// EOF is a legal error here - when reading the last headers in file
			if (err == NUT_ERR_EOF) { err = 0; tmp = SYNCPOINT_STARTCODE; }
		} while (tmp != SYNCPOINT_STARTCODE);
		if (tmp == SYNCPOINT_STARTCODE) { // success!
			nut->last_headers = pos;
			nut->i->buf_ptr = get_buf(nut->i, nut->last_headers);
			flush_buf(nut->i);
			return 0;
		}
	}

	// failure
	if (!nut->i->isc.seek) return NUT_ERR_NO_HEADERS;
	if (!nut->seek_status) {
		nut->seek_status = 18; // start search at 512kb
		// but first, let's check EOF
		seek_buf(nut->i, -512*1024, SEEK_END);
		return find_main_headers(nut);
	}
	seek_buf(nut->i, 1 << ++nut->seek_status, SEEK_SET);
	// evantually we'll hit EOF and give up
	return find_main_headers(nut);
err_out:
	return err;
}

static int find_syncpoint(nut_context_t * nut, int backwards, syncpoint_t * res, off_t stop) {
	int read;
	int err = 0;
	uint64_t tmp;
	off_t ptr = 0;
	assert(!backwards || !stop); // can't have both
retry:
	read = nut->max_distance;
	if (stop) read = MIN(read, stop - bctello(nut->i));
	read = ready_read_buf(nut->i, read);
	if (stop) read = MIN(read, stop - bctello(nut->i));
	tmp = 0;

	while (nut->i->buf_ptr - nut->i->buf < read) {
		tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		if (tmp != SYNCPOINT_STARTCODE) continue;
		if (res) {
			input_buffer_t itmp, * tmp = new_mem_buffer(&itmp);
			res->pos = bctello(nut->i) - 8;

			if ((err = get_header(nut->i, tmp)) == NUT_ERR_EAGAIN) goto err_out;
			if (err) { err = 0; continue; }

			GET_V(tmp, res->pts);
			GET_V(tmp, res->back_ptr);
			res->back_ptr = res->back_ptr * 16 + 15;
			res->seen_next = 0;
			res->pts_valid = 0;
		}
		if (!backwards) return 0;
		else ptr = bctello(nut->i);
	}

	if (ptr) {
		nut->i->buf_ptr -= bctello(nut->i) - ptr;
		return 0;
	}

	if (stop && bctello(nut->i) >= stop) {
		if (res) res->seen_next = 1;
		return 0;
	}

	if (read < nut->max_distance) return buf_eof(nut->i); // too little read

	if (backwards) {
		nut->i->buf_ptr = nut->i->buf;
		seek_buf(nut->i, -(nut->max_distance - 7), SEEK_CUR);
	} else {
		nut->i->buf_ptr -= 7; // repeat on last 7 bytes
		if (nut->i->buf_ptr < nut->i->buf) nut->i->buf_ptr = nut->i->buf;
		flush_buf(nut->i);
	}

	goto retry;
err_out:
	return err;
}

int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd) {
	int err = 0;
	if (nut->seek_status) { // in error mode!
		syncpoint_t s;
		CHECK(find_syncpoint(nut, 0, &s, 0));
		nut->i->buf_ptr -= bctello(nut->i) - s.pos; // go back to begginning of syncpoint
		flush_buf(nut->i);
		clear_dts_cache(nut);
		nut->last_syncpoint = 0;
		nut->seek_status = 0;
	}

	while ((err = get_packet(nut, pd, NULL)) == -1) flush_buf(nut->i);
	if (err > NUT_ERR_OUT_OF_MEM) { // some error occured!
		fprintf(stderr, "NUT: %s\n", nut_error(err));
		// rewind as much as possible
		if (nut->i->isc.seek) seek_buf(nut->i, nut->last_syncpoint + 8, SEEK_SET);
		else nut->i->buf_ptr = nut->i->buf;

		nut->seek_status = 1; // enter error mode
		return nut_read_next_packet(nut, pd);
	}

	if (!err) push_frame(nut, pd);
err_out:
	if (err != NUT_ERR_EAGAIN) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

int nut_read_headers(nut_context_t * nut, nut_stream_header_t * s [], nut_info_packet_t * info []) {
	int i, err = 0;
	uint64_t tmp;
	syncpoint_t sp;
	if (!nut->sc) { // we already have headers, we were called just for index
		if (!nut->last_headers) CHECK(find_main_headers(nut));

		CHECK(get_headers(nut, !!info));

		if (nut->dopts.read_index) { // check for index right after main headers
			CHECK(skip_reserved_headers(nut, INDEX_STARTCODE));
			CHECK(get_bytes(nut->i, 8, &tmp));
			nut->i->buf_ptr -= 8;
			if (tmp == INDEX_STARTCODE) nut->seek_status = 2; // signals to not seek to find index
			flush_buf(nut->i);
		}
	}

	if (nut->dopts.read_index & 1) { // we already have index, we were called just for the final syncpoint search
		uint64_t idx_ptr;
		if (nut->seek_status <= 1) {
			if (nut->seek_status == 0) {
				nut->before_seek = bctello(nut->i);
				seek_buf(nut->i, -12, SEEK_END);
			}
			nut->seek_status = 1;
			CHECK(get_bytes(nut->i, 8, &idx_ptr));
			if (idx_ptr) idx_ptr = nut->i->filesize - idx_ptr;
			if (!idx_ptr || idx_ptr >= nut->i->filesize) nut->dopts.read_index = 0; // invalid ptr
		}
		if (nut->dopts.read_index) {
			if (nut->seek_status == 1) seek_buf(nut->i, idx_ptr, SEEK_SET);
			nut->seek_status = 2;
			// only EAGAIN from get_index is interesting
			if ((err = get_index(nut)) == NUT_ERR_EAGAIN) goto err_out;
			if (err) nut->dopts.read_index = 0;
			else nut->dopts.read_index = 2;
			err = 0;
		}
		if (nut->before_seek) seek_buf(nut->i, nut->before_seek, SEEK_SET);
		nut->before_seek = 0;
	}

	// last step - find the first syncpoint in file
	if (nut->last_headers > 1024 && !nut->seek_status) {
		// the headers weren't found in begginning of file
		assert(nut->i->isc.seek);
		seek_buf(nut->i, 0, SEEK_SET);
		nut->seek_status = 1;
	}
	CHECK(find_syncpoint(nut, 0, &sp, 0));
	CHECK(add_syncpoint(nut, sp, NULL, NULL, NULL));
	nut->i->buf_ptr = get_buf(nut->i, sp.pos); // rewind to the syncpoint, this is where playback starts...
	nut->seek_status = 0;

	SAFE_CALLOC(nut->alloc, *s, sizeof(nut_stream_header_t), nut->stream_count + 1);
	for (i = 0; i < nut->stream_count; i++) (*s)[i] = nut->sc[i].sh;
	(*s)[i].type = -1;
	nut->tmp_buffer = (void*)*s;
	if (info) *info = nut->info;
err_out:
	if (err != NUT_ERR_EAGAIN) flush_buf(nut->i); // unless EAGAIN
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

static int binary_search_syncpoint(nut_context_t * nut, double time_pos, off_t * start, off_t * end, syncpoint_t * stopper) {
	int i, err = 0;
	syncpoint_t s;
	off_t hi, lo;
	uint64_t hip, lop;
	uint64_t timebases[nut->timebase_count];
	syncpoint_list_t * sl = &nut->syncpoints;
	int a = 0;

	for (i = 0; i < nut->timebase_count; i++) timebases[i] = (uint64_t)(time_pos / nut->tb[i].nom * nut->tb[i].den);

	assert(sl->len); // it is impossible for the first syncpoint to not have been read

	// find last syncpoint if it's not already found
	if (!sl->s[sl->len-1].seen_next) {
		// searching bakwards from EOF
		if (!nut->seek_status) seek_buf(nut->i, -nut->max_distance, SEEK_END);
		nut->seek_status = 1;
		CHECK(find_syncpoint(nut, 1, &s, 0));
		CHECK(add_syncpoint(nut, s, NULL, NULL, &i));
		assert(i == sl->len-1);
		sl->s[i].seen_next = 1;
		nut->seek_status = 0;
	}
	// sl->len MUST be >=2, which is the first and last syncpoints in the file
	ERROR(sl->len < 2, NUT_ERR_NOT_SEEKABLE);

	for (i = 0; i < sl->len; i++) {
		TO_PTS(tmp, sl->s[i].pts)
		if (timebases[tmp_t] <= tmp_p) break;
	}

	if (i == sl->len) { // there isn't any syncpoint bigger than requested
		seek_buf(nut->i, 0, SEEK_END); // intentionally seeking to EOF
		err = 1; // EOF
		goto err_out;
	}
	if (i == 0) { // there isn't any syncpoint smaller than requested
		seek_buf(nut->i, sl->s[0].pos, SEEK_SET); // seek to first syncpoint
		clear_dts_cache(nut);
		nut->last_syncpoint = 0;
		goto err_out;
	}

	i--;

	lo = sl->s[i].pos;
	lop = sl->s[i].pts;
	hi = sl->s[i+1].pos;
	hip = sl->s[i+1].pts;
	if (nut->seek_status) hi = nut->seek_status;

	while (!sl->s[i].seen_next) {
		// start binary search between sl->s[i].pos (lo) to sl->s[i+1].pos (hi) ...
		off_t guess;
		int res, tmp;
		double hi_pd = TO_DOUBLE_PTS(hip);
		double lo_pd = TO_DOUBLE_PTS(lop);
		a++;
		if (hi - lo < nut->max_distance*2) guess = lo + 16;
		else { // linear interpolation
#define INTERPOLATE_WEIGHT (19./20)
			double a = (double)(hi - lo) / (hi_pd - lo_pd);
			guess = lo + a * (time_pos - lo_pd);
			guess = guess * INTERPOLATE_WEIGHT + (lo+hi)/2 * (1 - INTERPOLATE_WEIGHT);
			if (hi - guess < nut->max_distance*2) guess = hi - nut->max_distance*2; //(lo + hi)/2;
		}
		if (guess < lo + 8) guess = lo + 16;
		fprintf(stderr, "\n%d [ (%d,%.3f) .. (%d,%.3f) .. (%d,%.3f) ] ", i, (int)lo, lo_pd, (int)guess, time_pos, (int)hi, hi_pd);
		if (!nut->seek_status) seek_buf(nut->i, guess, SEEK_SET);
		nut->seek_status = hi; // so we know where to continue off...
		CHECK(find_syncpoint(nut, 0, &s, hi));
		nut->seek_status = 0;

		if (s.seen_next == 1 || s.pos >= hi) { // we got back to 'hi'
			// either we scanned everything from lo to hi, or we keep trying
			if (guess <= lo + 16) sl->s[i].seen_next = 1; // we are done!
			else hi = guess;
			continue;
		}

		res = (s.pts / nut->timebase_count > timebases[s.pts % nut->timebase_count]);
		if (res) {
			hi = s.pos;
			hip = s.pts;
		} else {
			lo = s.pos;
			lop = s.pts;
		}
		CHECK(add_syncpoint(nut, s, NULL, NULL, &tmp)); // unconditionally even without syncpoint cache
		if (!res) i = tmp;
	}

	fprintf(stderr, "\n[ (%d,%d) .. %d .. (%d,%d) ] => %d (%d seeks) %d\n",
		(int)lo, (int)lop, (int)timebases[0], (int)hi, (int)hip, (int)(lo - sl->s[i].back_ptr), a, sl->s[i].back_ptr);
	// at this point, s[i].pts < P < s[i+1].pts, and s[i].flag is set
	// meaning, there are no more syncpoints between s[i] to s[i+1]
	*start = sl->s[i].pos - sl->s[i].back_ptr;
	*end = sl->s[i+1].pos;
	*stopper = sl->s[i+1];
err_out:
	if (err == NUT_ERR_EAGAIN) nut->i->buf_ptr = nut->i->buf;
	return err;
}

static int linear_search_seek(nut_context_t * nut, int backwards, off_t start, off_t end, syncpoint_t * stopper) {
	syncpoint_list_t * sl = &nut->syncpoints;
	int i, err = 0;
	off_t min_pos = 0;
	off_t buf_before = 0;
	off_t stopper_syncpoint = 0;

	if (nut->seek_status <= 1) {
		syncpoint_t s;
		if (!nut->seek_status) seek_buf(nut->i, start, SEEK_SET);
		nut->seek_status = 1;
		// find closest syncpoint by linear search, SHOULD be one pointed by back_ptr...
		buf_before = bctello(nut->i);
		CHECK(find_syncpoint(nut, 0, &s, 0));
		clear_dts_cache(nut);
		nut->last_syncpoint = 0; // last_key is invalid
		seek_buf(nut->i, s.pos, SEEK_SET); // go back to syncpoint. This will not need a seek.
		nut->seek_status = s.pos << 1;
		if (s.pos > start + 15) goto err_out; // error condition, we didn't get the syncpoint we wanted
	}

	if (stopper) {
		off_t back_ptr = stopper->pos - stopper->back_ptr;
		for (i = 1; i < sl->len; i++) {
			if (sl->s[i].pos > back_ptr + 15) {
				// no need to worry about this being exact or not - if we're using stopper, that means we don't have an index
				if (sl->s[i-1].seen_next) stopper_syncpoint = sl->s[i].pos;
				break;
			}
		}
		if (stopper_syncpoint > bctello(nut->i)) stopper_syncpoint = 0; // don't premature
	}

	if (!(nut->seek_status & 1)) while (bctello(nut->i) < end || !end) {
		int saw_syncpoint;
		nut_packet_t pd;

		buf_before = bctello(nut->i);
		err = get_packet(nut, &pd, &saw_syncpoint); // FIXME we're counting on syncpoint cache!! for the good_key later, and stopper_syncpoint
		if (err == -1) continue;
		CHECK(err);

		if (saw_syncpoint) {
			if (stopper && !stopper_syncpoint && buf_before > stopper->pos - stopper->back_ptr + 15) {
				int n = 1;
				stopper_syncpoint = buf_before;
				for (i = 0; i < nut->stream_count; i++) if (!nut->sc[i].state.active && nut->sc[i].state.good_key) n = 0;
				if (n) break; // no inactive streams have keyframes between stopper and stopper_syncpoint, stop now
			}
			if (buf_before != stopper_syncpoint) {  // flush at every syncpoint - except stopper_syncpoint
								// give it a chance, we might be able to do this in a single seek
				int header_size = bctello(nut->i) - buf_before;
				nut->i->buf_ptr -= header_size;
				flush_buf(nut->i);
				nut->i->buf_ptr += header_size;
			}
		}

		if (nut->sc[pd.stream].state.active) {
			if (end && pd.pts > nut->sc[pd.stream].state.pts) { // higher than requested pts
				int n = 1;
				nut->sc[pd.stream].state.pts_higher = 1;
				for (i = 0; i < nut->stream_count; i++) if (nut->sc[i].state.active && !nut->sc[i].state.pts_higher) n = 0;
				if (n) break; // pts for all active streams higher than requested pts
			}
			if (pd.flags & NUT_FLAG_KEY) {
				if (pd.pts <= nut->sc[pd.stream].state.pts) {
					nut->sc[pd.stream].state.good_key = buf_before;
					if (pd.flags & NUT_FLAG_EOR) nut->sc[pd.stream].state.good_key = 0;
				}
				if (!end && pd.pts >= nut->sc[pd.stream].state.pts) { // forward seek end
					nut->i->buf_ptr -= bctello(nut->i) - buf_before;
					break;
				}
			}
		} else if (stopper && pd.flags&NUT_FLAG_KEY) {
			off_t back_ptr = stopper->pos - stopper->back_ptr;
			TO_PTS(stopper, stopper->pts)
			// only relavent if pts is smaller than stopper, and we passed stopper's back_ptr
			if (compare_ts(pd.pts, TO_TB(pd.stream), stopper_p, nut->tb[stopper_t]) < 0 && buf_before >= back_ptr) {
				if (!stopper_syncpoint) nut->sc[pd.stream].state.good_key = 1;
				else if (nut->sc[pd.stream].state.good_key) {
					int n = 1;
					nut->sc[pd.stream].state.good_key = 0;
					for (i = 0; i < nut->stream_count; i++) if (!nut->sc[i].state.active && nut->sc[i].state.good_key) n = 0;
					// smart linear search stop
					// keyframe for every inactive stream (which had a keyframe in stopper area), after stopper_syncpoint
					if (n) break;
				}
			}
		}
		// dts higher than requested pts
		if (end && peek_dts(nut->sc[pd.stream].sh.decode_delay, nut->sc[pd.stream].pts_cache, pd.pts) > (int64_t)nut->sc[pd.stream].state.pts) break;

		CHECK(skip_buffer(nut->i, pd.len));
		push_frame(nut, &pd);
	}
	if (!end) goto err_out; // forward seek

	for (i = 0; i < nut->stream_count; i++) {
		if (!nut->sc[i].state.active) continue;
		if (nut->sc[i].state.good_key && (!min_pos || nut->sc[i].state.good_key < min_pos)) min_pos = nut->sc[i].state.good_key;
	}
	if (!min_pos) {
		fprintf(stderr, "BIG FAT WARNING\n");
		for (i = 0; i < nut->stream_count; i++) fprintf(stderr, "%d: %d\n", i, (int)nut->sc[i].state.good_key);
		min_pos = nut->seek_status >> 1;
	}

	// after ALL this, we ended up in a worse position than where we were...
	ERROR(!backwards && min_pos < nut->before_seek, NUT_ERR_NOT_SEEKABLE);

	// FIXME we're counting on syncpoint cache dopts.cache_syncpoints
	for (i = 1; i < sl->len; i++) if (sl->s[i].pos > min_pos) break;
	i--;
	if (!(nut->seek_status & 1)) seek_buf(nut->i, sl->s[i].pos, SEEK_SET);

	nut->seek_status |= 1;
	nut->last_syncpoint = 0; // last_key is invalid
	clear_dts_cache(nut);
	buf_before = bctello(nut->i);

	while (bctello(nut->i) < min_pos) {
		nut_packet_t pd;
		while ((err = get_packet(nut, &pd, NULL)) == -1);
		CHECK(err);
		push_frame(nut, &pd);
		CHECK(skip_buffer(nut->i, pd.len));
	}

err_out:
	if (err != NUT_ERR_EAGAIN) { // unless EAGAIN
		if (err) {
			if (err == NUT_ERR_NOT_SEEKABLE) { // a failed seek - then go back to before everything started
				for (i = 0; i < nut->stream_count; i++) nut->sc[i].last_pts = nut->sc[i].state.old_last_pts;
				seek_buf(nut->i, nut->before_seek, SEEK_SET);
			} else { // some NUT error, let's just go back to last good syncpoint
				err = 0;
				seek_buf(nut->i, nut->seek_status >> 1, SEEK_SET);
			}
			nut->last_syncpoint = 0; // last_key is invalid
			clear_dts_cache(nut);
		}
		nut->seek_status = 0;
	} else if (buf_before) {
		nut->i->buf_ptr -= bctello(nut->i) - buf_before; // rewind
		if (nut->i->buf_ptr < nut->i->buf) nut->i->buf_ptr = nut->i->buf; // special case, possible with find_syncpoint
	}
	return err;
}

int nut_seek(nut_context_t * nut, double time_pos, int flags, const int * active_streams) {
	int err = 0;
	off_t start = 0, end = 0;
	int backwards = flags & 1 ? time_pos < 0 : 1;
	syncpoint_t stopper = { 0, 0, 0, 0, 0 };

	if (!nut->i->isc.seek) return NUT_ERR_NOT_SEEKABLE;

	if (!nut->before_seek) {
		int i;
		nut->before_seek = bctello(nut->i);

		for (i = 0; i < nut->stream_count; i++) {
			nut->sc[i].state.old_last_pts = nut->sc[i].last_pts;
			nut->sc[i].state.active = active_streams ? 0 : 1;
			nut->sc[i].state.good_key = nut->sc[i].state.pts_higher = 0;
		}
		if (active_streams) for (i = 0; active_streams[i] != -1; i++) nut->sc[active_streams[i]].state.active = 1;

		if (flags & 1) { // relative seek
			uint64_t orig_pts = 0;
			int orig_timebase = 0;
			for (i = 0; i < nut->stream_count; i++) {
				uint64_t dts = nut->sc[i].last_dts != -1 ? nut->sc[i].last_dts : nut->sc[i].last_pts;
				if (!nut->sc[i].state.active) continue;
				if (compare_ts(orig_pts, nut->tb[orig_timebase], dts, TO_TB(i)) < 0) {
					orig_pts = dts;
					orig_timebase = nut->sc[i].timebase_id;
				}
			}
			time_pos += TO_DOUBLE(orig_timebase, orig_pts);
		}
		if (time_pos < 0.) time_pos = 0.;

		for (i = 0; i < nut->stream_count; i++) nut->sc[i].state.pts = (uint64_t)(time_pos / TO_TB(i).nom * TO_TB(i).den);
		nut->seek_time_pos = time_pos;
		nut->dopts.cache_syncpoints |= 2;
	} else {
		time_pos = nut->seek_time_pos;
	}

	if (nut->syncpoints.len) {
		syncpoint_list_t * sl = &nut->syncpoints;
		int i;
		int sync[nut->stream_count];
		int good_sync = -2;
		int last_sync = 0;
		int backup = -1;
		for (i = 0; i < nut->stream_count; i++) sync[i] = -1;

		for (i = 1; i < sl->len; i++) {
			int j;
			if (!sl->s[i].pts_valid) continue;
			for (j = 0; j < nut->stream_count; j++) {
				uint64_t tmp;
				if (!nut->sc[j].state.active) continue;
				tmp = sl->pts[i * nut->stream_count + j];
				if (tmp--) { // -- because all pts array is off-by-one. zero indicate no keyframe.
					if (tmp > nut->sc[j].state.pts) { if (!last_sync) last_sync = i; }
					else sync[j] = (i-1);
				}
				tmp = sl->eor[i * nut->stream_count + j];
				if (tmp--) if (tmp <= nut->sc[j].state.pts) sync[j] = -(i+1); // flag stream eor
			}
		}
		for (i = 0; i < nut->stream_count; i++) {
			if (!nut->sc[i].state.active) continue;
			if (sync[i] < -1) { backup = MAX(backup, -sync[i] - 1); continue; } // eor stream
			if (good_sync == -2 || good_sync > sync[i]) good_sync = sync[i];
		}
		if (good_sync == -2) good_sync = backup; // all active streams are eor, just pick a random point, sort of.

		if (sl->s[sl->len-1].seen_next && last_sync && good_sync >= 0) {
			for (i = good_sync; i <= last_sync; i++) if (!sl->s[i].pts_valid) break;
			if (i != last_sync+1 && good_sync <= last_sync) good_sync = -1;
		} else good_sync = -1;
		if (good_sync >= 0) {
			start = sl->s[good_sync].pos;
			end = sl->s[++good_sync].pos;
			if (flags & 2) end = sl->s[last_sync - 1].pos; // for forward seek
		}
	}

	if (start == 0) CHECK(binary_search_syncpoint(nut, time_pos, &start, &end, &stopper));
	else fprintf(stderr, "============= NO BINARY SEARCH   \n");

	if (start) { // "unsuccessful" seek needs no linear search
		if (!(flags & 2)) { // regular seek
			CHECK(linear_search_seek(nut, backwards, start, end, stopper.pos ? &stopper : NULL));
		} else { // forwards seek, find keyframe
			CHECK(linear_search_seek(nut, backwards, end, 0, NULL));
		}
	}
	fprintf(stderr, "DONE SEEK\n");
err_out:
	if (err != NUT_ERR_EAGAIN) { // unless EAGAIN
		syncpoint_list_t * sl = &nut->syncpoints;
		flush_buf(nut->i);
		nut->before_seek = 0;
		nut->dopts.cache_syncpoints &= ~2;
		if (!nut->dopts.cache_syncpoints && sl->len > 1) {
			sl->s[1] = sl->s[sl->len - 1];
			sl->len = 2;
			sl->s[0].seen_next = 0;
		}
	}
	return err;
}

nut_context_t * nut_demuxer_init(nut_demuxer_opts_t * dopts) {
	nut_context_t * nut;

	if (dopts->alloc.malloc) nut = dopts->alloc.malloc(sizeof(nut_context_t));
	else nut = malloc(sizeof(nut_context_t));

	if (!nut) return NULL;

	nut->syncpoints.len = 0;
	nut->syncpoints.alloc_len = 0;
	nut->syncpoints.s = NULL;
	nut->syncpoints.pts = NULL;
	nut->syncpoints.eor = NULL;

	nut->sc = NULL;
	nut->tb = NULL;
	nut->info = NULL;
	nut->tmp_buffer = NULL; // the caller's allocated stream list
	nut->last_headers = 0;
	nut->stream_count = 0;
	nut->info_count = 0;
	nut->dopts = *dopts;
	nut->seek_status = 0;
	nut->before_seek = 0;
	nut->last_syncpoint = 0;

	nut->alloc = &nut->dopts.alloc;

	if (!nut->alloc->malloc) {
		nut->alloc->malloc = malloc;
		nut->alloc->realloc = realloc;
		nut->alloc->free = free;
	}

	nut->i = new_input_buffer(nut->alloc, dopts->input);

	if (!nut->i) {
		nut->alloc->free(nut);
		return NULL;
	}

	// use only lsb for options
	nut->dopts.cache_syncpoints = !!nut->dopts.cache_syncpoints;
	nut->dopts.read_index = !!nut->dopts.read_index;
	if (!nut->i->isc.seek) {
		nut->dopts.cache_syncpoints = 0;
		nut->dopts.read_index = 0;
	}
	if (nut->dopts.read_index) nut->dopts.cache_syncpoints = 1;

	return nut;
}

void nut_demuxer_uninit(nut_context_t * nut) {
	int i;
	if (!nut) return;
	for (i = 0; i < nut->stream_count; i++) {
		nut->alloc->free(nut->sc[i].sh.fourcc);
		nut->alloc->free(nut->sc[i].sh.codec_specific);
		nut->alloc->free(nut->sc[i].pts_cache);
	}
	for (i = 0; i < nut->info_count; i++) free_info_packet(nut, &nut->info[i]);

	nut->alloc->free(nut->syncpoints.s);
	nut->alloc->free(nut->syncpoints.pts);
	nut->alloc->free(nut->syncpoints.eor);
	nut->alloc->free(nut->sc);
	nut->alloc->free(nut->tmp_buffer); // the caller's allocated stream list
	nut->alloc->free(nut->info);
	nut->alloc->free(nut->tb);
	free_buffer(nut->i);
	nut->alloc->free(nut);
}

const char * nut_error(int error) {
	switch((enum nut_errors)error) {
		case NUT_ERR_NO_ERROR: return "No error.";
		case NUT_ERR_EOF: return "Unexpected EOF.";
		case NUT_ERR_EAGAIN: return "Not enough data given and no EOF.";
		case NUT_ERR_GENERAL_ERROR: return "General Error.";
		case NUT_ERR_BAD_VERSION: return "Bad NUT Version.";
		case NUT_ERR_NOT_FRAME_NOT_N: return "Invalid Framecode.";
		case NUT_ERR_BAD_CHECKSUM: return "Bad Checksum.";
		case NUT_ERR_MAX_SYNCPOINT_DISTANCE: return "max_distance syncpoint";
		case NUT_ERR_MAX_DISTANCE: return "max_distance";
		case NUT_ERR_NO_HEADERS: return "No headers found!";
		case NUT_ERR_NOT_SEEKABLE: return "Cannot seek to that position.";
		case NUT_ERR_OUT_OF_ORDER: return "out of order dts";
		case NUT_ERR_MAX_PTS_DISTANCE: return "Pts difference higher than max_pts_distance.";
		case NUT_ERR_BAD_STREAM_ORDER: return "Stream headers are stored in wrong order.";
		case NUT_ERR_NOSTREAM_STARTCODE: return "Expected stream startcode not found.";
		case NUT_ERR_BAD_EOF: return "Invalid forward_ptr!";
		case NUT_ERR_VLC_TOO_LONG: return "VLC too long";
		case NUT_ERR_OUT_OF_MEM: return "Out of memory";
	}
	return NULL;
}
