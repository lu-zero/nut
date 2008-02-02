include ../config.mak

CFLAGS += -I../libnut -lm

all: avireader nutmerge nutindex nutparse # oggreader

%reader: demux_%.c
	$(CC) $(CFLAGS) -DSTANDALONE $^ -o $@

nutmerge: demux_ogg.c demux_avi.c framer_vorbis.c framer_mp3.c framer_mpeg4.c nutmerge.c ../libnut/libnut.a # demux_nut.c
	$(CC) $(CFLAGS) $^ -o $@

%: %.c
	$(CC) $(CFLAGS) $^ -o $@

avireader nutindex nutmerge nutparse oggreader: nutmerge.h

clean:
	rm -f *\~ nutmerge nutindex nutparse avireader oggreader *.o

.PHONY: all clean
