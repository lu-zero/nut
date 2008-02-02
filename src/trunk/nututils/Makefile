include ../config.mak

CFLAGS += -I../libnut

all: nutmerge nutindex nutparse

nutmerge: demux_ogg.c demux_avi.c framer_vorbis.c framer_mp3.c framer_mpeg4.c nutmerge.c ../libnut/libnut.a nutmerge.h # demux_nut.c

%: %.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f *\~ nutmerge nutindex nutparse *.o

.PHONY: all clean
