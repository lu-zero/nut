include ../config.mak

CFLAGS += -I../libnut -lm

all: riffreader avireader nutmerge

riffreader: demux_avi.c
	${CC} ${CFLAGS} -DRIFF_PROG $^ -o $@
avireader: demux_avi.c
	${CC} ${CFLAGS} -DAVI_PROG $^ -o $@
nutmerge: demux_nut.c demux_avi.c nutmerge.c ../libnut/libnut.so
	${CC} ${CFLAGS} $^ -o $@

clean:
	rm -f *\~ nutmerge avireader riffreader *.o
