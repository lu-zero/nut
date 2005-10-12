CFLAGS += -I../libnut -lm

all: riffreader avireader nutmerge

riffreader: avireader.c
	${CC} ${CFLAGS} -DRIFF_PROG $^ -o $@
avireader: avireader.c
	${CC} ${CFLAGS} -DAVI_PROG $^ -o $@
nutmerge: avireader.c nutmerge.c ../libnut/muxer.o ../libnut/reorder.o
	${CC} ${CFLAGS} $^ -o $@

clean:
	rm -f *\~ nutmerge avireader riffreader *.o
