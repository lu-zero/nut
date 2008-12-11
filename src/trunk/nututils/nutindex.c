// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>

#define ID_STRING "nut/multimedia container"

#define      MAIN_STARTCODE (0x7A561F5F04ADULL + (((uint64_t)('N'<<8) + 'M')<<48))
#define    STREAM_STARTCODE (0x11405BF2F9DBULL + (((uint64_t)('N'<<8) + 'S')<<48))
#define SYNCPOINT_STARTCODE (0xE4ADEECA4569ULL + (((uint64_t)('N'<<8) + 'K')<<48))
#define     INDEX_STARTCODE (0xDD672F23E64EULL + (((uint64_t)('N'<<8) + 'X')<<48))
#define      INFO_STARTCODE (0xAB68B596BA78ULL + (((uint64_t)('N'<<8) + 'I')<<48))

#define PREALLOC_SIZE 4096

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) > 0 ? (a) : -(a))

#define bctello(bc) ((bc)->file_pos + ((bc)->buf_ptr - (bc)->buf))

static inline uint32_t crc32(uint8_t * buf, int len){
	int32_t crc = 0;
	int i;
	while (len--) {
		crc ^= *buf++ << 24;
		for(i = 0; i < 8; i++) crc = (crc<<1) ^ (0x04C11DB7 & (crc>>31));
	}
	return crc;
}

// ============ OUTPUT BUFFER ==============

typedef struct {
        int is_mem;
	uint8_t * buf;
	uint8_t * buf_ptr;
	int write_len; // allocated memory
	off_t file_pos;
	FILE * out;
} output_buffer_tt;

static void flush_out_buf(output_buffer_tt * bc) {
	assert(!bc->is_mem);
	bc->file_pos += fwrite(bc->buf, 1, bc->buf_ptr - bc->buf, bc->out);
	bc->buf_ptr = bc->buf;
}

static void ready_write_buf(output_buffer_tt * bc, int amount) {
	if (bc->write_len - (bc->buf_ptr - bc->buf) > amount) return;

        if (bc->is_mem) {
		int tmp = bc->buf_ptr - bc->buf;
		bc->write_len = tmp + amount + PREALLOC_SIZE;
		bc->buf = realloc(bc->buf, bc->write_len);
		bc->buf_ptr = bc->buf + tmp;
	} else {
		flush_out_buf(bc);
		if (bc->write_len < amount) {
			free(bc->buf);
			bc->write_len = amount + PREALLOC_SIZE;
			bc->buf_ptr = bc->buf = malloc(bc->write_len);
		}
	}
}

static void put_bytes(output_buffer_tt * bc, int count, uint64_t val) {
	ready_write_buf(bc, count);
	for(count--; count >= 0; count--){
		*(bc->buf_ptr++) = val >> (8 * count);
	}
}

static output_buffer_tt * new_mem_buffer(output_buffer_tt * bc) {
	bc->write_len = 0;
	bc->is_mem = 1;
	bc->file_pos = 0;
	bc->buf_ptr = bc->buf = NULL;
	return bc;
}

static output_buffer_tt * new_output_buffer(output_buffer_tt * bc, FILE * out) {
	new_mem_buffer(bc);
	bc->is_mem = 0;
	bc->out = out;
	return bc;
}

static int v_len(uint64_t val) {
	int i;
	val &= 0x7FFFFFFFFFFFFFFFULL;
	for(i=1; val>>(i*7); i++);
	return i;
}

static void put_v(output_buffer_tt * bc, uint64_t val) {
	int i = v_len(val);
	ready_write_buf(bc, i);
	for(i = (i-1)*7; i; i-=7) {
		*(bc->buf_ptr++) = 0x80 | (val>>i);
	}
	*(bc->buf_ptr++)= val & 0x7F;
}

static void put_data(output_buffer_tt * bc, int len, const uint8_t * data) {
	if (len + (bc->buf_ptr - bc->buf) < bc->write_len || bc->is_mem) {
		ready_write_buf(bc, len);
		memcpy(bc->buf_ptr, data, len);
		bc->buf_ptr += len;
	} else {
		flush_out_buf(bc);
		bc->file_pos += fwrite(data, 1, len, bc->out);
	}
}

static void free_out_buffer(output_buffer_tt * bc) {
	if (!bc) return;
	if (!bc->is_mem) flush_out_buf(bc);
	free(bc->buf);
}

// ============ INPUT BUFFER ==============

typedef struct {
	FILE * in;
	uint8_t * buf;
	uint8_t * buf_ptr;
	int write_len; // allocated memory
	int read_len;  // data in memory
	off_t file_pos;
	off_t filesize;
} input_buffer_tt;

