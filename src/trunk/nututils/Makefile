include ../config.mak

CFLAGS += -I../libnut

all: nutmerge nutindex nutparse

nutmerge: demux_avi.c demux_ogg.c framer_mp3.c framer_mpeg4.c framer_vorbis.c nutmerge.h ../libnut/libnut.a # demux_nut.c

clean:
	rm -f *\~ nutmerge nutindex nutparse *.o

.PHONY: all clean
