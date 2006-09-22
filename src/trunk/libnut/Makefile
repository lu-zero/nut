include ../config.mak

all: libnut.a

libnut.a: muxer.o demuxer.o reorder.o
	rm -f $@
	${AR} rc $@ $^
	$(RANLIB) $@

libnut.so: muxer.o demuxer.o reorder.o
	${CC} ${CFLAGS} -shared $^ -o $@

.c.o: priv.h nut.h
	${CC} ${CFLAGS} $^ -c -o $@

clean:
	rm -f *\~ libnut.so libnut.a *.o
