// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include "nutmerge.h"
#include <string.h>

#define mmioFOURCC(ch0, ch1, ch2, ch3) ((ch0) | ((ch1) << 8) | ((ch2) << 16) | ((ch3) << 24))
#define strFOURCC(str) mmioFOURCC((str)[0], (str)[1], (str)[2], (str)[3])

#define FREAD(file, len, var) do { if (fread((var), 1, (len), (file)) != (len)) return err_unexpected_eof; }while(0)

typedef struct riff_tree_s {
	uint32_t len;
	char name[4];
	char listname[4];
	int type; // 0 - list/tree, 1 - node
	int amount; // if a list, amount of nodes
	struct riff_tree_s * tree; // this is an array (size is 'amount')
	char * data;
	int offset;
} riff_tree_tt;

typedef struct {
	int amount;
	riff_tree_tt * tree;
} full_riff_tree_tt;

typedef struct {
	uint8_t wFormatTag[2];
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} audio_header_tt;

typedef struct {
	uint32_t biSize;
	uint32_t biWidth;
	uint32_t biHeight;
	uint16_t biPlanes;
	uint16_t biBitCount;
	uint8_t biCompression[4];
	uint32_t biSizeImage;
	uint32_t biXPelsPerMeter;
	uint32_t biYPelsPerMeter;
	uint32_t biClrUsed;
	uint32_t biClrImportant;
} video_header_tt;

typedef struct {
	uint32_t dwMicroSecPerFrame;
	uint32_t dwMaxBytesPerSec;
	uint32_t dwReserved1;
	uint32_t dwFlags;
	uint32_t dwTotalFrames;
	uint32_t dwInitialFrames;
	uint32_t dwStreams;
	uint32_t dwSuggestedBufferSize;
	uint32_t dwWidth;
	uint32_t dwHeight;
	uint32_t dwScale;
	uint32_t dwRate;
	uint32_t dwStart;
	uint32_t dwLength;
} avi_header_tt;

typedef struct {
	uint8_t fccType[4];
	uint8_t fccHandler[4];
	uint32_t dwFlags;
	uint32_t dwReserved1;
	uint32_t dwInitialFrames;
	uint32_t dwScale;
	uint32_t dwRate;
	uint32_t dwStart;
	uint32_t dwLength;
	uint32_t dwSuggestedBufferSize;
	uint32_t dwQuality;
	uint32_t dwSampleSize;
	uint16_t rcframe[4];
} avi_stream_header_tt;

typedef struct {
	uint8_t ckid[4];
	uint32_t dwFlags;
	uint32_t dwChunkOffset;
	uint32_t dwChunkLength;
} avi_index_entry_tt;

typedef struct {
	int type; // 0 video, 1 audio
	avi_stream_header_tt * strh;
	video_header_tt * video;
	audio_header_tt * audio;
	int extra_len;
	int last_pts;
	uint8_t * extra;
} avi_stream_context_tt;

struct demuxer_priv_s {
	FILE * in;
	full_riff_tree_tt * riff;
	stream_tt * s;
	avi_header_tt * avih;
	avi_stream_context_tt * stream; // this is an array, free this
	avi_index_entry_tt * index; // this is an array and data
	int packets;
	int cur;
};

#define READ_B(out, ptr, count) do { memcpy(out, ptr, count); ptr += count; } while (0)
#define READ_16(out, ptr) do { out = ((uint8_t*)ptr)[0] | (((uint8_t*)ptr)[1] << 8); ptr += 2; } while (0)
#define READ_32(out, ptr) do { out = ((uint8_t*)ptr)[0] | (((uint8_t*)ptr)[1] << 8) | (((uint8_t*)ptr)[2] << 16) | (((uint8_t*)ptr)[3] << 24); ptr += 4; } while (0)

