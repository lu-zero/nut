#include "nutmerge.h"
#include "avireader.h"

static int avi_read_stream_header(AVIStreamContext * stream, riff_tree_t * tree) {
	int i, j;
	assert(tree->type == 0);
	assert(strFOURCC(tree->listname) == mmioFOURCC('s','t','r','l'));

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "strh", 4)) {
			if (tree->tree[i].len != 56) return 2;
			stream->strh = (AVIStreamHeader*)tree->tree[i].data;
			break;
		}
	}
	if (i == tree->amount) return 2;

	for(i = 2; i < 12; i++) FIXENDIAN32(((uint32_t*)stream->strh)[i]);

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "strf", 4)) {
			int len = tree->tree[i].len;
			switch(strFOURCC(stream->strh->fccType)) {
				case mmioFOURCC('v','i','d','s'):
					if (len < 40) return 2;
					stream->type = 0;
					stream->video = (BITMAPINFOHEADER*)tree->tree[i].data;
					for(j = 0; j < 3; j++)  FIXENDIAN32(((uint32_t*)stream->video)[j]);
					for(j = 6; j < 8; j++)  FIXENDIAN16(((uint16_t*)stream->video)[j]);
					for(j = 5; j < 10; j++) FIXENDIAN32(((uint32_t*)stream->video)[j]);
					stream->extra_len = len - 40;
					if (len > 40) stream->extra = (uint8_t*)tree->tree[i].data + 40;
					break;
				case mmioFOURCC('a','u','d','s'):
					if (len < 18) return 2;
					stream->type = 1;
					stream->audio = (WAVEFORMATEX *)tree->tree[i].data;
					for(j = 1; j < 2; j++) FIXENDIAN16(((uint16_t*)stream->audio)[j]);
					for(j = 1; j < 3; j++) FIXENDIAN32(((uint32_t*)stream->audio)[j]);
					for(j = 6; j < 9; j++) FIXENDIAN16(((uint16_t*)stream->audio)[j]);
					stream->extra_len = len - 18;
					if (len > 18) stream->extra = (uint8_t*)tree->tree[i].data + 18;
					break;
				default:
					return 3;
			}
			break;
		}
	}
	if (i == tree->amount) return 2;

	return 0;
}

static int avi_read_main_header(AVIContext * avi, const riff_tree_t * tree) {
	int i, tmp = 0, err;
	assert(tree->type == 0);
	assert(strFOURCC(tree->listname) == mmioFOURCC('h','d','r','l'));

	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 1 && !strncmp(tree->tree[i].name, "avih", 4)) {
			if (tree->tree[i].len != 56) return 2;
			avi->avih = (MainAVIHeader*)tree->tree[i].data;
			break;
		}
	}
	if (i == tree->amount) return 2;

	for(i = 0; i < 14; i++) FIXENDIAN32(((uint32_t*)avi->avih)[i]);

	if (avi->avih->dwStreams > 200) return 2;
	avi->stream = malloc(avi->avih->dwStreams * sizeof(AVIStreamContext));
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
	if (tmp != avi->avih->dwStreams) return 2;
	return 0;
}

int avi_read_headers(AVIContext * avi) {
	const riff_tree_t * tree;
	int i, err;
	if ((err = get_full_riff_tree(avi->in, avi->riff))) return err;
	tree = &avi->riff->tree[0];
	if (tree->type != 0) return 2;
	if (strncmp(tree->name, "RIFF", 4)) return 2;
	if (strncmp(tree->listname, "AVI ", 4)) return 2;
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "hdrl", 4)) {
			if ((err = avi_read_main_header(avi, &tree->tree[i]))) return err;
			break;
		}
	}
	if (i == tree->amount) return 2;
	for (i = 0; i < avi->riff->amount; i++) {
		int j;
		tree = &avi->riff->tree[i];
		for (j = 0; j < tree->amount; j++) {
			if (tree->tree[j].type == 1 && !strncmp(tree->tree[j].name, "idx1", 4)) {
				avi->index = (AVIINDEXENTRY *)tree->tree[j].data;
				avi->packets = tree->tree[j].len / 16;
				for (i = 0; i < avi->packets; i++) {
					FIXENDIAN32(avi->index[i].dwFlags);
					FIXENDIAN32(avi->index[i].dwChunkOffset);
					FIXENDIAN32(avi->index[i].dwChunkLength);
				}
				break;
			}
		}
		if (j != tree->amount) break;
	}
	if (i == avi->riff->amount) return 2;
	for (i = 0; i < tree->amount; i++) {
		if (tree->tree[i].type == 0 && !strncmp(tree->tree[i].listname, "movi", 4)) {
			fseek(avi->in, tree->tree[i].offset + 12, SEEK_SET);
			break;
		}
	}
	if (i == tree->amount) return 2;
	return 0;
}

