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

static void flush_buf(input_buffer_tt *bc) {
	assert(!bc->is_mem);
	bc->file_pos += bc->buf_ptr - bc->buf;
	bc->read_len -= bc->buf_ptr - bc->buf;
	memmove(bc->buf, bc->buf_ptr, bc->read_len);
	bc->buf_ptr = bc->buf;
}

static int ready_read_buf(input_buffer_tt * bc, int amount) {
	int pos = (bc->buf_ptr - bc->buf);
	if (bc->read_len - pos < amount && !bc->is_mem) {
		amount += 10;
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

static void seek_buf(input_buffer_tt * bc, long long pos, int whence) {
	assert(!bc->is_mem);
	if (whence != SEEK_END) {
		// don't do anything when already in seeked position. but still flush_buf
		off_t req = pos + (whence == SEEK_CUR ? bctello(bc) : 0);
		if (req >= bc->file_pos && req <= bc->file_pos + bc->read_len) {
			bc->buf_ptr = bc->buf + (req - bc->file_pos);
			flush_buf(bc);
			return;
		}
	}
	if (whence == SEEK_CUR) pos -= bc->read_len - (bc->buf_ptr - bc->buf);
	debug_msg("seeking %d ", (int)pos);
	switch (whence) {
		case SEEK_SET: debug_msg("SEEK_SET   "); break;
		case SEEK_CUR: debug_msg("SEEK_CUR   "); break;
		case SEEK_END: debug_msg("SEEK_END   "); break;
	}
	bc->file_pos = bc->isc.seek(bc->isc.priv, pos, whence);
	bc->buf_ptr = bc->buf;
	bc->read_len = 0;
	if (whence == SEEK_END) bc->filesize = bc->file_pos - pos;
}

static int buf_eof(input_buffer_tt * bc) {
	if (bc->is_mem) return NUT_ERR_BAD_EOF;
	if (!bc->alloc) return NUT_ERR_OUT_OF_MEM;
	if (!bc->isc.eof || bc->isc.eof(bc->isc.priv)) return NUT_ERR_EOF;
	return NUT_ERR_EAGAIN;
}

static int skip_buffer(input_buffer_tt * bc, int len) {
	if (ready_read_buf(bc, len) < len) return buf_eof(bc);
	bc->buf_ptr += len;
	return 0;
}

static uint8_t * get_buf(input_buffer_tt * bc, off_t start) {
	start -= bc->file_pos;
	assert((unsigned)start < bc->read_len);
	return bc->buf + start;
}

static input_buffer_tt * new_mem_buffer(input_buffer_tt * bc) {
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

static input_buffer_tt * new_input_buffer(nut_alloc_tt * alloc, nut_input_stream_tt isc) {
	input_buffer_tt * bc = new_mem_buffer(alloc->malloc(sizeof(input_buffer_tt)));
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

static void free_buffer(input_buffer_tt * bc) {
	if (!bc) return;
	assert(!bc->is_mem);
	bc->alloc->free(bc->buf);
	bc->alloc->free(bc);
}

static int get_bytes(input_buffer_tt * bc, int count, uint64_t * val) {
	int i;
	if (ready_read_buf(bc, count) < count) return buf_eof(bc);
	*val = 0;
	for (i = 0; i < count; i++) {
		*val = (*val << 8) | *(bc->buf_ptr++);
	}
	return 0;
}

static int get_v(input_buffer_tt * bc, uint64_t * val) {
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

static int get_s(input_buffer_tt * bc, int64_t * val) {
	uint64_t tmp;
	int err;
	if ((err = get_v(bc, &tmp))) return err;
	tmp++;
	if (tmp & 1) *val = -(tmp >> 1);
	else         *val =  (tmp >> 1);
	return 0;
}

#ifdef TRACE
static int get_v_trace(input_buffer_tt * bc, uint64_t * val, char * var, char * file, int line, char * func) {
	int a = get_v(bc, val);
	printf("GET_V %llu to var `%s' at %s:%d, %s() (ret: %d)\n", *val, var, file, line, func, a);
	return a;
}

static int get_s_trace(input_buffer_tt * bc, int64_t * val, char * var, char * file, int line, char * func) {
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

static int get_data(input_buffer_tt * bc, int len, uint8_t * buf) {
	int tmp;

	if (!len) return 0;
	assert(buf);

	tmp = ready_read_buf(bc, len);

	len = MIN(len, tmp);
	memcpy(buf, bc->buf_ptr, len);
	bc->buf_ptr += len;

	return len;
}

static int get_vb(nut_alloc_tt * alloc, input_buffer_tt * in, int * len, uint8_t ** buf) {
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

static int get_header(input_buffer_tt * in, input_buffer_tt * out) {
	off_t start = bctello(in) - 8; // startcode
	int forward_ptr;
	int err = 0;

	GET_V(in, forward_ptr);
	if (forward_ptr > 4096) {
		CHECK(skip_buffer(in, 4)); // header_checksum
		ERROR(crc32(get_buf(in, start), bctello(in) - start), NUT_ERR_BAD_CHECKSUM);
	}
	start = bctello(in);

	CHECK(skip_buffer(in, forward_ptr));
	ERROR(crc32(get_buf(in, start), forward_ptr), NUT_ERR_BAD_CHECKSUM);

	if (out) {
		assert(out->is_mem);
		assert(out->buf == out->buf_ptr);
		out->buf_ptr = out->buf = get_buf(in, start);
		out->write_len = out->read_len = forward_ptr - 4; // not including checksum
	}
err_out:
	return err;
}

static int get_main_header(nut_context_tt * nut) {
	input_buffer_tt itmp, * tmp = new_mem_buffer(&itmp);
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
	ERROR(SIZE_MAX/sizeof(nut_timebase_tt) < nut->timebase_count, NUT_ERR_OUT_OF_MEM);
	nut->tb = nut->alloc->malloc(nut->timebase_count * sizeof(nut_timebase_tt));
	ERROR(!nut->tb, NUT_ERR_OUT_OF_MEM);
	for (i = 0; i < nut->timebase_count; i++) {
		GET_V(tmp, nut->tb[i].num);
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

static int get_stream_header(nut_context_tt * nut) {
	input_buffer_tt itmp, * tmp = new_mem_buffer(&itmp);
	stream_context_tt * sc;
	int i, err = 0;

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, i);
	ERROR(i > nut->stream_count, NUT_ERR_BAD_STREAM_ORDER);
	sc = &nut->sc[i];
	if (sc->sh.type != -1) return 0; // we've already taken care of this stream

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
			GET_V(tmp, sc->sh.colorspace_type);
			break;
		case NUT_AUDIO_CLASS:
			GET_V(tmp, sc->sh.samplerate_num);
			GET_V(tmp, sc->sh.samplerate_denom);
			GET_V(tmp, sc->sh.channel_count);
			break;
	}

	SAFE_CALLOC(nut->alloc, sc->pts_cache, sizeof(int64_t), sc->sh.decode_delay);
	for (i = 0; i < sc->sh.decode_delay; i++) sc->pts_cache[i] = -1;
err_out:
	return err;
}

static void free_info_packet(nut_context_tt * nut, nut_info_packet_tt * info) {
	int i;
	for (i = 0; i < info->count; i++) nut->alloc->free(info->fields[i].data);
	nut->alloc->free(info->fields);
}

static int get_info_header(nut_context_tt * nut, nut_info_packet_tt * info) {
	input_buffer_tt itmp, * tmp = new_mem_buffer(&itmp);
	int i, err = 0;
	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, info->stream_id_plus1);
	GET_V(tmp, info->chapter_id);
	GET_V(tmp, info->chapter_start);
	info->chapter_tb = nut->tb[info->chapter_start % nut->timebase_count];
	info->chapter_start /= nut->timebase_count;
	GET_V(tmp, info->chapter_len);

	GET_V(tmp, info->count);
	SAFE_CALLOC(nut->alloc, info->fields, sizeof(nut_info_field_tt), info->count);

	for (i = 0; i < info->count; i++) {
		int len;
		nut_info_field_tt * field = &info->fields[i];

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

static void add_existing_syncpoint(nut_context_tt * nut, syncpoint_tt sp, uint64_t * pts, uint64_t * eor, int i) {
	syncpoint_list_tt * sl = &nut->syncpoints;
	int j;
	int pts_cache = nut->dopts.cache_syncpoints & 1;

	assert(sl->s[i].pos <= sp.pos && sp.pos < sl->s[i].pos + 16); // code sanity

	assert(!sl->s[i].pts || sl->s[i].pts == sp.pts);
	assert(!sl->s[i].back_ptr || sl->s[i].back_ptr == sp.back_ptr);
	sl->s[i].pos = sp.pos;
	sl->s[i].pts = sp.pts;
	sl->s[i].back_ptr = sp.back_ptr;
	if (pts_cache && sp.pts_valid) {
		for (j = 0; j < nut->stream_count; j++) {
			assert(!sl->s[i].pts_valid || sl->pts[i * nut->stream_count + j] == pts[j]);
			assert(!sl->s[i].pts_valid || sl->eor[i * nut->stream_count + j] == eor[j]);
			sl->pts[i * nut->stream_count + j] = pts[j];
			sl->eor[i * nut->stream_count + j] = eor[j];
		}
		sl->s[i].pts_valid = 1;
	}
	if (sp.pts_valid && i) sl->s[i-1].seen_next = 1;
}

static int add_syncpoint(nut_context_tt * nut, syncpoint_tt sp, uint64_t * pts, uint64_t * eor, int * out) {
	syncpoint_list_tt * sl = &nut->syncpoints;
	int i, j, err = 0;
	int pts_cache = nut->dopts.cache_syncpoints & 1;

	if (pts_cache && sp.pts_valid) assert(pts && eor); // code sanity check

	for (i = sl->len; i--; ) { // more often than not, we're adding at end of list
		if (sl->s[i].pos > sp.pos) continue;
		if (sp.pos < sl->s[i].pos + 16) { // syncpoint already in list
			add_existing_syncpoint(nut, sp, pts, eor, i);
			if (out) *out = i;
			return 0;
		}
		break;
	}
	i++;
	if (sl->len + 1 > sl->alloc_len) {
		sl->alloc_len += PREALLOC_SIZE/4;
		SAFE_REALLOC(nut->alloc, sl->s, sizeof(syncpoint_tt), sl->alloc_len);
		if (pts_cache) {
			SAFE_REALLOC(nut->alloc, sl->pts, nut->stream_count * sizeof(uint64_t), sl->alloc_len);
			SAFE_REALLOC(nut->alloc, sl->eor, nut->stream_count * sizeof(uint64_t), sl->alloc_len);
		}
	}
	memmove(sl->s + i + 1, sl->s + i, (sl->len - i) * sizeof(syncpoint_tt));
	sl->s[i] = sp;
	if (sl->s[i].pts_valid) {
		if (!pts_cache) sl->s[i].pts_valid = 0; // pts_valid is not really true, only used for seen_next
		if (i) sl->s[i-1].seen_next = 1;
	}

	if (pts_cache) {
		memmove(sl->pts + (i + 1) * nut->stream_count, sl->pts + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));
		memmove(sl->eor + (i + 1) * nut->stream_count, sl->eor + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));
		for (j = 0; j < nut->stream_count; j++) {
			sl->pts[i * nut->stream_count + j] = sl->s[i].pts_valid ? pts[j] : 0;
			sl->eor[i * nut->stream_count + j] = sl->s[i].pts_valid ? eor[j] : 0;
		}
	}

	sl->len++;
	if (out) *out = i;
err_out:
	return err;
}

static int queue_add_syncpoint(nut_context_tt * nut, syncpoint_tt sp, uint64_t * pts, uint64_t * eor) {
	syncpoint_list_tt * sl = &nut->syncpoints;
	syncpoint_linked_tt * s;
	size_t malloc_size;
	int pts_cache = nut->dopts.cache_syncpoints & 1;
	int err = 0;
	int i = sl->cached_pos;

	if (i >= sl->len) i = sl->len - 1;

	while (sl->s[i].pos > sp.pos && i) i--;
	while (sl->s[i+1].pos <= sp.pos && i < sl->len-1) i++;
	// Result: sl->s[i].pos <= sp.pos < sl->s[i+1].pos
	sl->cached_pos = i;

	if (sl->s[i].pos <= sp.pos && sp.pos < sl->s[i].pos + 16) { // syncpoint already in list
		add_existing_syncpoint(nut, sp, pts, eor, i);
		return 0;
	}

	malloc_size = sizeof(syncpoint_linked_tt) - sizeof(uint64_t);
	if (pts_cache && sp.pts_valid) {
		assert(pts && eor); // code sanity check
		malloc_size += (nut->stream_count*2) * sizeof(uint64_t);
	}

	SAFE_CALLOC(nut->alloc, s, 1, malloc_size);

	s->s = sp;
	if (pts_cache && sp.pts_valid) {
		for (i = 0; i < nut->stream_count; i++) {
			s->pts_eor[i] = pts[i];
			s->pts_eor[i+nut->stream_count] = eor[i];
		}
	}

	s->prev = sl->linked;
	sl->linked = s;
err_out:
	return err;
}

static int flush_syncpoint_queue(nut_context_tt * nut) {
	syncpoint_list_tt * sl = &nut->syncpoints;
	int err = 0;
	while (sl->linked) {
		syncpoint_linked_tt * s = sl->linked;
		CHECK(add_syncpoint(nut, s->s, s->pts_eor, s->pts_eor + nut->stream_count, NULL));
		sl->linked = s->prev;
		nut->alloc->free(s);
	}
err_out:
	return err;
}

static void set_global_pts(nut_context_tt * nut, uint64_t pts) {
	int i;
	TO_PTS(timestamp, pts)

	for (i = 0; i < nut->stream_count; i++) {
		nut->sc[i].last_pts = convert_ts(timestamp_p, nut->tb[timestamp_tb], TO_TB(i));
	}
}

static int get_syncpoint(nut_context_tt * nut) {
	int err = 0;
	syncpoint_tt s;
	int after_seek = nut->last_syncpoint ? 0 : 1;
	input_buffer_tt itmp, * tmp = new_mem_buffer(&itmp);

	s.pos = bctello(nut->i) - 8;

	if (nut->last_syncpoint == s.pos) after_seek = 1; // don't go through the same syncpoint twice
	nut->last_syncpoint = s.pos;

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, s.pts);
	GET_V(tmp, s.back_ptr);
	s.back_ptr = s.back_ptr * 16 + 15;

	set_global_pts(nut, s.pts);

	s.seen_next = 0;
	s.pts_valid = !after_seek;
	if (nut->dopts.cache_syncpoints) { // either we're using syncpoint cache, or we're seeking and we need the cache
		int i;
		uint64_t pts[nut->stream_count];
		uint64_t eor[nut->stream_count];
		for (i = 0; i < nut->stream_count; i++) {
			pts[i] = nut->sc[i].last_key;
			eor[i] = nut->sc[i].eor;
			nut->sc[i].last_key = 0;
			nut->sc[i].eor = 0;
		}
		if (nut->dopts.cache_syncpoints & 2) // during seeking, syncpoints go into cache immediately
			CHECK(add_syncpoint(nut, s, pts, eor, NULL));
		else // otherwise, queue to a linked list to avoid CPU cache trashing during playback
			CHECK(queue_add_syncpoint(nut, s, pts, eor));
	}
err_out:
	return err;
}

static int get_index(nut_context_tt * nut) {
	input_buffer_tt itmp, * tmp = new_mem_buffer(&itmp);
	int i, err = 0;
	uint64_t max_pts;
	syncpoint_list_tt * sl = &nut->syncpoints;

	CHECK(get_header(nut->i, tmp));

	GET_V(tmp, max_pts);
	for (i = 0; i < nut->stream_count; i++) {
		TO_PTS(max, max_pts)
		nut->sc[i].sh.max_pts = convert_ts(max_p, nut->tb[max_tb], TO_TB(i));
	}

	GET_V(tmp, sl->len);
	sl->alloc_len = sl->len;
	SAFE_REALLOC(nut->alloc, sl->s, sizeof(syncpoint_tt), sl->alloc_len);
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
			uint64_t x;
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

	debug_msg("NUT index read successfully, %d syncpoints\n", sl->len);

err_out:
	return err;
}

static void clear_dts_cache(nut_context_tt * nut) {
	int i;
	for (i = 0; i < nut->stream_count; i++) {
		int j;
		for (j = 0; j < nut->sc[i].sh.decode_delay; j++) nut->sc[i].pts_cache[j] = -1;
		nut->sc[i].last_dts = -1;
	}
}

static int get_packet(nut_context_tt * nut, nut_packet_tt * pd, int * saw_syncpoint) {
	nut_info_packet_tt info = { 0 };
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
				while (tmp != SYNCPOINT_STARTCODE) {
					ERROR(tmp >> 56 != 'N', NUT_ERR_NOT_FRAME_NOT_N);
					CHECK(get_header(nut->i, NULL));
					CHECK(get_bytes(nut->i, 8, &tmp));
				}
				nut->i->buf_ptr -= 8;
				return -1;
			case INFO_STARTCODE: if (nut->dopts.new_info && !nut->seek_status) {
				CHECK(get_info_header(nut, &info));
				nut->dopts.new_info(nut->dopts.info_priv, &info);
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
		ERROR(crc32(get_buf(nut->i, start), bctello(nut->i) - start), NUT_ERR_BAD_CHECKSUM);
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
			debug_msg("%lld %d (%f) %lld %d (%f) \n",
				pd->pts, pd->stream, TO_DOUBLE(pd->stream, pd->pts),
				nut->sc[i].last_dts, i, TO_DOUBLE(i, nut->sc[i].last_dts));
		ERROR(compare_ts(pd->pts, TO_TB(pd->stream), nut->sc[i].last_dts, TO_TB(i)) < 0, NUT_ERR_OUT_OF_ORDER);
	}

	if (saw_syncpoint) *saw_syncpoint = !!after_sync;
err_out:
	free_info_packet(nut, &info);
	return err;
}

static void push_frame(nut_context_tt * nut, nut_packet_tt * pd) {
	stream_context_tt * sc = &nut->sc[pd->stream];
	sc->last_pts = pd->pts;
	sc->last_dts = get_dts(sc->sh.decode_delay, sc->pts_cache, pd->pts);
	if (pd->flags & NUT_FLAG_KEY && !sc->last_key) sc->last_key = pd->pts + 1;
	if (pd->flags & NUT_FLAG_EOR) sc->eor = pd->pts + 1;
	else sc->eor = 0;
}

static int find_main_headers(nut_context_tt * nut) {
	int err = 0;
	uint64_t tmp;
	int len = PREALLOC_SIZE;
	int read_data = ready_read_buf(nut->i, len);

	// don't waste cpu by running this check every damn time for EAGAIN
	if (read_data < len && buf_eof(nut->i) != NUT_ERR_EOF) return buf_eof(nut->i);

	CHECK(get_bytes(nut->i, 7, &tmp)); // true EOF will fail here
	len = (read_data -= 7);
	while (len--) {
		tmp = (tmp << 8) | *(nut->i->buf_ptr++);
		if (tmp == MAIN_STARTCODE) break;
		// give up if we reach a syncpoint, unless we're searching the file end
		if (tmp == SYNCPOINT_STARTCODE && nut->seek_status != 18 && !nut->last_syncpoint) break;
	}
	if (tmp == MAIN_STARTCODE) {
		off_t pos = bctello(nut->i) - 8;
		// load all headers into memory so they can be cleanly decoded without EAGAIN issues
		// also check validity of the headers we just found
		do {
			if ((err = get_header(nut->i, NULL)) == NUT_ERR_EAGAIN) goto err_out;
			if (err) { tmp = err = 0; break; } // bad

			// EOF is a legal error here - when reading the last headers in the file
			if ((err = get_bytes(nut->i, 8, &tmp)) == NUT_ERR_EOF) { err = 0; tmp = SYNCPOINT_STARTCODE; }
			CHECK(err); // if get_bytes returns EAGAIN or a memory error, check for that
		} while (tmp != SYNCPOINT_STARTCODE);
		if (tmp == SYNCPOINT_STARTCODE) { // success!
			nut->last_syncpoint = nut->before_seek = nut->seek_status = 0;
			nut->last_headers = pos;
			nut->i->buf_ptr = get_buf(nut->i, nut->last_headers);
			flush_buf(nut->i);
			return 0;
		}
	}

	// failure
	if (len == -1 && (nut->before_seek += read_data) < 512*1024 && read_data > 0) {
		nut->i->buf_ptr -= 7; // rewind 7 bytes, try again
		flush_buf(nut->i);
		return find_main_headers(nut);
	} else nut->before_seek = 0;
	if (!nut->i->isc.seek) return NUT_ERR_NO_HEADERS;
	if (!nut->seek_status) {
		nut->seek_status = 18; // start search at 512kb
		// but first, let's check EOF
		if (!nut->last_syncpoint) { // unless we've checked it already
			seek_buf(nut->i, -512*1024, SEEK_END);
			return find_main_headers(nut);
		}
	}
	seek_buf(nut->i, 1 << ++nut->seek_status, SEEK_SET);
	// eventually we'll hit EOF and give up
	return find_main_headers(nut);
err_out:
	if (err == NUT_ERR_EOF && !nut->last_syncpoint && nut->seek_status) {
		// last resort: after checking whole file, try again, this time don't stop at syncpoints.
		nut->last_syncpoint = 1;
		nut->before_seek = nut->seek_status = 0;
		seek_buf(nut->i, 0, SEEK_SET);
		return find_main_headers(nut);
	}
	return err;
}

static int find_syncpoint(nut_context_tt * nut, syncpoint_tt * res, int backwards, off_t stop) {
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
			input_buffer_tt itmp, * tmp = new_mem_buffer(&itmp);
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

	if (read < nut->max_distance) return buf_eof(nut->i); // too little was read

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

static int smart_find_syncpoint(nut_context_tt * nut, syncpoint_tt * sp, int backwards, off_t stop) {
	struct find_syncpoint_state_s * fss = &nut->find_syncpoint_state;
	syncpoint_list_tt * sl = &nut->syncpoints;
	int i = fss->i, err = 0;
	off_t pos = fss->i ? fss->pos : bctello(nut->i);

	ERROR(!(nut->dopts.cache_syncpoints & 1) || !sl->len, -1);

	CHECK(flush_syncpoint_queue(nut));

	if (i) i--;
	else {
		for (i = 0; i < sl->len; i++) if (sl->s[i].pos+15 > pos) break;
		ERROR(i == sl->len || (i && !sl->s[i-1].seen_next), -1);

		// trust the caller if it gave more precise syncpoint location
		if (ABS(pos - sl->s[i].pos) > 15) seek_buf(nut->i, sl->s[i].pos, SEEK_SET);
	}
	fss->i = i + 1;
	fss->pos = pos;

	if (!fss->begin) CHECK(find_syncpoint(nut, sp, 0, sl->s[i].pos + 15 + 8));
	else sp->seen_next = 1;

	if (sp->seen_next) { // failure
		int begin = fss->begin ? fss->begin - 1 : i;
		int o = backwards ? -1 : 1;
		fss->begin = begin + 1;
		while (sp->seen_next) {
			if ((unsigned)(i+o) >= sl->len) break; // bounds check
			if (i > o && !sl->s[i+o-1].seen_next) break; // no seen_next, nothing to check
			if (stop && sl->s[i+o].pos > stop) break; // passed stop condition
			if (!fss->seeked) seek_buf(nut->i, sl->s[i+o].pos, SEEK_SET);
			fss->seeked = 1;
			CHECK(find_syncpoint(nut, sp, 0, sl->s[i+o].pos + 15 + 8));
			fss->seeked = 0;
			fss->i = (i+=o) + 1;
		}
		if (sp->seen_next) { // still nothing! let's linear search the whole area
			if (!fss->seeked) {
				if (backwards) seek_buf(nut->i, sl->s[begin].pos - nut->max_distance, SEEK_SET);
				else seek_buf(nut->i, begin > 0 ? sl->s[begin-1].pos+15 : 0, SEEK_SET);
			}
			fss->seeked = 1;
			CHECK(find_syncpoint(nut, sp, backwards, stop));
			fss->seeked = 0;
		}
		CHECK(add_syncpoint(nut, *sp, NULL, NULL, &i));
		assert((i >= begin && !backwards) || (i <= begin && backwards));

		if (backwards) { // swap vars
			int tmp = begin;
			begin = i + 1;
			i = tmp + 1;
		}

		memmove(sl->s + begin, sl->s + i, (sl->len - i) * sizeof(syncpoint_tt));
		memmove(sl->pts + begin * nut->stream_count, sl->pts + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));
		memmove(sl->eor + begin * nut->stream_count, sl->eor + i * nut->stream_count, (sl->len - i) * nut->stream_count * sizeof(uint64_t));

		sl->len -= i-begin;

		if (sp->pos < pos && !backwards) { // wow, how silly!
			fss->pos = fss->i = fss->begin = fss->seeked = 0;
			seek_buf(nut->i, pos, SEEK_SET);
			return smart_find_syncpoint(nut, sp, backwards, stop);
		}
	}
	fss->pos = fss->i = fss->begin = fss->seeked = 0;

err_out:
	if (err == -1) {
		if (backwards && !fss->seeked) {
			CHECK(find_syncpoint(nut, sp, 0, pos + 15 + 8));
			if (!sp->seen_next) return 0;
			seek_buf(nut->i, -nut->max_distance, SEEK_CUR);
		}
		fss->seeked = 1;
		CHECK(find_syncpoint(nut, sp, backwards, stop));
		fss->seeked = 0;
		err = 0;
	}
	return err;
}

int nut_read_next_packet(nut_context_tt * nut, nut_packet_tt * pd) {
	int err = 0;
	if (nut->seek_status) { // in error mode!
		syncpoint_tt s;
		CHECK(smart_find_syncpoint(nut, &s, 0, 0));
		nut->i->buf_ptr = get_buf(nut->i, s.pos); // go back to beginning of syncpoint
		flush_buf(nut->i);
		clear_dts_cache(nut);
		nut->last_syncpoint = 0;
		nut->seek_status = 0;
	}

	while ((err = get_packet(nut, pd, NULL)) == -1) flush_buf(nut->i);
	if (err > NUT_ERR_OUT_OF_MEM) { // some error occured!
		debug_msg("NUT: %s\n", nut_error(err));
		// rewind as much as possible
		if (nut->i->isc.seek) seek_buf(nut->i, nut->last_syncpoint + 16, SEEK_SET);
		else nut->i->buf_ptr = nut->i->buf + MIN(16, nut->i->read_len);

		nut->seek_status = 1; // enter error mode
		return nut_read_next_packet(nut, pd);
	}

	if (!err) push_frame(nut, pd);
err_out:
	if (err != NUT_ERR_EAGAIN) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

static int get_headers(nut_context_tt * nut, int read_info) {
	int i, err = 0;
	uint64_t tmp;

	CHECK(get_bytes(nut->i, 8, &tmp));
	assert(tmp == MAIN_STARTCODE); // sanity, get_headers should only be called in this situation
	CHECK(get_main_header(nut));

	SAFE_CALLOC(nut->alloc, nut->sc, sizeof(stream_context_tt), nut->stream_count);
	for (i = 0; i < nut->stream_count; i++) nut->sc[i].sh.type = -1;

	CHECK(get_bytes(nut->i, 8, &tmp));

	while (tmp != SYNCPOINT_STARTCODE) {
		ERROR(tmp >> 56 != 'N', NUT_ERR_NOT_FRAME_NOT_N);
		if (tmp == STREAM_STARTCODE) {
			CHECK(get_stream_header(nut));
		} else if (tmp == INFO_STARTCODE && read_info) {
			SAFE_REALLOC(nut->alloc, nut->info, sizeof(nut_info_packet_tt), ++nut->info_count + 1);
			memset(&nut->info[nut->info_count - 1], 0, sizeof(nut_info_packet_tt));
			CHECK(get_info_header(nut, &nut->info[nut->info_count - 1]));
			nut->info[nut->info_count].count = -1;
		} else if (tmp == INDEX_STARTCODE && nut->dopts.read_index&1) {
			CHECK(get_index(nut)); // usually you don't care about get_index() errors, but nothing except a memory error can happen here
			nut->dopts.read_index = 2;
		} else {
			CHECK(get_header(nut->i, NULL));
		}
		// EOF is a legal error here - when reading the last headers in the file
		if ((err = get_bytes(nut->i, 8, &tmp)) == NUT_ERR_EOF) { tmp = err = 0; break; }
		CHECK(err); // it's just barely possible for get_bytes to return a memory error, check for that
	}
	if (tmp == SYNCPOINT_STARTCODE) nut->i->buf_ptr -= 8;

	for (i = 0; i < nut->stream_count; i++) ERROR(nut->sc[i].sh.type == -1, NUT_ERR_NOSTREAM_STARTCODE);

err_out:
	assert(err != NUT_ERR_EAGAIN); // EAGAIN is illegal here!!
	return err;
}

int nut_read_headers(nut_context_tt * nut, nut_stream_header_tt * s [], nut_info_packet_tt * info []) {
	int i, err = 0;
	syncpoint_tt sp;

	// step 1 - find headers and load to memory
	if (!nut->last_headers) CHECK(find_main_headers(nut));

	// step 2 - parse headers
	if (!nut->sc) CHECK(get_headers(nut, !!info));

	// step 3 - search for index if necessary
	if (nut->dopts.read_index & 1) {
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
			CHECK(get_bytes(nut->i, 8, &idx_ptr));
			if (idx_ptr != INDEX_STARTCODE) err = 1;
			else {
				// only EAGAIN from get_index is interesting
				if ((err = get_index(nut)) == NUT_ERR_EAGAIN) goto err_out;
			}
			if (err) nut->dopts.read_index = 0;
			else nut->dopts.read_index = 2;
			err = 0;
		}
		if (nut->before_seek && nut->last_headers <= 1024) seek_buf(nut->i, nut->before_seek, SEEK_SET);
		nut->before_seek = 0;
		nut->seek_status = 0;
	}

	// step 4 - find the first syncpoint in file
	if (nut->last_headers > 1024 && !nut->seek_status && nut->i->isc.seek) {
		// the headers weren't found in the beginning of the file
		assert(nut->i->isc.seek);
		seek_buf(nut->i, 0, SEEK_SET);
		nut->seek_status = 1;
	}
	CHECK(smart_find_syncpoint(nut, &sp, 0, 0));
	CHECK(add_syncpoint(nut, sp, NULL, NULL, NULL));
	nut->i->buf_ptr = get_buf(nut->i, sp.pos); // rewind to the syncpoint, this is where playback starts...
	nut->seek_status = 0;

	SAFE_CALLOC(nut->alloc, *s, sizeof(nut_stream_header_tt), nut->stream_count + 1);
	for (i = 0; i < nut->stream_count; i++) (*s)[i] = nut->sc[i].sh;
	(*s)[i].type = -1;
	nut->tmp_buffer = (void*)*s;
	if (info) *info = nut->info;
err_out:
	if (err != NUT_ERR_EAGAIN) flush_buf(nut->i); // unless EAGAIN
	else nut->i->buf_ptr = nut->i->buf; // rewind
	return err;
}

int nut_read_frame(nut_context_tt * nut, int * len, uint8_t * buf) {
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

static off_t seek_interpolate(int max_distance, double time_pos, off_t lo, off_t hi, double lo_pd, double hi_pd, off_t fake_hi) {
	double weight = 19./20.;
	off_t guess;
	if (fake_hi - lo < max_distance) guess = lo + 16;
	else { // linear interpolation
		guess = (lo + (double)(hi-lo)/(hi_pd-lo_pd) * (time_pos-lo_pd))*weight + (lo+hi)/2.*(1.-weight);
		if (fake_hi - guess < max_distance) guess = fake_hi - max_distance; //(lo + hi)/2;
	}
	if (guess < lo + 16) guess = lo + 16;
	return guess;
}

static int binary_search_syncpoint(nut_context_tt * nut, double time_pos, off_t * start, off_t * end, syncpoint_tt * stopper) {
	int i, err = 0;
	syncpoint_tt s;
	off_t fake_hi, * guess = &nut->binary_guess;
	uint64_t timebases[nut->timebase_count];
	syncpoint_list_tt * sl = &nut->syncpoints;
	int a = 0;
	for (i = 0; i < nut->timebase_count; i++) timebases[i] = (uint64_t)(time_pos / nut->tb[i].num * nut->tb[i].den);
	assert(sl->len); // it is impossible for the first syncpoint to not have been read

	// find last syncpoint if it's not already found
	if (!sl->s[sl->len-1].seen_next) {
		// searching backwards from EOF
		if (!nut->seek_status) seek_buf(nut->i, -nut->max_distance, SEEK_END);
		nut->seek_status = 1;
		CHECK(find_syncpoint(nut, &s, 1, 0));
		CHECK(add_syncpoint(nut, s, NULL, NULL, &i));
		assert(i == sl->len-1);
		sl->s[i].seen_next = 1;
		nut->seek_status = 0;
	}
	// sl->len MUST be >=2, which is the first and last syncpoints in the file
	ERROR(sl->len < 2, NUT_ERR_NOT_SEEKABLE);

	for (i = 0; i < sl->len; i++) {
		TO_PTS(tmp, sl->s[i].pts)
		if (timebases[tmp_tb] <= tmp_p) break;
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

#define LO (sl->s[i])
#define HI (sl->s[i+1])
	fake_hi = nut->seek_status ? (nut->seek_status >> 1) : HI.pos;

	while (!LO.seen_next) {
		// start binary search between LO (sl->s[i].pos) to HI (sl->s[i+1].pos) ...
		if (!*guess) *guess = seek_interpolate(nut->max_distance*2, time_pos, LO.pos, HI.pos, TO_DOUBLE_PTS(LO.pts), TO_DOUBLE_PTS(HI.pts), fake_hi);

		debug_msg("\n%d [ (%d,%.3f) .. (%d,%.3f) .. (%d(%d),%.3f) ] ", i, (int)LO.pos, TO_DOUBLE_PTS(LO.pts), (int)*guess, time_pos,
		                                                                   (int)HI.pos, (int)fake_hi, TO_DOUBLE_PTS(HI.pts));
		a++;

		if (!(nut->seek_status & 1)) {
			if (!nut->seek_status) seek_buf(nut->i, *guess, SEEK_SET);
			nut->seek_status = fake_hi << 1;
			CHECK(find_syncpoint(nut, &s, 0, fake_hi));
			nut->seek_status = 0;
		}

scan_backwards:
		if (nut->seek_status & 1 || s.seen_next == 1 || s.pos >= fake_hi) { // we got back to 'HI'
			if (*guess == LO.pos + 16) { // we are done!
				LO.seen_next = 1;
				break;
			}
			// Now let's scan backwards from 'guess' to 'LO' - after this, we can set s.seen_next
			if (!nut->seek_status) seek_buf(nut->i, *guess - nut->max_distance + 7, SEEK_SET);
			nut->seek_status = (fake_hi << 1) + 1;
			CHECK(find_syncpoint(nut, &s, 1, 0));
			nut->seek_status = 0;
 			s.seen_next = 1; // now this can cause LO.seen_next to be set after add_syncpoint(), it's magic
		}

		CHECK(add_syncpoint(nut, s, NULL, NULL, timebases[s.pts%nut->timebase_count] < s.pts/nut->timebase_count ? NULL : &i));

		if (s.pos - *guess > nut->max_distance * 100) {
			// we just scanned a very large area of (probably) damaged data
			// let's avoid scanning it again by caching the syncpoint right before it with seen_next
			s.seen_next = 1;
			goto scan_backwards;
		}

		if (HI.pos < fake_hi) fake_hi = MIN(HI.pos, *guess);
		*guess = 0;
	}
	*guess = 0;

	debug_msg("\n[ (%d,%d) .. %d .. (%d,%d) ] => %d (%d seeks) %d\n",
	        (int)LO.pos, (int)LO.pts, (int)timebases[0], (int)HI.pos, (int)HI.pts, (int)(LO.pos - LO.back_ptr), a, LO.back_ptr);
	// at this point, s[i].pts < P < s[i+1].pts, and s[i].seen_next is set
	*start = LO.pos - LO.back_ptr;
	*end = HI.pos;
	*stopper = HI;
err_out:
	if (err == NUT_ERR_EAGAIN) nut->i->buf_ptr = nut->i->buf;
	return err;
}

static int linear_search_seek(nut_context_tt * nut, int backwards, off_t start, off_t end, syncpoint_tt * stopper) {
	syncpoint_list_tt * sl = &nut->syncpoints;
	int i, err = 0;
	off_t min_pos = 0;
	off_t buf_before = 0;
	off_t stopper_syncpoint = 0;

	if (nut->seek_status <= 1) {
		syncpoint_tt s;
		if (!nut->seek_status) seek_buf(nut->i, start, SEEK_SET);
		nut->seek_status = 1;
		// find closest syncpoint by linear search, SHOULD be one pointed to by back_ptr...
		CHECK(smart_find_syncpoint(nut, &s, !!end, 0));
		clear_dts_cache(nut);
		nut->last_syncpoint = 0; // last_key is invalid
		seek_buf(nut->i, s.pos, SEEK_SET); // go back to syncpoint. This will not need a seek.
		nut->seek_status = s.pos << 1;
		ERROR(s.pos < start || s.pos > start + 15, 0); // error condition, we didn't get the syncpoint we wanted
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
		if (stopper_syncpoint > bctello(nut->i)) stopper_syncpoint = 0; // do not load stopper_syncpoint position before it is actually reached
	}

#define CHECK_break(expr) { if ((err = (expr))) { if (end && err != NUT_ERR_EAGAIN) break; else goto err_out; } }
	if (!(nut->seek_status & 1)) while (bctello(nut->i) < end || !end) {
		int saw_syncpoint;
		nut_packet_tt pd;

		buf_before = bctello(nut->i);
		err = get_packet(nut, &pd, &saw_syncpoint); // we're counting on syncpoint cache!! for the good_key later, and stopper_syncpoint
		if (err == -1) continue;
		CHECK_break(err);

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
					nut->i->buf_ptr = get_buf(nut->i, buf_before);
					break;
				}
			}
		} else if (stopper && pd.flags&NUT_FLAG_KEY) {
			off_t back_ptr = stopper->pos - stopper->back_ptr;
			TO_PTS(stopper, stopper->pts)
			// only relevant if pts is smaller than stopper, and we passed stopper's back_ptr
			if (compare_ts(pd.pts, TO_TB(pd.stream), stopper_p, nut->tb[stopper_tb]) < 0 && buf_before >= back_ptr) {
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

		CHECK_break(skip_buffer(nut->i, pd.len));
		push_frame(nut, &pd);
	}
	if (!end) goto err_out; // forward seek

	for (i = 0; i < nut->stream_count; i++) {
		if (!nut->sc[i].state.active) continue;
		if (nut->sc[i].state.good_key && (!min_pos || nut->sc[i].state.good_key < min_pos)) min_pos = nut->sc[i].state.good_key;
	}
	if (!min_pos) {
		debug_msg("BIG FAT WARNING (Possibly caused by `%s')", nut_error(err));
		for (i = 0; i < nut->stream_count; i++) debug_msg("%d: %d\n", i, (int)nut->sc[i].state.good_key);
		min_pos = nut->seek_status >> 1;
	}

	// after ALL this, we ended up in a worse position than where we were...
	ERROR(!backwards && min_pos < nut->before_seek, NUT_ERR_NOT_SEEKABLE);

	for (i = 1; i < sl->len; i++) if (sl->s[i].pos > min_pos) break;
	i--;
	if (!(nut->seek_status & 1)) seek_buf(nut->i, sl->s[i].pos, SEEK_SET);

	nut->seek_status |= 1;
	nut->last_syncpoint = 0; // last_key is invalid
	clear_dts_cache(nut);
	buf_before = bctello(nut->i);

	while (bctello(nut->i) < min_pos) {
		nut_packet_tt pd;
		while ((err = get_packet(nut, &pd, NULL)) == -1);
		CHECK(err);
		push_frame(nut, &pd);
		CHECK(skip_buffer(nut->i, pd.len));
	}
	err = 0;

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
	} else if (buf_before) nut->i->buf_ptr = get_buf(nut->i, buf_before); // rewind smart
	else nut->i->buf_ptr = nut->i->buf; // just rewind
	return err;
}

int nut_seek(nut_context_tt * nut, double time_pos, int flags, const int * active_streams) {
	int err = 0;
	off_t start = 0, end = 0;
	int backwards = flags & 1 ? time_pos < 0 : 1;
	syncpoint_tt stopper = { 0, 0, 0, 0, 0 };

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

		for (i = 0; i < nut->stream_count; i++) nut->sc[i].state.pts = (uint64_t)(time_pos / TO_TB(i).num * TO_TB(i).den);
		nut->seek_time_pos = time_pos;
		nut->dopts.cache_syncpoints |= 2;
		CHECK(flush_syncpoint_queue(nut));
	} else {
		time_pos = nut->seek_time_pos;
	}

	if (nut->syncpoints.s[nut->syncpoints.len-1].seen_next) {
		syncpoint_list_tt * sl = &nut->syncpoints;
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
				if (tmp--) { // -- because all pts array is off-by-one. Zero indicates no keyframe.
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

		if (last_sync && good_sync >= 0) {
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
	else debug_msg("============= NO BINARY SEARCH   \n");

	if (start) { // "unsuccessful" seek needs no linear search
		if (!(flags & 2)) { // regular seek
			CHECK(linear_search_seek(nut, backwards, start, end, stopper.pos ? &stopper : NULL));
		} else { // forward seek, find keyframe
			CHECK(linear_search_seek(nut, backwards, end, 0, NULL));
		}
	}
	debug_msg("DONE SEEK\n");
err_out:
	if (err != NUT_ERR_EAGAIN) { // unless EAGAIN
		syncpoint_list_tt * sl = &nut->syncpoints;
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

nut_context_tt * nut_demuxer_init(nut_demuxer_opts_tt * dopts) {
	nut_context_tt * nut;

	if (dopts->alloc.malloc) nut = dopts->alloc.malloc(sizeof(nut_context_tt));
	else nut = malloc(sizeof(nut_context_tt));

	if (!nut) return NULL;

	nut->syncpoints.len = 0;
	nut->syncpoints.alloc_len = 0;
	nut->syncpoints.s = NULL;
	nut->syncpoints.pts = NULL;
	nut->syncpoints.eor = NULL;
	nut->syncpoints.cached_pos = 0;
	nut->syncpoints.linked = NULL;

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
	nut->binary_guess = 0;
	nut->last_syncpoint = 0;
	nut->find_syncpoint_state = (struct find_syncpoint_state_s){0,0,0,0};

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

void nut_demuxer_uninit(nut_context_tt * nut) {
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
	while (nut->syncpoints.linked) {
		syncpoint_linked_tt * s = nut->syncpoints.linked;
		nut->syncpoints.linked = s->prev;
		nut->alloc->free(s);
	}
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
		case NUT_ERR_MAX_PTS_DISTANCE: return "pts difference higher than max_pts_distance.";
		case NUT_ERR_BAD_STREAM_ORDER: return "Stream headers are stored in wrong order.";
		case NUT_ERR_NOSTREAM_STARTCODE: return "Expected stream startcode not found.";
		case NUT_ERR_BAD_EOF: return "Invalid forward_ptr!";
		case NUT_ERR_VLC_TOO_LONG: return "VLC too long";
		case NUT_ERR_OUT_OF_MEM: return "Out of memory";
	}
	return NULL;
}