static void data_to_audio_header(void * data, audio_header_tt * out) {
	uint8_t * p = data;
	READ_B(out->wFormatTag, p, 2);
	READ_16(out->nChannels, p);
	READ_32(out->nSamplesPerSec, p);
	READ_32(out->nAvgBytesPerSec, p);
	READ_16(out->nBlockAlign, p);
	READ_16(out->wBitsPerSample, p);
	READ_16(out->cbSize, p);
}

static void data_to_video_header(void * data, video_header_tt * out) {
	uint8_t * p = data;
	READ_32(out->biSize, p);
	READ_32(out->biWidth, p);
	READ_32(out->biHeight, p);
	READ_16(out->biPlanes, p);
	READ_16(out->biBitCount, p);
	READ_B(out->biCompression, p, 4);
	READ_32(out->biSizeImage, p);
	READ_32(out->biXPelsPerMeter, p);
	READ_32(out->biYPelsPerMeter, p);
	READ_32(out->biClrUsed, p);
	READ_32(out->biClrImportant, p);
}

static void data_to_avi_header(void * data, avi_header_tt * out) {
	uint8_t * p = data;
	READ_32(out->dwMicroSecPerFrame, p);
	READ_32(out->dwMaxBytesPerSec, p);
	READ_32(out->dwReserved1, p);
	READ_32(out->dwFlags, p);
	READ_32(out->dwTotalFrames, p);
	READ_32(out->dwInitialFrames, p);
	READ_32(out->dwStreams, p);
	READ_32(out->dwSuggestedBufferSize, p);
	READ_32(out->dwWidth, p);
	READ_32(out->dwHeight, p);
	READ_32(out->dwScale, p);
	READ_32(out->dwRate, p);
	READ_32(out->dwStart, p);
	READ_32(out->dwLength, p);
}

static void data_to_stream_header(void * data, avi_stream_header_tt * out) {
	uint8_t * p = data;
	READ_B(out->fccType, p, 4);
	READ_B(out->fccHandler, p, 4);
	READ_32(out->dwFlags, p);
	READ_32(out->dwReserved1, p);
	READ_32(out->dwInitialFrames, p);
	READ_32(out->dwScale, p);
	READ_32(out->dwRate, p);
	READ_32(out->dwStart, p);
	READ_32(out->dwLength, p);
	READ_32(out->dwSuggestedBufferSize, p);
	READ_32(out->dwQuality, p);
	READ_32(out->dwSampleSize, p);
	READ_16(out->rcframe[0], p);
	READ_16(out->rcframe[1], p);
	READ_16(out->rcframe[2], p);
	READ_16(out->rcframe[3], p);
}

static void data_to_index_entry(void * data, avi_index_entry_tt * out) {
	uint8_t * p = data;
	READ_B(out->ckid, p, 4);
	READ_32(out->dwFlags, p);
	READ_32(out->dwChunkOffset, p);
	READ_32(out->dwChunkLength, p);
}

static int mk_riff_tree(FILE * in, riff_tree_tt * tree) {
	char lenc[4], * p = lenc;
	int left;
	tree->tree = NULL;
	tree->data = NULL;
	tree->amount = 0;
	tree->offset = ftell(in);
	FREAD(in, 4, tree->name);
	FREAD(in, 4, lenc);
	READ_32(tree->len, p);
	left = tree->len;

	switch(strFOURCC(tree->name)) {
		case mmioFOURCC('L','I','S','T'):
		case mmioFOURCC('R','I','F','F'):
			tree->type = 0;
			FREAD(in, 4, tree->listname); left -= 4; // read real name
			if (!strncmp(tree->listname, "movi", 4)) {
				fseek(in, left, SEEK_CUR);
				break;
			}
			while (left > 0) {
				int err;
				tree->tree =
					realloc(tree->tree, sizeof(riff_tree_tt) * (tree->amount+1));
				if ((err = mk_riff_tree(in, &tree->tree[tree->amount])))
					return err;
				left -= (tree->tree[tree->amount].len + 8);
				if (tree->tree[tree->amount].len & 1) left--;
				tree->amount++;
			}
			break;
		default:
			tree->type = 1;
			tree->data = malloc(left);
			FREAD(in, left, tree->data);
	}
	if (tree->len & 1) fgetc(in);
	return 0;
}

