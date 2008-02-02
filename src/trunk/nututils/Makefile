include ../config.mak

CFLAGS += -I../libnut

all: nutmerge nutindex nutparse

nutmerge: demux_ogg.c demux_avi.c framer_vorbis.c framer_mp3.c framer_mpeg4.c nutmerge.c ../libnut/libnut.a # demux_nut.c
	$(CC) $(CFLAGS) $^ -o $@

%: %.c
	$(CC) $(CFLAGS) $^ -o $@

nutindex nutmerge nutparse: nutmerge.h

clean:
	rm -f *\~ nutmerge nutindex nutparse *.o

.PHONY: all clean
