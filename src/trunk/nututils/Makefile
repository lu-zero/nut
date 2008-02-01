include ../config.mak

CFLAGS += -I../libnut -lm

all: avireader nutmerge nutindex nutparse # oggreader

avireader: demux_avi.c
	$(CC) $(CFLAGS) -DAVI_PROG $^ -o $@

oggreader: demux_ogg.c
	$(CC) $(CFLAGS) -DOGG_PROG $^ -o $@

nutmerge: demux_ogg.c demux_avi.c framer_vorbis.c framer_mp3.c framer_mpeg4.c nutmerge.c ../libnut/libnut.a # demux_nut.c
	$(CC) $(CFLAGS) $^ -o $@

nutindex: nutindex.c
	$(CC) $(CFLAGS) $^ -o $@

nutparse: nutparse.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f *\~ nutmerge nutindex nutparse avireader oggreader *.o

.PHONY: all clean