static void free_riff_tree(riff_tree_tt * tree) {
	int i;
	if (!tree) return;

	for (i = 0; i < tree->amount; i++) free_riff_tree(&tree->tree[i]);
	tree->amount = 0;

	free(tree->tree); tree->tree = NULL;
	free(tree->data); tree->data = NULL;
}

static full_riff_tree_tt * init_riff() {
	full_riff_tree_tt * full = malloc(sizeof(full_riff_tree_tt));
	full->amount = 0;
	full->tree = NULL;
	return full;
}

static int get_full_riff_tree(FILE * in, full_riff_tree_tt * full) {
	int err = 0;

	while (1) {
		int c;
		if ((c = fgetc(in)) == EOF) break; ungetc(c, in);
		full->tree = realloc(full->tree, sizeof(riff_tree_tt) * ++full->amount);
		if ((err = mk_riff_tree(in, &full->tree[full->amount - 1]))) goto err_out;
	}
err_out:
	return err;
}

static void uninit_riff(full_riff_tree_tt * full) {
	int i;
	if (!full) return;
	for (i = 0; i < full->amount; i++) free_riff_tree(&full->tree[i]);
	free(full->tree);
	free(full);
}

static int avi_read_stream_header(avi_stream_context_tt * stream, riff_tree_tt * tree) {
	int i;
	assert(tree->type == 0);
	assert(strFOURCC(tree->listname) == mmioFOURCC('s','t','r','l'));

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "strh", 4)) {
			if (tree->tree[i].len != 56) return err_avi_bad_strh_len;
			stream->strh = malloc(sizeof(avi_stream_header_tt));
			data_to_stream_header(tree->tree[i].data, stream->strh);
			break;
		}
	}
	if (i == tree->amount) return err_avi_no_strh;

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "strf", 4)) {
			int len = tree->tree[i].len;
			switch(strFOURCC(stream->strh->fccType)) {
				case mmioFOURCC('v','i','d','s'):
					if (len < 40) return err_avi_bad_vids_len;
					stream->type = 0;
					stream->video = malloc(sizeof(video_header_tt));
					data_to_video_header(tree->tree[i].data, stream->video);
					stream->extra_len = len - 40;
					if (len > 40) stream->extra = (uint8_t*)tree->tree[i].data + 40;
					break;
				case mmioFOURCC('a','u','d','s'):
					if (len < 18) return err_avi_bad_auds_len;
					stream->type = 1;
					stream->audio = malloc(sizeof(audio_header_tt));
					data_to_audio_header(tree->tree[i].data, stream->audio);
					stream->extra_len = len - 18;
					if (len > 18) stream->extra = (uint8_t*)tree->tree[i].data + 18;
					break;
				default:
					return err_avi_bad_strf_type;
			}
			break;
		}
	}
	if (i == tree->amount) return err_avi_no_strf;

	return 0;
}

static int avi_read_main_header(demuxer_priv_tt * avi, const riff_tree_tt * tree) {
	int i, tmp = 0, err;
	assert(tree->type == 0);
	assert(strFOURCC(tree->listname) == mmioFOURCC('h','d','r','l'));

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "avih", 4)) {
			if (tree->tree[i].len != 56) return err_avi_bad_avih_len;
			avi->avih = malloc(sizeof(avi_header_tt));
			data_to_avi_header(tree->tree[i].data, avi->avih);
			break;
		}
	}
	if (i == tree->amount) return err_avi_no_avih;

	if (avi->avih->dwStreams > 200) return err_avi_stream_overflow;
	avi->stream = malloc(avi->avih->dwStreams * sizeof(avi_stream_context_tt));
	for (i = 0; i < avi->avih->dwStreams; i++) {
		avi->stream[i].video = NULL;
		avi->stream[i].audio = NULL;
		avi->stream[i].extra = NULL;
		avi->stream[i].extra_len = 0;
		avi->stream[i].last_pts = 0;
	}
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "strl", 4)) {
			if ((err = avi_read_stream_header(&avi->stream[tmp++], &tree->tree[i]))) return err;
		}
	}
	if (tmp != avi->avih->dwStreams) return err_avi_no_strl;
	return 0;
}

