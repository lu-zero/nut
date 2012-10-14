// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include "nutmerge.h"

struct framer_priv_s {
	stream_tt * stream;
};

static int get_packet(framer_priv_tt * mp, packet_tt * p) {
	static const int tabsel_123[2][3][16] = {
		{ {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0},
		  {0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0},
		  {0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,0} },
		{ {0,32,48,56, 64, 80, 96,112,128,144,160,176,192,224,256,0},
		  {0, 8,16,24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160,0},
		  {0, 8,16,24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160,0} }
	};
	static const int freqs[9] = { 44100, 48000, 32000,  // MPEG 1.0
	                              22050, 24000, 16000,  // MPEG 2.0
	                              11025, 12000,  8000}; // MPEG 2.5
	static const int mult[3] = { 12000, 144000, 144000 };
	int lsf,framesize,padding,freq,bitrate,layer;
	uint32_t newhead;
	int err;

	if ((err = get_stream_packet(mp->stream, p))) return err;
	if (p->p.len < 4) return err_mp3_bad_packet;

	newhead = p->buf[0]<<24 | p->buf[1]<<16 | p->buf[2]<<8 | p->buf[3];

	if ((newhead & 0xffe00000) != 0xffe00000) return err_mp3_bad_packet;

	layer   = 4-((newhead>>17)&0x3); // valid: 1..3
	freq    =    (newhead>>10)&0x3;  // valid: 0..2
	bitrate =    (newhead>>12)&0xf;  // valid: 1..14
	padding =    (newhead>> 9)&0x1;

	if (layer==4) return err_mp3_bad_packet;
	if (freq==3)  return err_mp3_bad_packet;

	//>>19 & 0x3
	if (newhead & (1<<20)) { // MPEG 1.0 (lsf==0) or MPEG 2.0 (lsf==1)
	  lsf = (newhead & (1<<19)) ? 0 : 1;
	  if (lsf) freq += 3;
	} else { // MPEG 2.5
	  lsf = 1;
	  freq += 6;
	}

	bitrate = tabsel_123[lsf][layer-1][bitrate];
	framesize = bitrate * mult[layer-1];

	if(!framesize) return err_mp3_bad_packet;

	framesize /= (layer == 3 ? (freqs[freq] << lsf) : freqs[freq]);
	framesize += padding;
	if(layer==1) framesize *= 4;

	if (p->p.len != framesize) return err_mp3_bad_packet;

	return 0;
}

static int setup_headers(framer_priv_tt * mp, nut_stream_header_tt * s) {
	*s = mp->stream->sh;
	return 0; // nothing to do
}

static framer_priv_tt * init(stream_tt * s) {
	framer_priv_tt * mp = malloc(sizeof(framer_priv_tt));
	mp->stream = s;
	return mp;
}

static void uninit(framer_priv_tt * mp) {
	free(mp);
}

framer_tt mp3_framer = {
	e_mp3,
	init,
	setup_headers,
	get_packet,
	uninit,
	NULL
};