AVIContext * init_avi(FILE * in) {
	AVIContext * avi = malloc(sizeof(AVIContext));
	avi->avih = NULL;
	avi->stream = NULL;
	avi->index = NULL;
	avi->in = in;
	avi->riff = init_riff();
	avi->cur = 0;
	return avi;
}

void uninit_avi(AVIContext * avi) {
	if (!avi) return;

	uninit_riff(avi->riff);
	free(avi->stream);
	free(avi);
}

nut_stream_header_t * nut_create_stream_context(AVIContext * avi) {
	nut_stream_header_t * s;
	int i;
	s = malloc(sizeof(nut_stream_header_t) * (avi->avih->dwStreams + 1));
	for (i = 0; i < avi->avih->dwStreams; i++) {
		s[i].type = avi->stream[i].type;
		s[i].time_base_denom = avi->stream[i].strh->dwRate;
		s[i].time_base_nom = avi->stream[i].strh->dwScale;
		s[i].fixed_fps = 1;
		s[i].codec_specific_len = avi->stream[i].extra_len;
		s[i].codec_specific = avi->stream[i].extra;
		if (avi->stream[i].type == 0) { // video
			s[i].fourcc_len = 4;
			s[i].fourcc = avi->stream[i].video->biCompression;

			s[i].width = avi->stream[i].video->biWidth;
			s[i].height = avi->stream[i].video->biHeight;
			s[i].sample_width = 0;
			s[i].sample_height = 0;
			s[i].colorspace_type = 0;
		} else { // audio
			s[i].fourcc_len = 2;
			s[i].fourcc = avi->stream[i].audio->wFormatTag;

			s[i].samplerate_nom = 1;
			s[i].samplerate_denom = avi->stream[i].audio->nSamplesPerSec;
			s[i].channel_count = avi->stream[i].audio->nChannels;
		}
	}
	s[i].type = -1;
	return s;
}

int find_frame_type(FILE * in, int len, int * type) {
	uint8_t buf[len];
	int i;
	FREAD(in, len, buf);
	fseek(in, -len, SEEK_CUR);
	for (i = 0; i < len; i++) {
		if (buf[i] != 0xB6) continue;

		if (i == len - 1) return 11;
		*type = buf[i+1] >> 6;
		return 0;
	}
	return 13;
}

int get_avi_packet(AVIContext * avi, nut_packet_t * p) {
	char fourcc[4];
	int err = 0;
	int s; // stream
	uint32_t len;
	if (ftell(avi->in) & 1) fgetc(avi->in);

	if (avi->cur >= avi->packets) return -1;

	FREAD(avi->in, 4, fourcc);
	FREAD(avi->in, 4, &len);
	FIXENDIAN32(len);
	p->next_pts = 0;
	p->len = len;
	p->is_key = !!(avi->index[avi->cur++].dwFlags & 0x10);
	p->stream = s = (fourcc[0] - '0') * 10 + (fourcc[1] - '0');
	if (s == 0) { // 1 frame of video
		int type;
		p->pts = avi->stream[0].last_pts++; // FIXME
		if ((err = find_frame_type(avi->in, len, &type))) return err;
		if (stats) fprintf(stats, "%c", type==0?'I':type==1?'P':type==2?'B':'S');
		switch (type) {
			case 0: // I
				if (!p->is_key) printf("Error detected stream %d frame %d\n", s, p->pts);
				p->is_key = 1;
				break;
			case 1: { // P
				off_t where = ftell(avi->in);
				while (fourcc[0] != 'i') {
					len += len & 1; // round up
					fseek(avi->in, len, SEEK_CUR);
					FREAD(avi->in, 4, fourcc);
					FREAD(avi->in, 4, &len);
					FIXENDIAN32(len);
					if ((fourcc[0] - '0') * 10 + (fourcc[1] - '0') != 0) continue;
					if ((err = find_frame_type(avi->in, len, &type))) goto err_out;
					if (type != 2) break;
					p->pts++;
				}
				fseek(avi->in, where, SEEK_SET);
				break;
			}
			case 2: // B
				p->pts--;
				break;
			case 3: // S
				if (type == 3) printf("S-Frame %d\n", (int)ftell(avi->in));
				err = 12;
				goto err_out;
		}
	} else if (s < avi->avih->dwStreams) { // 0.5 secs of audio or a single packet
		int samplesize = avi->stream[s].strh->dwSampleSize;

		if (!p->is_key) printf("Error detected stream %d frame %d\n", s, p->pts);
		p->is_key = 1;

		p->pts = avi->stream[s].last_pts;
		if (samplesize) avi->stream[s].last_pts += p->len / samplesize;
		else avi->stream[s].last_pts++;
	} else {
		printf("%d %4.4s\n", avi->cur, fourcc);
		err = 10;
		goto err_out;
	}
err_out:
	return err;
}

#ifdef AVI_PROG

