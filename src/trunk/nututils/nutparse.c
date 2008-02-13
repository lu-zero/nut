/*
 * nutparse.c - dumps a .nut file as text
 * Copyright (c) 2007 Clemens Ladisch <clemens@ladisch.de>
 *
 * This file is available under the MIT/X license, see COPYING.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#define MAIN_STARTCODE		UINT64_C(0x4e4d7a561f5f04ad)
#define STREAM_STARTCODE	UINT64_C(0x4e5311405bf2f9db)
#define SYNCPOINT_STARTCODE	UINT64_C(0x4e4be4adeeca4569)
#define INDEX_STARTCODE		UINT64_C(0x4e58dd672f23e64e)
#define INFO_STARTCODE		UINT64_C(0x4e49ab68b596ba78)

#define FLAG_KEY	0x0001
#define FLAG_EOR	0x0002
#define FLAG_CODED_PTS	0x0008
#define FLAG_STREAM_ID	0x0010
#define FLAG_SIZE_MSB	0x0020
#define FLAG_CHECKSUM	0x0040
#define FLAG_RESERVED	0x0080
#define FLAG_MATCH_TIME	0x0800
#define FLAG_CODED	0x1000
#define FLAG_INVALID	0x2000

#define ABS(x) ((x) > 0 ? (x) : -(x))

static FILE *input;
static uint64_t file_position;
static uint32_t crc;
static int stream_count = -1;
static uint64_t max_distance;
static int time_base_count = -1;
static struct {
	uint64_t num;
	uint64_t denom;
} *time_bases;
static struct {
	uint64_t flags;
	uint64_t stream_id;
	uint64_t data_size_mul;
	uint64_t data_size_lsb;
	int64_t pts_delta;
	uint64_t reserved_count;
} frame_types[256];
static struct {
	int time_base_id;
	int msb_pts_shift;
	uint64_t max_pts_distance;
	uint64_t last_pts;
} *streams;

static void error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	putc('\n', stderr);
	exit(EXIT_FAILURE);
}

static void usage(void)
{
	puts("Usage: nutparse input.nut");
}

static void version(void)
{
	puts("nutparse version 0.1");
}

static uint64_t gcd(uint64_t a, uint64_t b)
{
	while (b) {
		uint64_t t = b;
		b = a % b;
		a = t;
	}
	return a;
}

static inline void reset_checksum(void)
{
	crc = 0;
}

static void update_checksum(uint8_t byte)
{
	static const uint32_t crc_table[16] = {
		0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
		0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
		0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61,
		0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
	};

	crc ^= byte << 24;
	crc = (crc << 4) ^ crc_table[crc >> 28];
	crc = (crc << 4) ^ crc_table[crc >> 28];
}

static uint8_t read_byte(void)
{
	int c = getc(input);
	if (c == EOF)
		error("unexpected EOF");
	++file_position;
	update_checksum(c);
	return c;
}

static uint64_t read_fixed(int bytes)
{
	uint64_t value = 0;
	int i;

	for (i = bytes; i > 0; --i)
		value = (value << 8) | read_byte();
	return value;
}

static uint64_t read_var(void)
{
	uint64_t value = 0;
	uint8_t byte;

	do {
		if (value & UINT64_C(0xe000000000000000))
			error("value too big (more than 64 bits)");
		byte = read_byte();
		value = (value << 7) | (byte & 0x7f);
	} while (byte & 0x80);
	return value;
}

static uint64_t read_var_restricted(void)
{
	uint64_t value = 0;
	uint8_t byte;
	int padding = 0;

	for (;;) {
		byte = read_byte();
		if (byte != 0x80)
			break;
		++padding;
		if (padding > 8)
			error("more than 8 consecutive padding bytes");
	}
	for (;;) {
		if (value & UINT64_C(0xe000000000000000))
			error("value too big (more than 64 bits)");
		value = (value << 7) | (byte & 0x7f);
		if ((byte & 0x80) == 0)
			break;
		byte = read_byte();
	}
	return value;
}

static int64_t convert_to_signed(uint64_t temp)
{
	++temp;
	if (temp == 0)
		error("signed value too big (more than 63 bits)");
	if (temp & 1)
		return -(temp >> 1);
	else
		return temp >> 1;
}

static int64_t read_svar(void)
{
	return convert_to_signed(read_var());
}

static int64_t read_svar_restricted(void)
{
	return convert_to_signed(read_var_restricted());
}

static uint64_t convert_ts(uint64_t t, int tb_from, int tb_to)
{
	uint64_t ln = time_bases[tb_from].num * time_bases[tb_to].denom;
	uint64_t d1 = time_bases[tb_from].denom;
	return (ln / d1 * t + ln % d1 * t / d1) / time_bases[tb_to].num;
}

static double time_in_s(uint64_t t, int time_base_id)
{
	return (double)t * time_bases[time_base_id].num / time_bases[time_base_id].denom;
}

static void parse_file_id(void)
{
	char id[25];

	if (fread(id, 1, 25, input) != 25)
		error("unexpected EOF");
	if (memcmp(id, "nut/multimedia container", 25) != 0)
		error("invalid file ID string");
	puts(id);
	file_position = 25;
}

static void parse_main_header(void)
{
	uint64_t value;
	uint64_t tmp_flag;
	uint64_t tmp_fields;
	int64_t tmp_pts;
	uint64_t tmp_mul;
	uint64_t tmp_stream;
	int64_t tmp_match;
	uint64_t tmp_size;
	uint64_t tmp_res;
	uint64_t count;
	int i, j;
	uint64_t j64;

	value = read_var();
	printf("  version: %"PRIu64"\n", value);
	if (value != 2 && value != 3) /* TODO: remove 2 */
		error("invalid version");
	value = read_var();
	printf("  stream_count: %"PRIu64"\n", value);
	if (value > 0x1000)
		error("implementation limit exceeded");
	stream_count = value;
	streams = calloc(stream_count, sizeof *streams);
	if (!streams)
		error("out of memory");
	value = read_var();
	printf("  max_distance: %"PRIu64"\n", value);
	max_distance = value;
	value = read_var();
	printf("  time_base_count: %"PRIu64"\n", value);
	if (!value)
		error("time_base_count must not be zero");
	if (value > 0x1000)
		error("implementation limit exceeded");
	time_base_count = value;
	time_bases = calloc(time_base_count, sizeof *time_bases);
	if (!time_bases)
		error("out of memory");
	for (i = 0; i < time_base_count; ++i) {
		time_bases[i].num = read_var();
		time_bases[i].denom = read_var();
		printf("  time_base[%d]: %"PRIu64"/%"PRIu64"\n",
		       i, time_bases[i].num, time_bases[i].denom);
		if (!time_bases[i].num || !time_bases[i].denom)
			error("time base values must not be zero");
		if (gcd(time_bases[i].num, time_bases[i].denom) != 1)
			error("time base values must be relatively prime");
		if (time_bases[i].denom >= (1 << 31))
			error("time base denominator is not less than 2^31");
		for (j = 0; j < i; ++j)
			if (time_bases[i].num == time_bases[j].num &&
			    time_bases[i].denom == time_bases[j].denom)
				error("time base is identical to time base %d", j);
	}
	tmp_pts = 0;
	tmp_mul = 1;
	tmp_stream = 0;
	tmp_match = 1 - (INT64_C(1) << 62);
	for (i = 0; i < 256; ) {
		tmp_flag = read_var();
		printf("  tmp_flag: %"PRIu64"\n", tmp_flag);
		tmp_fields = read_var();
		printf("  tmp_fields: %"PRIu64"\n", tmp_fields);
		if (tmp_fields > 0) {
			tmp_pts = read_svar();
			printf("  tmp_pts: %"PRId64"\n", tmp_pts);
			if (tmp_pts <= -16384 || tmp_pts >= 16384)
				error("absolute pts difference must be less than 16384");
		}
		if (tmp_fields > 1) {
			tmp_mul = read_var();
			printf("  tmp_mul: %"PRIu64"\n", tmp_mul);
			if (tmp_mul >= 16384)
				error("data size multiplicator must be less than 16384");
		}
		if (tmp_fields > 2) {
			tmp_stream = read_var();
			printf("  tmp_stream: %"PRIu64"\n", tmp_stream);
			if (tmp_stream >= stream_count)
				error("invalid stream id");
			if (tmp_stream >= 250)
				error("stream id must be less than 250");
		}
		if (tmp_fields > 3) {
			tmp_size = read_var();
			printf("  tmp_size: %"PRIu64"\n", tmp_size);
			if (tmp_size >= 16384)
				error("data size lsb must be less than 16384");
		} else
			tmp_size = 0;
		if (tmp_fields > 4) {
			tmp_res = read_var();
			printf("  tmp_res: %"PRIu64"\n", tmp_res);
			if (tmp_res >= 256)
				error("reserved count must be less than 256");
		} else
			tmp_res = 0;
		if (tmp_fields > 5) {
			count = read_var();
			printf("  count: %"PRIu64"\n", count);
		} else {
			if (tmp_size > tmp_mul)
				error("count underflow");
			count = tmp_mul - tmp_size;
			if (!count)
				error("count is zero");
		}
		if (tmp_fields > 6) {
			tmp_match = read_svar();
			printf("  tmp_match: %"PRId64"\n", tmp_match);
			if ((tmp_match <= -32768 || tmp_match >= 32768) &&
			    tmp_match != 1 - (INT64_C(1) << 62))
				error("absolute delta match time must be less than 32768");
		}
		for (j64 = 7; j64 < tmp_fields; ++j64)
			printf("  tmp_reserved[%"PRIu64"]: %"PRIu64"\n", j64, read_var());
		for (j = 0; j < count && i < 256; ++j, ++i) {
			if (i == 'N') {
				frame_types[i].flags = FLAG_INVALID;
				--j;
				continue;
			}
			if (j == 0) {
				printf("  frame_type[0x%02x]:", i);
				if (tmp_flag & FLAG_KEY)
					printf(" key");
				if (tmp_flag & FLAG_EOR)
					printf(" eor");
				if (tmp_flag & FLAG_CODED_PTS)
					printf(" coded_pts");
				if (tmp_flag & FLAG_STREAM_ID)
					printf(" stream_id");
				if (tmp_flag & FLAG_SIZE_MSB)
					printf(" size_msb");
				if (tmp_flag & FLAG_CHECKSUM)
					printf(" checksum");
				if (tmp_flag & FLAG_RESERVED)
					printf(" reserved");
				if (tmp_flag & FLAG_MATCH_TIME)
					printf(" match_time");
				if (tmp_flag & FLAG_CODED)
					printf(" flag_coded");
				if (tmp_flag & FLAG_INVALID)
					printf(" invalid");
				if (!(tmp_flag & (FLAG_STREAM_ID | FLAG_INVALID)) || (tmp_flag & FLAG_CODED))
					printf(" stream_id: %"PRIu64, tmp_stream);
				if (!(tmp_flag & (FLAG_SIZE_MSB | FLAG_INVALID)) || (tmp_flag & FLAG_CODED)) {
					printf(" data_size_mul: %"PRIu64, tmp_mul);
					printf(" data_size_lsb: %"PRIu64, tmp_size);
				}
				if (!(tmp_flag & (FLAG_CODED_PTS | FLAG_INVALID)) || (tmp_flag & FLAG_CODED))
					printf(" pts_delta: %"PRId64, tmp_pts);
				if ((!(tmp_flag & (FLAG_RESERVED | FLAG_INVALID)) || (tmp_flag & FLAG_CODED)) &&
				    tmp_res != 0)
					printf(" reserved_count: %"PRIu64, tmp_res);
				putchar('\n');
			}
			frame_types[i].flags = tmp_flag;
			frame_types[i].stream_id = tmp_stream;
			frame_types[i].data_size_mul = tmp_mul;
			frame_types[i].data_size_lsb = tmp_size + j;
			frame_types[i].pts_delta = tmp_pts;
			frame_types[i].reserved_count = tmp_res;
		}
	}
}

