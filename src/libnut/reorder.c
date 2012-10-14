// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "libnut.h"
#include "priv.h"

static void shift_frames(nut_context_tt * nut, stream_context_tt * s, int amount) {
	int i;
	assert(amount <= s->num_packets);
	for (i = 0; i < amount; i++) {
		nut_write_frame(nut, &s->packets[i].p, s->packets[i].buf);
		nut->alloc->free(s->packets[i].buf); // FIXME
	}
	if (s->next_pts != -2) s->next_pts = s->packets[i - 1].p.next_pts;
	s->num_packets -= amount;

	memmove(s->packets, s->packets + amount, s->num_packets * sizeof(reorder_packet_tt));
	s->packets = nut->alloc->realloc(s->packets, s->num_packets * sizeof(reorder_packet_tt));
}

static void flushcheck_frames(nut_context_tt * nut) {
	int change, i;
	for (i = 0; i < nut->stream_count; i++) {
		// check if any streams are missing essential info
		if (!nut->sc[i].num_packets && !nut->sc[i].next_pts) return;
	}
	do {
		change = 0;
		for (i = 0; i < nut->stream_count; i++) {
			int j;
			int64_t min = -1;
			if (!nut->sc[i].num_packets) continue; // no packets pending in this stream
			for (j = 0; j < nut->stream_count; j++) {
				int64_t pts;
				if (i == j) continue;

				if (nut->sc[j].num_packets) pts = nut->sc[j].packets[0].dts; // CANT USE p.pts;
				else pts = nut->sc[j].next_pts;

				if (pts >= 0) {
					pts = convert_ts(pts, TO_TB(j), TO_TB(i));
					if (min > pts || min == -1) min = pts;
				}
			}
			// MN rule, (i < j) && (i.dts <= j.pts)
			// actually, strict dts
			if (min == -1 || nut->sc[i].packets[0].dts <= min) {
				for (j = 1; j < nut->sc[i].num_packets; j++) {
					if (min != -1 && nut->sc[i].packets[j].dts > min) break;
				}
				shift_frames(nut, &nut->sc[i], j);
				change = 1;
			}
		}
	} while (change);
}

void nut_muxer_uninit_reorder(nut_context_tt * nut) {
	int i;
	if (!nut) return;

	for (i = 0; i < nut->stream_count; i++) nut->sc[i].next_pts = -2;

	flushcheck_frames(nut);
	for (i = 0; i < nut->stream_count; i++) {
		assert(!nut->sc[i].num_packets);
		nut->alloc->free(nut->sc[i].packets);
		nut->sc[i].packets = NULL;
	}
	nut_muxer_uninit(nut);
}

void nut_write_frame_reorder(nut_context_tt * nut, const nut_packet_tt * p, const uint8_t * buf) {
	stream_context_tt * s = &nut->sc[p->stream];
	if (nut->stream_count < 2) { // do nothing
		nut_write_frame(nut, p, buf);
		return;
	}

	s->num_packets++;
	s->packets = nut->alloc->realloc(s->packets, s->num_packets * sizeof(reorder_packet_tt));
	s->packets[s->num_packets - 1].p = *p;
	s->packets[s->num_packets - 1].dts = get_dts(s->sh.decode_delay, s->reorder_pts_cache, p->pts);

	s->packets[s->num_packets - 1].buf = nut->alloc->malloc(p->len); // FIXME
	memcpy(s->packets[s->num_packets - 1].buf, buf, p->len);

	flushcheck_frames(nut);
}
