#include <string.h>

#define mmioFOURCC(ch0, ch1, ch2, ch3) ((ch0) | ((ch1) << 8) | ((ch2) << 16) | ((ch3) << 24))
#define strFOURCC(str) mmioFOURCC((str)[0], (str)[1], (str)[2], (str)[3])

#ifdef WORDS_BIGENDIAN
#define FIXENDIAN32(a) do { \
	(a) = (((a) & 0xFF00FF00) >> 8)  | (((a) & 0x00FF00FF) << 8); \
	(a) = (((a) & 0xFFFF0000) >> 16) | (((a) & 0x0000FFFF) << 16); \
	} while(0)
#define FIXENDIAN16(a) \
	(a) = (((a) & 0xFF00) >> 8)  | (((a) & 0x00FF) << 8)
#else
#define FIXENDIAN32(a) do{}while(0)
#define FIXENDIAN16(a) do{}while(0)
#endif

typedef struct riff_tree_s {
	uint32_t len;
	char name[4];
	char listname[4];
	int type; // 0 - list/tree, 1 - node
	int amount; // if a list, amount of nodes
	struct riff_tree_s * tree; // this is an array (size is 'amount')
	char * data;
	int offset;
} riff_tree_t;

typedef struct {
	int amount;
	riff_tree_t * tree;
} full_riff_tree_t;

typedef struct  __attribute__((packed)) {
	uint8_t wFormatTag[2];
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WAVEFORMATEX;

typedef struct  __attribute__((packed)) {
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
} BITMAPINFOHEADER;

typedef struct  __attribute__((packed)) {
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
} MainAVIHeader;

typedef struct  __attribute__((packed)) {
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
} AVIStreamHeader;

typedef struct __attribute__((packed)) {
	uint8_t ckid[4];
	uint32_t dwFlags;
	uint32_t dwChunkOffset;
	uint32_t dwChunkLength;
} AVIINDEXENTRY;

typedef struct {
	int type; // 0 video, 1 audio
	AVIStreamHeader * strh; // these are all pointers to data
	BITMAPINFOHEADER * video;
	WAVEFORMATEX * audio;
	int extra_len;
	int last_pts;
	uint8_t * extra;
} AVIStreamContext;

typedef struct {
	full_riff_tree_t * riff;
	MainAVIHeader * avih;
	AVIStreamContext * stream; // this is an array, free this
	AVIINDEXENTRY * index; // this is an array and data
	int packets;
	FILE * in;
	int cur;
} AVIContext;

full_riff_tree_t * init_riff();
int get_full_riff_tree(FILE * in, full_riff_tree_t * full);
void uninit_riff(full_riff_tree_t * full);

int avi_read_headers(AVIContext * avi);
AVIContext * init_avi(FILE * in);
void uninit_avi(AVIContext * avi);
nut_stream_header_t * nut_create_stream_context(AVIContext * avi);
int get_avi_packet(AVIContext * avi, nut_packet_t * p);