static int avi_read_headers(demuxer_priv_tt * avi) {
	const riff_tree_tt * tree;
	int i, err;
	if ((err = get_full_riff_tree(avi->in, avi->riff))) return err;
	tree = &avi->riff->tree[0];
	if (tree->type != 0) return err_avi_bad_riff;
	if (strncmp(tree->name, "RIFF", 4)) return err_avi_bad_riff;
	if (strncmp(tree->listname, "AVI ", 4)) return err_avi_bad_avi;
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "hdrl", 4)) {
			if ((err = avi_read_main_header(avi, &tree->tree[i]))) return err;
			break;
		}
	}
	if (i == tree->amount) return err_avi_no_hdrl;
	for (i = 0; i < avi->riff->amount; i++) {
		int j, ii;
		tree = &avi->riff->tree[i];
		for (j = 0; j < tree->amount; j++) {
			if (tree->tree[j].type == 1 && !strncmp(tree->tree[j].name, "idx1", 4)) {
				avi->packets = tree->tree[j].len / 16;
				avi->index = calloc(avi->packets, sizeof(avi_index_entry_tt));
				for (ii = 0; ii < avi->packets; ii++) {
					data_to_index_entry((char*)tree->tree[j].data + 16*ii, avi->index + ii);
				}
				break;
			}
		}
		if (j != tree->amount) break;
	}
	if (i == avi->riff->amount) return err_avi_no_idx;
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "movi", 4)) {
			fseek(avi->in, tree->tree[i].offset + 12, SEEK_SET);
			break;
		}
	}
	if (i == tree->amount) return err_avi_no_movi;
	return 0;
}

static int read_headers(demuxer_priv_tt * avi, stream_tt ** streams) {
	int i;
	if ((i = avi_read_headers(avi))) return i;
	for (i = 0; i < avi->avih->dwStreams; i++) {
		if (avi->stream[i].type == 0) { // video
			char * fourccs[] = {"FMP4","fmp4","DIVX","divx",
			                    "DIV1","div1","MP4S","mp4s",
			                    "xvid","XVID","XviD","XVIX",
			                    "M4S2","m4s2","mp4v","MP4V",
			                    "DX50","dx50","BLZ0",
			};
			int j;
			for (j = sizeof(fourccs)/sizeof(fourccs[0]); j--; ) {
				if (!strncmp((char*)avi->stream[i].video->biCompression, fourccs[j], 4)) break;
			}
			if (j == -1) return err_avi_no_video_codec;
		} else {
			if (avi->stream[i].audio->wFormatTag[0] != 0x55 ||
			    avi->stream[i].audio->wFormatTag[1] != 0x00) return err_avi_no_audio_codec;
		}
	}

	*streams = avi->s = malloc(sizeof(stream_tt) * (avi->avih->dwStreams + 1));
	for (i = 0; i < avi->avih->dwStreams; i++) {
		extern demuxer_tt avi_demuxer;
		avi->s[i].stream_id = i;
		avi->s[i].demuxer = avi_demuxer;
		avi->s[i].demuxer.priv = avi;
		avi->s[i].packets_alloc = avi->s[i].npackets = 0;
		avi->s[i].packets = NULL;

		avi->s[i].sh.type = avi->stream[i].type;
		avi->s[i].sh.time_base.den = avi->stream[i].strh->dwRate;
		avi->s[i].sh.time_base.num = avi->stream[i].strh->dwScale;
		avi->s[i].sh.fixed_fps = 1;
		avi->s[i].sh.codec_specific_len = avi->stream[i].extra_len;
		avi->s[i].sh.codec_specific = avi->stream[i].extra;
		if (avi->stream[i].type == 0) { // video
			avi->s[i].sh.fourcc_len = 4;
			avi->s[i].sh.fourcc = (uint8_t*)"mp4v";
			avi->s[i].codec_id = e_mpeg4;
			avi->s[i].sh.decode_delay = 1;

			avi->s[i].sh.width = avi->stream[i].video->biWidth;
			avi->s[i].sh.height = avi->stream[i].video->biHeight;
			avi->s[i].sh.sample_width = 0;
			avi->s[i].sh.sample_height = 0;
			avi->s[i].sh.colorspace_type = 0;
		} else { // audio
			avi->s[i].sh.fourcc_len = 4;
			avi->s[i].sh.fourcc = (uint8_t*)"mp3 ";
			avi->s[i].codec_id = e_mp3;
			avi->s[i].sh.decode_delay = 0;

			avi->s[i].sh.codec_specific_len = 0;

			avi->s[i].sh.samplerate_num = avi->stream[i].audio->nSamplesPerSec;
			avi->s[i].sh.samplerate_denom = 1;
			avi->s[i].sh.channel_count = avi->stream[i].audio->nChannels;
		}
	}
	avi->s[i].stream_id = -1;
	return 0;
}