static void parse_stream_header(void)
{
	uint64_t value, value2;
	int id, class, i;
	uint8_t fourcc[4];

	if (stream_count < 0)
		error("no main_header before stream_header");
	value = read_var();
	printf("  stream_id: %"PRIu64"\n", value);
	if (value >= stream_count)
		error("invalid stream id");
	id = value;
	value = read_var();
	printf("  stream_class: %"PRIu64, value);
	if (value < 4) {
		static const char *const class_names[4] = {
			"video", "audio", "subtitles", "userdata"
		};
		class = value;
		printf(" (%s)", class_names[class]);
	} else
		class = -1;
	putchar('\n');
	value = read_var();
	if (value != 2 && value != 4)
		error("invalid fourcc length");
	printf("  fourcc: ");
	for (i = 0; i < value; ++i)
		fourcc[i] = read_byte();
	if (value == 2)
		printf("0x%02x%02x\n", fourcc[1], fourcc[0]);
	else {
		printf("%02x,%02x,%02x,%02x (", fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
		for (i = 0; i < 4; ++i)
			if (fourcc[i] >= 32 && fourcc[i] < 127)
				putchar(fourcc[i]);
			else
				putchar('.');
		puts(")");
	}
	value = read_var();
	printf("  time_base_id: %"PRIu64"\n", value);
	if (value >= time_base_count)
		error("invalid time base id");
	streams[id].time_base_id = value;
	value = read_var();
	printf("  msb_pts_shift: %"PRIu64"\n", value);
	if (value >= 16)
		error("invalid msb_pts_shift value");
	streams[id].msb_pts_shift = value;
	value = read_var();
	printf("  max_pts_distance: %"PRIu64"\n", value);
	streams[id].max_pts_distance = value;
	printf("  decode_delay: %"PRIu64"\n", read_var());
	value = read_var();
	printf("  stream_flags: %"PRIu64, value);
	if (value & 1)
		printf(" (fixed_fps)");
	putchar('\n');
	value = read_var();
	printf("  codec_specific_data: %"PRIu64" bytes\n", value);
	for (; value > 0; --value)
		read_byte();
	if (class == 0) {
		value = read_var();
		printf("  width: %"PRIu64"\n", value);
		if (!value)
			error("width must not be zero");
		value = read_var();
		printf("  height: %"PRIu64"\n", value);
		if (!value)
			error("height must not be zero");
		value = read_var();
		printf("  sample_width: %"PRIu64"\n", value);
		value2 = read_var();
		printf("  sample_height: %"PRIu64"\n", value2);
		if ((!value) ^ (!value2))
			error("none or both of sample_width/height must be zero");
		if (value && gcd(value, value2) != 1)
			error("sample_width/height must be relatively prime");
		printf("  colorspace_type: %"PRIu64"\n", read_var());
	} else if (class == 1) {
		value = read_var();
		printf("  samplerate_num: %"PRIu64"\n", value);
		if (!value)
			error("sample rate must not be zero");
		value = read_var();
		printf("  samplerate_denom: %"PRIu64"\n", value);
		if (!value)
			error("invalid sample rate");
		value = read_var();
		printf("  channel_count: %"PRIu64"\n", value);
		if (!value)
			error("channel count must not be zero");
	}
}

static void parse_syncpoint(uint64_t pos)
{
	uint64_t value, pts;
	int tbid, i;

	if (stream_count < 0)
		error("no main_header before syncpoint");
	value = read_var();
	pts = value / time_base_count;
	tbid = value % time_base_count;
	printf("  global_key_pts: %"PRIu64"@%d (%.3lf s)\n", pts, tbid, time_in_s(pts, tbid));
	for (i = 0; i < stream_count; ++i)
		streams[i].last_pts = convert_ts(pts, tbid, streams[i].time_base_id);
	value = read_var();
	printf("  back_ptr_div16: %"PRIu64" (0x%"PRIx64")\n", value, pos - (value * 16 + 15));
}

static void parse_index(void)
{
	uint64_t value, pts;
	uint64_t syncpoints;
	uint64_t last_pos;
	int64_t last_pts;
	uint64_t j, x, xx, n, a, b;
	int tbid, i, type, flag = 0, has_keyframe;

	if (stream_count < 0)
		error("no main_header before index");
	value = read_var();
	pts = value / time_base_count;
	tbid = value % time_base_count;
	printf("  max_pts: %"PRIu64"@%d (%.3lf s)\n", pts, tbid, time_in_s(pts, tbid));
	syncpoints = read_var();
	printf("  syncpoints %"PRIu64"\n", syncpoints);
	last_pos = 0;
	for (j = 0; j < syncpoints; ++j) {
		value = read_var();
		printf("  syncpoint_pos_div16: %"PRIu64" (0x%"PRIx64")\n", value, (last_pos + value) << 4);
		last_pos += value;
	}
	for (i = 0; i < stream_count; ++i) {
		last_pts = -1;
		for (j = 0; j < syncpoints; ) {
			x = read_var();
			printf("  x: %"PRIu64" ", x);
			type = x & 1;
			x >>= 1;
			n = j;
			if (type) {
				flag = x & 1;
				x >>= 1;
				printf("(%"PRIu64"*%d,%d)\n", x, flag, !flag);
				n += x + 1;
			} else {
				putchar('(');
				for (xx = x; xx != 1; ++n, xx >>= 1)
					printf("%d", (int)xx & 1);
				puts(")");
			}
			for (; j < n && j < syncpoints; ++j) {
				if (type) {
					if (x > 0) {
						has_keyframe = flag;
						--x;
					} else
						has_keyframe = !flag;
				} else {
					has_keyframe = (int)x & 1;
					x >>= 1;
				}
				if (!has_keyframe)
					continue;
				a = read_var();
				if (!a) {
					printf("  A: %"PRIu64"\n", a);
					a = read_var();
					printf("  A: %"PRIu64" (keyframe_pts[%"PRIu64"][%d]: %"PRIu64")\n", a, j, i, last_pts + a);
					b = read_var();
					printf("  B: %"PRIu64" (eor_pts[%"PRIu64"][%d]: %"PRIu64")\n", b, j, i, last_pts + a + b);
				} else {
					printf("  A: %"PRIu64" (keyframe_pts[%"PRIu64"][%d]: %"PRIu64")\n", a, j, i, last_pts + a);
					b = 0;
				}
				last_pts += a + b;
			}
		}
	}
}

static void parse_info_packet(void)
{
	uint64_t value, pts, count;
	int64_t type;
	int tbid;

	if (stream_count < 0)
		error("no main_header before info_packet");
	value = read_var();
	printf("  stream_id_plus1: %"PRIu64"\n", value);
	if (value > stream_count)
		error("invalid stream id");
	printf("  chapter_id: %"PRIu64"\n", read_svar());
	value = read_var();
	pts = value / time_base_count;
	tbid = value % time_base_count;
	printf("  chapter_start: %"PRIu64"@%d (%.3lf s)\n", pts, tbid, time_in_s(pts, tbid));
	value = read_var();
	printf("  chapter_len: %"PRIu64" (%.3lf s)\n", value, time_in_s(value, tbid));
	count = read_var();
	printf("  count: %"PRIu64"\n", count);
	for (; count > 0; --count) {
		value = read_var();
		printf("  name: \"");
		for (; value > 0; --value)
			fputc(read_byte(), stdout);
		puts("\"");
		type = read_svar();
		if (type == -1) {
			puts("  value: -1 (type: UTF-8)");
			value = read_var();
			printf("  value: \"");
			for (; value > 0; --value)
				fputc(read_byte(), stdout);
			puts("\"");
		} else if (type == -2) {
			puts("  value: -2 (type: user-defined)");
			value = read_var();
			printf("  type: \"");
			for (; value > 0; --value)
				fputc(read_byte(), stdout);
			puts("\"");
			value = read_var();
			printf("  value: ");
			if (value > 0)
				printf("%02x", read_byte());
			for (--value; value > 0; --value)
				printf(",%02x", read_byte());
			putchar('\n');
		} else if (type == -3) {
			puts("  value: -3 (type: s)");
			printf("  value: %"PRId64"\n", read_svar());
		} else if (type == -4) {
			puts("  value: -4 (type: t)");
			value = read_var();
			pts = value / time_base_count;
			tbid = value % time_base_count;
			printf("  value: %"PRIu64"@%d (%.3lf s)\n", pts, tbid, time_in_s(pts, tbid));
		} else if (value < -4) {
			printf("  value: %"PRId64" (type: r)\n", type);
			printf("  value: %"PRId64"/%"PRId64"\n", read_svar(), -type - 4);
		} else
			printf("  value: %"PRId64" (type: v)\n", type);
	}
}

static void parse_packet(void)
{
	uint64_t startcode;
	uint64_t forward_ptr;
	uint32_t checksum, actual_checksum;
	uint64_t end_pos;
	uint64_t packet_start = file_position - 1;
	int i;

	/* TODO: check max_distance */
	startcode = read_fixed(7) | ((uint64_t)'N' << 56);
	switch (startcode) {
	case MAIN_STARTCODE:
		printf("main_header");
		break;
	case STREAM_STARTCODE:
		printf("stream_header");
		break;
	case SYNCPOINT_STARTCODE:
		printf("syncpoint");
		break;
	case INDEX_STARTCODE:
		printf("index");
		break;
	case INFO_STARTCODE:
		printf("info_packet");
		break;
	default:
		printf("unknown_packet");
		break;
	}
	printf(" at 0x%"PRIx64" [0x%"PRIx64, packet_start, packet_start - 1);
	for (i = 2; i < 16; i++) printf(" 0x%"PRIx64, packet_start - i);
	printf("]\n");
	printf("  startcode: 0x%016"PRIx64"\n", startcode);
	forward_ptr = read_var_restricted();
	printf("  forward_ptr: %"PRIu64"\n", forward_ptr);
	if (forward_ptr > 4096) {
		actual_checksum = crc;
		checksum = read_fixed(4);
		printf("  header_checksum: 0x%08"PRIx32"\n", checksum);
		if (checksum != actual_checksum)
			error("invalid checksum");
	}
	reset_checksum();
	end_pos = file_position + forward_ptr - 4;
	switch (startcode) {
	case MAIN_STARTCODE:
		parse_main_header();
		break;
	case STREAM_STARTCODE:
		parse_stream_header();
		break;
	case SYNCPOINT_STARTCODE:
		parse_syncpoint(packet_start);
		break;
	case INDEX_STARTCODE:
		end_pos -= 8; /* special handling for index_ptr behind reserved_bytes */
		parse_index();
		break;
	case INFO_STARTCODE:
		parse_info_packet();
		break;
	}
	if (file_position > end_pos)
		error("packet has more data than indicated by forward_ptr");
	else if (file_position < end_pos) {
		printf("  reserved_bytes: %02x", read_byte());
		while (file_position < end_pos)
			printf(",%02x", read_byte());
		putchar('\n');
	}
	if (startcode == INDEX_STARTCODE)
		printf("  index_ptr: 0x%016"PRIx64"\n", read_fixed(8));
	actual_checksum = crc;
	checksum = read_fixed(4);
	printf("  checksum: 0x%08"PRIx32"\n", checksum);
	if (checksum != actual_checksum)
		error("invalid checksum");
}

static void parse_frame(int frame_type)
{
	uint64_t value;
	uint64_t frame_flags;
	uint64_t stream_id;
	uint64_t coded_pts = 0;
	int64_t pts_delta;
	uint64_t data_size_msb;
	int64_t match_time_delta;
	uint64_t reserved_count;
	uint64_t data_size;
	uint64_t old_last_pts;
	uint32_t checksum, actual_checksum;

	printf("frame_0x%02x\n", frame_type);
	if (stream_count < 0)
		error("no main_header before frame");
	frame_flags = frame_types[frame_type].flags;
	stream_id = frame_types[frame_type].stream_id;
	pts_delta = frame_types[frame_type].pts_delta;
	data_size_msb = 0;
	reserved_count = frame_types[frame_type].reserved_count;
	if (frame_flags & FLAG_CODED) {
		value = read_var_restricted();
		printf("  coded_flags: %"PRIu64"\n", value);
		frame_flags ^= value;
	}
	if (frame_flags & FLAG_INVALID)
		error("frame type is invalid");
	if (frame_flags & FLAG_STREAM_ID) {
		stream_id = read_var_restricted();
		printf("  stream_id: %"PRIu64"\n", stream_id);
		if (stream_id >= stream_count)
			error("invalid stream id");
	}
	if (frame_flags & FLAG_CODED_PTS) {
		coded_pts = read_var_restricted();
		printf("  coded_pts: %"PRIu64"\n", coded_pts);
	}
	if (frame_flags & FLAG_SIZE_MSB) {
		data_size_msb = read_var_restricted();
		printf("  data_size_msb: %"PRIu64"\n", data_size_msb);
	}
	if (frame_flags & FLAG_MATCH_TIME) {
		match_time_delta = read_svar_restricted();
		printf("  match_time_delta: %"PRId64"\n", match_time_delta);
	}
	if (frame_flags & FLAG_RESERVED) {
		reserved_count = read_var_restricted();
		printf("  reserved_count: %"PRIu64"\n", reserved_count);
	}
	for (; reserved_count > 0; --reserved_count)
		printf("  reserved: %"PRIu64"\n", read_var_restricted());
	if (frame_flags & FLAG_CHECKSUM) {
		actual_checksum = crc;
		checksum = read_fixed(4);
		printf("  checksum: 0x%08"PRIx32"\n", checksum);
		if (checksum != actual_checksum)
			error("invalid checksum");
	}
	data_size = data_size_msb * frame_types[frame_type].data_size_mul + frame_types[frame_type].data_size_lsb;
	printf("  frame:");
	if (frame_flags & FLAG_KEY)
		printf(" key");
	if (frame_flags & FLAG_EOR)
		printf(" eor");
	if (!(frame_flags & FLAG_STREAM_ID))
		printf(" stream_id: %"PRIu64, stream_id);
	old_last_pts = streams[stream_id].last_pts;
	if (!(frame_flags & FLAG_CODED_PTS))
		streams[stream_id].last_pts += frame_types[frame_type].pts_delta;
	else if (coded_pts >= (1 << streams[stream_id].msb_pts_shift))
		streams[stream_id].last_pts = coded_pts - (1 << streams[stream_id].msb_pts_shift);
	else {
		unsigned int mask = (1 << streams[stream_id].msb_pts_shift) - 1;
		int64_t delta = streams[stream_id].last_pts - mask / 2;
		streams[stream_id].last_pts = ((coded_pts - delta) & mask) + delta;
	}
	printf(" pts: %"PRIi64" (%.3lf s)", streams[stream_id].last_pts,
	       time_in_s(streams[stream_id].last_pts, streams[stream_id].time_base_id));
	/* TODO: check pts monotonicity and other pts/dts constraints */
	printf(" data_size: %"PRIu64"\n", data_size_msb * frame_types[frame_type].data_size_mul + frame_types[frame_type].data_size_lsb);
	if ((frame_flags & (FLAG_KEY | FLAG_EOR)) == FLAG_EOR)
		error("eor frames must be key frames");
	if ((frame_flags & FLAG_EOR) && data_size)
		error("eor frames must have zero size");
	if (!(frame_flags & FLAG_CHECKSUM)) {
		if (data_size > 2 * max_distance)
			error("large frame must have a checksum");
		if (ABS((int64_t)streams[stream_id].last_pts - (int64_t)old_last_pts) > streams[stream_id].max_pts_distance)
			error("max_pts_distance exceeded without a checksum");
	}
	for (; data_size > 0; --data_size)
		read_byte();
}

int main(int argc, char *argv[])
{
	static char short_options[] = "hV";
	static struct option long_options[] = {
		{"help", 0, NULL, 'h'},
		{"version", 0, NULL, 'V'},
		{}
	};
	int c;

	while ((c = getopt_long(argc, argv, short_options,
				long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage();
			return 0;
		case 'V':
			version();
			return 0;
		default:
			error("Try `nutparse --help' for more information.");
		}
	}

	if (argc - optind != 1) {
		usage();
		return 1;
	}

	input = fopen(argv[1], "rb");
	if (!input)
		error("Cannot open %s: %s", argv[1], strerror(errno));

	parse_file_id();
	for (;;) {
		reset_checksum();
		c = fgetc(input);
		if (c == EOF)
			break;
		++file_position;
		update_checksum(c);
		if (c == 'N')
			parse_packet();
		else
			parse_frame(c);
	}

	fclose(input);
	return 0;
}
