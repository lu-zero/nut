CFLAGS += -I../libnut -lm

all: riffreader avireader nutmerge

riffreader: riffreader.c
	${CC} ${CFLAGS} -DRIFF_PROG $^ -o $@
avireader: riffreader.c avireader.c
	${CC} ${CFLAGS} -DAVI_PROG $^ -o $@
nutmerge: riffreader.c avireader.c nutmerge.c ../libnut/muxer.o ../libnut/reorder.o
	${CC} ${CFLAGS} $^ -o $@

clean:
	rm -f *\~ nutmerge avireader riffreader *.o