static int fill_buffer(demuxer_priv_tt * avi) {
	char fourcc[4], lenc[4], * plen = lenc;
	int len;
	packet_tt p;
	if (ftell(avi->in) & 1) fgetc(avi->in);

	if (avi->cur >= avi->packets) return -1;

	FREAD(avi->in, 4, fourcc);
	FREAD(avi->in, 4, lenc);
	READ_32(len, plen);
	p.p.len = len;
	p.p.flags = (avi->index[avi->cur++].dwFlags & 0x10) ? NUT_FLAG_KEY : 0;
	p.p.stream = (fourcc[0] - '0') * 10 + (fourcc[1] - '0');
	p.p.next_pts = p.p.pts = 0;
	if ((unsigned)(fourcc[0] - '0') > 9 || (unsigned)(fourcc[1] - '0') > 9 || p.p.stream >= avi->avih->dwStreams) {
		fprintf(stderr, "%d %4.4s\n", avi->cur, fourcc);
		return err_avi_bad_packet;
	}
	if (p.p.stream == 1) {
		// 0.5 secs of audio or a single packet
		int samplesize = avi->stream[p.p.stream].strh->dwSampleSize;

		p.p.pts = avi->stream[p.p.stream].last_pts;
		if (samplesize) avi->stream[p.p.stream].last_pts += p.p.len / samplesize;
		else avi->stream[p.p.stream].last_pts++;

		if (!(p.p.flags & NUT_FLAG_KEY)) printf("Error detected stream %d frame %d\n", p.p.stream, (int)p.p.pts);
		p.p.flags |= NUT_FLAG_KEY;
	}
	p.buf = malloc(p.p.len);
	FREAD(avi->in, p.p.len, p.buf);
	push_packet(&avi->s[p.p.stream], &p);
	return 0;
}

static demuxer_priv_tt * init(FILE * in) {
	demuxer_priv_tt * avi = malloc(sizeof(demuxer_priv_tt));
	avi->avih = NULL;
	avi->stream = NULL;
	avi->index = NULL;
	avi->in = in;
	avi->riff = init_riff();
	avi->cur = 0;
	avi->s = NULL;
	return avi;
}

static void uninit(demuxer_priv_tt * avi) {
	int i, streams = avi->avih ? avi->avih->dwStreams : 0;
	uninit_riff(avi->riff);
	if (avi->stream) for (i = 0; i < streams; i++) {
		free(avi->stream[i].strh);
		free(avi->stream[i].video);
		free(avi->stream[i].audio);
	}
	free(avi->stream);
	free(avi->avih);
	free(avi->index);
	free_streams(avi->s);
	free(avi->s);
	free(avi);
}

demuxer_tt avi_demuxer = {
	"avi",
	init,
	read_headers,
	fill_buffer,
	uninit
};