FILE * stats = NULL;

int main(int argc, char * argv []) {
	FILE * in;
	AVIContext * avi = NULL;
	int err = 0;
	int i;
	if (argc < 2) { printf("bleh, more params you fool...\n"); return 1; }

	in = fopen(argv[1], "r");
	avi = init_avi(in);

	if ((err = avi_read_headers(avi))) goto err_out;

	printf("Main AVI Header:\n");
	printf("dwMicroSecPerFrame: %u\n", avi->avih->dwMicroSecPerFrame);
	printf("dwMaxBytesPerSec: %u\n", avi->avih->dwMaxBytesPerSec);
	printf("dwReserved1: %u\n", avi->avih->dwReserved1);
	printf("dwFlags: %u\n", avi->avih->dwFlags);
	printf("dwTotalFrames: %u\n", avi->avih->dwTotalFrames);
	printf("dwInitialFrames: %u\n", avi->avih->dwInitialFrames);
	printf("dwStreams: %u\n", avi->avih->dwStreams);
	printf("dwSuggestedBufferSize: %u\n", avi->avih->dwSuggestedBufferSize);
	printf("dwWidth: %u\n", avi->avih->dwWidth);
	printf("dwHeight: %u\n", avi->avih->dwHeight);
	printf("dwScale: %u\n", avi->avih->dwScale);
	printf("dwRate: %u\n", avi->avih->dwRate);
	printf("dwStart: %u\n", avi->avih->dwStart);
	printf("dwLength: %u\n", avi->avih->dwLength);

	for (i = 0; i < avi->avih->dwStreams; i++) {
		printf("\n");
		printf("Stream header number %d\n", i);

		printf(" fccType: %.4s\n", avi->stream[i].strh->fccType);
		printf(" fccHandler: %.4s\n", avi->stream[i].strh->fccHandler);

		printf(" dwFlags: %u\n", avi->stream[i].strh->dwFlags);
		printf(" dwReserved1: %u\n", avi->stream[i].strh->dwReserved1);
		printf(" dwInitialFrames: %u\n", avi->stream[i].strh->dwInitialFrames);
		printf(" dwScale: %u\n", avi->stream[i].strh->dwScale);
		printf(" dwRate: %u\n", avi->stream[i].strh->dwRate);
		printf(" dwStart: %u\n", avi->stream[i].strh->dwStart);
		printf(" dwLength: %u\n", avi->stream[i].strh->dwLength);
		printf(" dwSuggestedBufferSize: %u\n", avi->stream[i].strh->dwSuggestedBufferSize);
		printf(" dwQuality: %u\n", avi->stream[i].strh->dwQuality);
		printf(" dwSampleSize: %u\n", avi->stream[i].strh->dwSampleSize);

		printf(" rcframe: %u %u %u %u\n",
			avi->stream[i].strh->rcframe[0], avi->stream[i].strh->rcframe[1],
			avi->stream[i].strh->rcframe[2], avi->stream[i].strh->rcframe[3]);

		if (avi->stream[i].type == 0) { // video
			printf(" video:\n");
			printf("  biSize: %u\n", avi->stream[i].video->biSize);
			printf("  biWidth: %u\n", avi->stream[i].video->biWidth);
			printf("  biHeight: %u\n", avi->stream[i].video->biHeight);
			printf("  biPlanes: %u\n", avi->stream[i].video->biPlanes);
			printf("  biBitCount: %u\n", avi->stream[i].video->biBitCount);

			printf("  biCompression: %.4s\n", avi->stream[i].video->biCompression);

			printf("  biSizeImage: %u\n", avi->stream[i].video->biSizeImage);
			printf("  biXPelsPerMeter: %u\n", avi->stream[i].video->biXPelsPerMeter);
			printf("  biYPelsPerMeter: %u\n", avi->stream[i].video->biYPelsPerMeter);
			printf("  biClrUsed: %u\n", avi->stream[i].video->biClrUsed);
			printf("  biClrImportant: %u\n", avi->stream[i].video->biClrImportant);
		} else {
			printf(" audio:\n");
			printf("  wFormatTag: 0x%04X\n", *(uint16_t*)avi->stream[i].audio->wFormatTag);
			printf("  nChannels: %u\n", avi->stream[i].audio->nChannels);
			printf("  nSamplesPerSec: %u\n", avi->stream[i].audio->nSamplesPerSec);
			printf("  nAvgBytesPerSec: %u\n", avi->stream[i].audio->nAvgBytesPerSec);
			printf("  nBlockAlign: %u\n", avi->stream[i].audio->nBlockAlign);
			printf("  wBitsPerSample: %u\n", avi->stream[i].audio->wBitsPerSample);
			printf("  cbSize: %u\n", avi->stream[i].audio->cbSize);
		}
	}

err_out:
	uninit_avi(avi);
	fclose(in);
	return err;
}

#endif