static void flush_buf(input_buffer_tt *bc) {
	bc->file_pos += bc->buf_ptr - bc->buf;
	bc->read_len -= bc->buf_ptr - bc->buf;
	memmove(bc->buf, bc->buf_ptr, bc->read_len);
	bc->buf_ptr = bc->buf;
}

static int ready_read_buf(input_buffer_tt * bc, int amount) {
	int pos = (bc->buf_ptr - bc->buf);
	if (bc->read_len - pos < amount) {
		amount += 10;
		if (bc->write_len - pos < amount) {
			bc->write_len = amount + pos + PREALLOC_SIZE;
			bc->buf = realloc(bc->buf, bc->write_len);
			bc->buf_ptr = bc->buf + pos;
		}
		bc->read_len += fread(bc->buf + bc->read_len, 1, amount - (bc->read_len - pos), bc->in);
	}
	return bc->read_len - (bc->buf_ptr - bc->buf);
}

static void seek_buf(input_buffer_tt * bc, long long pos, int whence) {
	if (whence == SEEK_CUR) pos -= bc->read_len - (bc->buf_ptr - bc->buf);
	fseek(bc->in, pos, whence);
	bc->file_pos = ftell(bc->in);
	bc->buf_ptr = bc->buf;
	bc->read_len = 0;
	if (whence == SEEK_END) bc->filesize = bc->file_pos - pos;
}

static input_buffer_tt * new_input_buffer(input_buffer_tt * bc, FILE * in) {
	bc->read_len = 0;
	bc->write_len = 0;
	bc->file_pos = 0;
	bc->filesize = 0;
	bc->buf_ptr = bc->buf = NULL;
	bc->in = in;
	return bc;
}

static int skip_buffer(input_buffer_tt * bc, int len) {
	if (ready_read_buf(bc, len) < len) return 1;
	bc->buf_ptr += len;
	return 0;
}

static void free_buffer(input_buffer_tt * bc) {
	if (!bc) return;
	free(bc->buf);
}

static int get_bytes(input_buffer_tt * bc, int count, uint64_t * val) {
	int i;
	if (ready_read_buf(bc, count) < count) return 1;
	*val = 0;
	for(i = 0; i < count; i++){
		*val = (*val << 8) | *(bc->buf_ptr++);
	}
	return 0;
}

static int get_v(input_buffer_tt * bc, uint64_t * val) {
	int i, len;
	*val = 0;
	do {
		len = ready_read_buf(bc, 16);
		for(i = 0; i < len; i++){
			uint8_t tmp= *(bc->buf_ptr++);
			*val = (*val << 7) | (tmp & 0x7F);
			if (!(tmp & 0x80)) return 0;
		}
	} while (len >= 16);
	return 1;
}

static int get_data(input_buffer_tt * bc, int len, uint8_t * buf) {
	int tmp = MIN(len, bc->read_len - (bc->buf_ptr - bc->buf));
	if (tmp) {
		memcpy(buf, bc->buf_ptr, tmp);
		bc->buf_ptr += tmp;
		len -= tmp;
	}
	if (len) {
		int read = fread(buf + tmp, 1, len, bc->in);
		bc->file_pos += read;
		len -= read;
	}
	flush_buf(bc);
	return len;
}

#define CHECK(expr) do { int _a; if ((_a = (expr))) return _a; } while(0)
#define GET_V(bc, v) do { uint64_t _tmp; CHECK(get_v((bc), &_tmp)); (v) = _tmp; } while(0)

static int get_header(input_buffer_tt * in) {
	off_t start = bctello(in) - 8; // startcode
	int forward_ptr;

	GET_V(in, forward_ptr);
	if (forward_ptr > 4096) {
		CHECK(skip_buffer(in, 4)); // header_checksum
		if (crc32(in->buf_ptr - (bctello(in) - start), bctello(in) - start)) return 3;
	}

	CHECK(skip_buffer(in, forward_ptr));
	if (crc32(in->buf_ptr - forward_ptr, forward_ptr)) return 3;
	return 0;
}

static int read_headers(input_buffer_tt * in) {
	int len = strlen(ID_STRING) + 1;
	uint64_t tmp;
	assert(in->buf == in->buf_ptr);
	if (ready_read_buf(in, len) < len) return 1;
	if (memcmp(in->buf_ptr, ID_STRING, len)) return 1;
	in->buf_ptr += len;
	CHECK(get_bytes(in, 8, &tmp));
	while (tmp != INDEX_STARTCODE && tmp != SYNCPOINT_STARTCODE) {
		CHECK(get_header(in));
		CHECK(get_bytes(in, 8, &tmp));
	}
	if (tmp == INDEX_STARTCODE) return 2;
	in->buf_ptr -= 8;
	return 0;
}

