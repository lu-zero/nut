include ../config.mak

CFLAGS += -I../libnut -lm

all: riffreader avireader oggreader nutmerge nutindex

riffreader: demux_avi.c
	${CC} ${CFLAGS} -DRIFF_PROG $^ -o $@
avireader: demux_avi.c
	${CC} ${CFLAGS} -DAVI_PROG $^ -o $@
oggreader: demux_ogg.c
	${CC} ${CFLAGS} -DOGG_PROG $^ -o $@
nutmerge: demux_ogg.c demux_nut.c demux_avi.c nutmerge.c ../libnut/libnut.so
	${CC} ${CFLAGS} $^ -o $@
nutindex: nutindex.c
	${CC} ${CFLAGS} $^ -o $@

clean:
	rm -f *\~ nutmerge avireader oggreader riffreader *.o
