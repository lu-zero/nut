include ../config.mak

CFLAGS += -I../libnut -lm

all: riffreader avireader nutmerge nutindex # oggreader

riffreader: demux_avi.c
	${CC} ${CFLAGS} -DRIFF_PROG $^ -o $@
avireader: demux_avi.c
	${CC} ${CFLAGS} -DAVI_PROG $^ -o $@
oggreader: demux_ogg.c
	${CC} ${CFLAGS} -DOGG_PROG $^ -o $@
nutmerge: demux_ogg.c demux_avi.c framer_vorbis.c framer_mpeg4.c nutmerge.c ../libnut/libnut.so # demux_nut.c
	${CC} ${CFLAGS} $^ -o $@
nutindex: nutindex.c
	${CC} ${CFLAGS} $^ -o $@

clean:
	rm -f *\~ nutmerge nutindex avireader oggreader riffreader *.o