static int find_copy_index(input_buffer_tt * in, output_buffer_tt * out, off_t * end) {
	uint64_t tmp;
	uint64_t idx_len;
	uint64_t max_pts, syncpoints;
	int new_idx_len, forward_ptr;
	int i, padding;
	assert(out->is_mem);

	seek_buf(in, -12, SEEK_END);
	CHECK(get_bytes(in, 8, &idx_len));
	*end = in->filesize - idx_len;
	if (!*end || *end >= in->filesize) return 2;
	seek_buf(in, *end, SEEK_SET);
	CHECK(get_bytes(in, 8, &tmp));
	if (tmp != INDEX_STARTCODE) return 2;

	GET_V(in, forward_ptr);
	if (forward_ptr > 4096) { // checksum
		skip_buffer(in, 4);
		if (crc32(in->buf, in->buf_ptr - in->buf)) return 4;
	}

	GET_V(in, max_pts);
	GET_V(in, syncpoints);

	GET_V(in, tmp); // first syncpoint position

	new_idx_len = ((idx_len + 15)/16)*16;

	// paranoia, this should either iterate once or not at all
	while ( (v_len(tmp         + new_idx_len/16     ) - v_len(tmp        )) +
		(v_len(forward_ptr + new_idx_len-idx_len) - v_len(forward_ptr))
		> new_idx_len - idx_len) new_idx_len += 16;

	ready_write_buf(out, new_idx_len); // prealloc

	put_bytes(out, 8, INDEX_STARTCODE);
	put_v(out, forward_ptr + new_idx_len-idx_len);
	if (forward_ptr + new_idx_len-idx_len > 4096) put_bytes(out, 4, crc32(out->buf, bctello(out)));
	put_v(out, max_pts);
	put_v(out, syncpoints);

	padding = new_idx_len - idx_len;
	padding -= (v_len(tmp + new_idx_len/16) - v_len(tmp));
	padding -= (v_len(forward_ptr + new_idx_len-idx_len) - v_len(forward_ptr)); // extreme paranoia, this will be zero
	for (i = 0; i < padding; i++) put_bytes(out, 1, 0x80);
	put_v(out, tmp + new_idx_len/16); // the new first syncpoint position
	printf("%d => %d (%d)\n", (int)tmp, (int)(tmp + new_idx_len/16), new_idx_len);

	forward_ptr += new_idx_len-idx_len;

	idx_len = (in->filesize - 12) - bctello(in); // copy everything from where we are until the index_ptr and checksum
	if (get_data(in, idx_len, out->buf_ptr)) return 1;
	out->buf_ptr += idx_len;

	put_bytes(out, 8, new_idx_len); // index_ptr
	put_bytes(out, 4, crc32(out->buf_ptr - (forward_ptr - 4), forward_ptr - 4)); // checksum

	return 0;
}

int main(int argc, char * argv[]) {
	FILE * fin = argc>2 ? fopen(argv[1], "rb") : NULL;
	FILE * fout = argc>2 ? fopen(argv[2], "wb") : NULL;
	input_buffer_tt iin, * in;
	output_buffer_tt oout, * out, omem, * mem = new_mem_buffer(&omem);
	off_t end;
	if (!fin || !fout) {
		fprintf(stderr, "%s <input-nut-file> <output-nut-file>\n", argv[0]);
		return 1;
	}
	printf("Note! This program produces less error resilient files by moving the main headers!\n");

	in = new_input_buffer(&iin, fin);
	out = new_output_buffer(&oout, fout);
	CHECK(find_copy_index(in, mem, &end));

	seek_buf(in, 0, SEEK_SET);
	CHECK(read_headers(in));

	printf("headers: %d\n", (int)bctello(in));

	put_data(out, bctello(in), in->buf); // write headers
	put_data(out, bctello(mem), mem->buf); // write index

	flush_buf(in);

	// copy all data
	while (bctello(in) < end) {
		int len = MIN(PREALLOC_SIZE, end - bctello(in));
		if (ready_read_buf(in, len) < len) return 1;
		put_data(out, len, in->buf_ptr);
		in->buf_ptr += len;
		flush_buf(in);
		printf("%d\r", (int)bctello(in));
	}
	put_data(out, bctello(mem), mem->buf); // write index

	free_buffer(in);
	free_out_buffer(out);
	free_out_buffer(mem);

	fclose(fin);
	fclose(fout);

	return 0;
}
